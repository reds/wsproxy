#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <sys/types.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <syslog.h>
#include <sys/wait.h>
#include <event.h>

char* localhost = "0.0.0.0";
short localport = 6001;
char* serverhost = "127.0.0.1";
short serverport = 6000;

unsigned char frame[2] = { 0, 255 };

// server has produced data, relay it to the websocket client
static void server_read ( struct bufferevent* be, void* arg ) {
  struct bufferevent* client = (struct bufferevent*) arg;
  char buf[1024 * 10];
  int n = bufferevent_read ( be, buf, sizeof buf );
  if ( n > 0 ) {
    bufferevent_write ( client, frame, 1 );      // start of frame
    bufferevent_write ( client, buf, n );
    bufferevent_write ( client, frame + 1, 1 );  // end of frame
    bufferevent_enable ( client, EV_WRITE );
  }
}

// remove the start of frame/end of frame bytes from the websocket data
static char* deframe ( char* buf, int l ) {
  if ( buf[0] != '\0' ) {
    // error
    return NULL;
  }
  
  int i = 0;
  while ( i < l && (unsigned char)buf[i++] != 255 );
  buf[i - 1] = '\0';
  return buf + 1;
}

// websocket client has produced data, deframe if an send to server
static void client_read ( struct bufferevent* be, void* arg ) {
  struct bufferevent* server = (struct bufferevent*) arg;
  char buf[1024 * 10];
  int n = bufferevent_read ( be, buf, sizeof buf );
  if ( n > 0 ) {
    char* p = buf;
    while ( ( p = deframe ( p, buf + n - p ) ) != NULL ) {
      bufferevent_write ( server, p, strlen ( p ) );
      bufferevent_enable ( server, EV_WRITE );
    }
  }
}

struct bufferevent* connect_to_server ( char* host, int port );
// handle the websocket handshake
void client_read_ws_handshake ( struct bufferevent* be, void* arg ) {
  // Examine the input buffer without reading from it.
  // Wait untill we have the full handshake to begin processing.
  int len = EVBUFFER_LENGTH ( EVBUFFER_INPUT ( be ) );
  char* data = EVBUFFER_DATA (  EVBUFFER_INPUT ( be ) );
  char* buf = malloc ( len + 1 );
  memcpy ( buf, data, len );
  buf[len] = '\0';

  // the spec http://tools.ietf.org/html/draft-hixie-thewebsocketprotocol-68 is very
  // strict regarding the handshake.
  
  char* endofheaders = strstr ( buf, "\r\n\r\n" );
  if ( endofheaders == NULL ) return;

  char* origin;
  char* eoo; // end of origin
  if ( ( origin = strstr ( buf, "Origin:" ) ) == NULL || 
       ( eoo = strchr ( origin, '\r' ) ) == NULL ) {
    // didn't get all of the websocket headers, read more
    return;
  }

  origin += 8;
  *eoo = '\0';
  char* strings[50];
  int n = 0;
  char* ptmp;
  char* p = strtok_r ( buf, " ", &ptmp );
  while ( n < 50 && p != NULL ) {
    strings[n++] = p;
    p = strtok_r ( NULL, " ", &ptmp );
  }
  if ( n < 7 || strncmp ( "GET", strings[0], 3 ) != 0 ||
       strncmp ( "HTTP/1.1\r\nUpgrade:", strings[2], 18 ) != 0 ||
       strncmp ( "WebSocket\r\nConnection:", strings[3], 22 ) != 0 ||
       strncmp ( "Upgrade\r\nHost:", strings[4], 15 ) != 0 ||
       strlen ( strings[5] ) < 9 ||
       strlen ( strings[6] ) < 4 ) {
    // error
  }
  
  // clear input recvq
  // rb_linebuf_donebuf ( &client->localClient->buf_recvq );
  
  char* host = strings[5];
  int l = strlen ( host );
  host[l - 9] = '\0';

  host = strdup ( host );  origin = strdup ( origin ); char* uri = strdup ( strings[1] );

  sprintf ( buf, 
	    "HTTP/1.1 101 Web Socket Protocol Handshake\r\n"
	    "Upgrade: WebSocket\r\n"
	    "Connection: Upgrade\r\n"
	    "WebSocket-Origin: %s\r\n"
	    "WebSocket-Location: ws://%s%s\r\n\r\n",
	    origin, host, uri );
  free ( host ); free ( origin ); free ( uri );

  // send websock handshake
  bufferevent_write ( be, buf, strlen ( buf ) );
  bufferevent_enable ( be, EV_WRITE );

  struct bufferevent* server = connect_to_server ( serverhost, serverport );
  be->cbarg = server;
  server->cbarg = be;

  // remove the handshake and headers from the input buffer
  int hs_len = buf - endofheaders + 4;
  bufferevent_read ( be, buf, hs_len );

  // reconfig the read callback 
  be->readcb = client_read;
  client_read ( be, server );
}

