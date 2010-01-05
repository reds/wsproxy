CC = gcc
CFLAGS = -I/usr/local/include -O2 -g
LDFLAGS = -L/usr/local/lib -levent

all: wsproxy

clean:
	rm -f wsproxy.o wsproxy

	
