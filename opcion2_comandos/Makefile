CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -I../include
LDFLAGS = -pthread
COMUN   = ../src/common.c

all: collector_sock wrapper_traza

collector_sock: collector_sock.c $(COMUN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@ $(LDFLAGS)

wrapper_traza: wrapper_traza.c $(COMUN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@

run: all
	./wrapper_traza

clean:
	rm -f collector_sock wrapper_traza

.PHONY: all run clean