void be_error ( struct bufferevent* be, short type, void* arg ) {
  struct bufferevent* other = (struct bufferevent*) arg;
  close ( be->ev_read.ev_fd );
  close ( be->ev_write.ev_fd );
  bufferevent_free ( be );
  close ( other->ev_read.ev_fd );
  close ( other->ev_write.ev_fd );
  bufferevent_free ( other );
}

void be_write ( struct bufferevent* be, void* arg ) {
  bufferevent_disable ( be, EV_WRITE );
}

// the server has accepted a connection from the proxy
void server_connected ( struct bufferevent* be, void* arg ) {
  be->writecb = be_write;
  be->readcb = server_read;
  struct bufferevent* client = (struct bufferevent*) arg;
  char buf[1024 * 10];
  int n = bufferevent_read ( client, buf, sizeof buf );
  if ( n > 0 ) {
    bufferevent_write ( be, buf, n );
    bufferevent_enable ( be, EV_WRITE );
  }
}

struct bufferevent* connect_to_server ( char* host, int port ) {
  uint32_t ip;
  inet_pton ( AF_INET, host, &ip );
  
  struct sockaddr_in raddr;
  raddr.sin_addr.s_addr = ip;
  raddr.sin_port = htons ( port );
  raddr.sin_family = AF_INET;
  int fd = socket ( PF_INET, SOCK_STREAM, 0 );
  //  evutil_make_socket_nonblocking(fd);
  fcntl ( fd, F_SETFL, O_NONBLOCK );
  struct bufferevent* be = bufferevent_new ( fd, 
					     NULL,
					     server_connected,
					     be_error,
					     NULL );
  bufferevent_enable ( be, EV_READ|EV_WRITE );
  connect ( fd, (struct sockaddr*)&raddr, sizeof raddr );
  return be;
}

// wait for websocket connections
void listener_accept ( int fd, short type, void* arg ) {
  struct sockaddr_in raddr;
  int raddrlen = sizeof raddr;
  int sock = accept ( fd, (struct sockaddr*)&raddr, &raddrlen );
  if ( sock != -1 ) {
    struct bufferevent* client = bufferevent_new ( sock,
					      client_read_ws_handshake,
					      be_write,
					      be_error, NULL );
    bufferevent_enable ( client, EV_READ|EV_WRITE );
  }
}

main() {
  //  daemon ( 1, 0 );
  event_init();

  int sock = socket( AF_INET, SOCK_STREAM, 0 );
  (void) fcntl( sock, F_SETFD, 1 );
  int i = 1;
  if ( setsockopt( sock, SOL_SOCKET, SO_REUSEADDR, (void*) &i, sizeof(i) ) < 0 ) {
    exit ( -1 );
  }
  struct sockaddr_in server_addr;
  uint32_t ip;
  inet_pton ( AF_INET, localhost, &ip );

  server_addr.sin_addr.s_addr = ip;
  server_addr.sin_port = htons ( localport );
  server_addr.sin_family = AF_INET;

  if ( bind( sock, (struct sockaddr*)&server_addr, sizeof(struct sockaddr) ) < 0 ) {
    fprintf ( stderr, "bind\n" );
    exit ( -1 );
  }
  
  if ( listen( sock, 1024 ) < 0 ) {
    exit ( -1 );
  }
  struct event listen_read_event;
  event_set ( &listen_read_event, sock, EV_READ|EV_PERSIST, listener_accept, NULL );
  event_add ( &listen_read_event, NULL );
  
  event_dispatch();
}
