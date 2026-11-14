CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -Iinclude
BIN     = bin
COMUN   = src/common.c

all: opcion2 opcion3

opcion2:
	$(MAKE) -C opcion2_comandos all

opcion3:
	$(MAKE) -C opcion3_procesos_vivos all

test:
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) tests/test_span.c $(COMUN) -o $(BIN)/test_span && ./$(BIN)/test_span

clean:
	rm -rf $(BIN)
	$(MAKE) -C opcion2_comandos clean
	$(MAKE) -C opcion3_procesos_vivos clean

.PHONY: all opcion2 opcion3 test clean
