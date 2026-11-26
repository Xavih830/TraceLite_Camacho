CC      = gcc
CFLAGS  = -std=gnu11 -Wall -Wextra -O2 -Iinclude
LDFLAGS = -pthread -lm
BIN     = bin
COMUN   = src/common.c
HDRS    = $(wildcard include/*.h)

all: $(BIN)/tracelite $(BIN)/collector $(BIN)/monitor_pid $(BIN)/servicio_a $(BIN)/servicio_b

$(BIN)/tracelite: src/tracelite.c src/solicitudes.c src/tabla.c src/almacen.c $(COMUN) $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/tracelite.c src/solicitudes.c src/tabla.c src/almacen.c $(COMUN) -o $@ $(LDFLAGS)

$(BIN)/collector: src/collector.c src/tabla.c src/almacen.c $(COMUN) $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/collector.c src/tabla.c src/almacen.c $(COMUN) -o $@ $(LDFLAGS)

$(BIN)/servicio_b: src/servicio_b.c src/fuente_proc.c $(COMUN) $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) src/servicio_b.c src/fuente_proc.c $(COMUN) -o $@ $(LDFLAGS)

$(BIN)/%: src/%.c $(COMUN) $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $< $(COMUN) -o $@

ebpf: all
	@mkdir -p $(BIN) src/bpf
	@if [ ! -f src/bpf/vmlinux.h ]; then bpftool btf dump file /sys/kernel/btf/vmlinux format c > src/bpf/vmlinux.h; fi
	clang -O2 -g -target bpf -D__TARGET_ARCH_x86 -Isrc/bpf -c src/bpf/trace.bpf.c -o $(BIN)/trace.bpf.o
	bpftool gen skeleton $(BIN)/trace.bpf.o > src/bpf/trace.skel.h
	$(CC) $(CFLAGS) -DHAVE_EBPF -Isrc/bpf src/servicio_b.c src/fuente_proc.c src/fuente_ebpf.c $(COMUN) -o $(BIN)/servicio_b $(LDFLAGS) -lbpf -lelf -lz

run: all
	./$(BIN)/tracelite

test: $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) tests/test_span.c $(COMUN) -o $(BIN)/test_span && ./$(BIN)/test_span
	$(CC) $(CFLAGS) tests/test_almacen.c src/almacen.c src/tabla.c $(COMUN) -o $(BIN)/test_almacen && ./$(BIN)/test_almacen

check: all test
	bash docs/pruebas/pruebas_vivos.sh

gui: $(HDRS) include/gui_form.h
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) $$(pkg-config --cflags gtk4) src/gui.c src/gui_form.c src/almacen.c src/tabla.c $(COMUN) -o $(BIN)/tracelite-gui $(LDFLAGS) $$(pkg-config --libs gtk4)

clean:
	rm -rf $(BIN)

.PHONY: all ebpf run test check clean
