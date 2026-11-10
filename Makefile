CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -Iinclude
BIN     = bin
COMUN   = src/common.c
all: $(BIN)/launcher $(BIN)/collector $(BIN)/api $(BIN)/inventario
$(BIN)/%: src/%.c $(COMUN)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@
test: all
	$(CC) $(CFLAGS) tests/test_span.c $(COMUN) -o $(BIN)/test_span && ./$(BIN)/test_span
clean:
	rm -rf $(BIN)
.PHONY: all test clean
