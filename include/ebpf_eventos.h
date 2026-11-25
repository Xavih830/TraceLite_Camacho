#ifndef EBPF_EVENTOS_H
#define EBPF_EVENTOS_H

#include <stdint.h>

/* Estructura compartida entre el programa eBPF y fuente_ebpf.c */

enum ebpf_ev_tipo {
    EV_FORK = 1,
    EV_EXEC = 2,
    EV_EXIT = 3
};

struct ebpf_evento {
    uint32_t tipo;
    uint32_t solicitud;
    uint32_t pid;
    uint32_t ppid;
    int32_t  exit_code;
    uint64_t ts_ns;
    char     comm[16];
};

#endif /* EBPF_EVENTOS_H */
