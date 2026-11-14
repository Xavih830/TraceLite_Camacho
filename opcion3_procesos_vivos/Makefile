CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -I../include
LDFLAGS = -pthread
COMUN   = ../src/common.c

all: collector_sock monitor_pid orquestador_pids

collector_sock: collector_sock.c $(COMUN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@ $(LDFLAGS)

monitor_pid: monitor_pid.c $(COMUN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@

orquestador_pids: orquestador_pids.c $(COMUN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@

run: all
	./orquestador_pids

clean:
	rm -f collector_sock monitor_pid orquestador_pids

.PHONY: all run clean

