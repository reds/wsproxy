CC = gcc
CFLAGS = -O2 -g
LDFLAGS = -levent

wsproxy: wsproxy.o
	