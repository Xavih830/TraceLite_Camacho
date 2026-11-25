#ifndef FUENTE_H
#define FUENTE_H

#include <stdint.h>

/* Interfaz común para fuentes de observación (/proc y eBPF) */

typedef struct {
    int           n_procs;
    int           n_vivos;
    double        cpu_ms;
    int64_t       ultima_actividad_ns;
    char          atribucion[12];
    unsigned long perdidos;
} medida_t;

typedef struct {
    int  (*abrir)(void);
    int  (*iniciar)(int pid_programa, int pid_app, int solicitud);
    int  (*iniciar_conn)(int puerto_cliente, int puerto_backend, int solicitud);
    int  (*leer)(medida_t *m);
    void (*detener)(void);
    void (*cerrar)(void);
} fuente_t;

#endif /* FUENTE_H */
