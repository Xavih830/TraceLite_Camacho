CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread -lm
BIN     = bin
COMUN   = src/common.c

all: $(BIN)/tracelite $(BIN)/collector $(BIN)/monitor_pid

$(BIN)/collector: src/collector.c $(COMUN)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@ $(LDFLAGS)

$(BIN)/%: src/%.c $(COMUN)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@

run: all
	./$(BIN)/tracelite

test:
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) tests/test_span.c $(COMUN) -o $(BIN)/test_span && ./$(BIN)/test_span

clean:
	rm -rf $(BIN)

.PHONY: all run test clean
