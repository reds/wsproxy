CC = gcc
CFLAGS = -O2 -g
LDFLAGS = -levent

all: wsproxy

clean:
	rm -f wsproxy.o wsproxy

	