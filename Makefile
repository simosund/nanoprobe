CFLAGS=-Wall -Werror -O2
LDFLAGS=-lpthread
CC=clang

all: nanoprobe

OBJS = nanoprobe_main.o nanoprobe_common.o
nanoprobe: $(OBJS) nanoprobe.h
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -rf *.o nanoprobe

install:
	cp nanoprobe /usr/local/bin/
