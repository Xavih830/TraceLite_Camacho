#ifndef SPAN_H
#define SPAN_H

#include <stdint.h>

/* Estructura de un span de trazabilidad (<= 512 bytes) */
typedef struct {
    char    trace_id[33];       /* 32 hex + '\0' */
    char    span_id[17];        /* 16 hex + '\0' */
    char    parent[17];         /* "" si es raiz */
    char    servicio[24];
    char    operacion[48];
    int64_t ini;                /* Inicio monotónico en ns */
    int64_t fin;                /* Fin monotónico en ns */
    double  dur;                /* Duración en ms */
    int     estado;             /* 0 = OK, 1 = ERROR */
    char    msg[64];            /* Detalle o mensaje de error */
    char    anomalia[24];       /* "", "latencia_alta", "error_proceso" */
    int     pid;
} span_t;

#endif /* SPAN_H */
