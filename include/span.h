#ifndef SPAN_H
#define SPAN_H

#include <stdint.h>

/*
 * Estructura fija de un span de trazabilidad.
 * Por la sección 0.1 del plan, se mantiene sizeof(span_t) <= 512 bytes
 * y se verifica con assert en tiempo de ejecución.
 */
typedef struct {
    char    trace_id[33];       /* 32 hex + '\0' */
    char    span_id[17];        /* 16 hex + '\0' */
    char    parent[17];         /* "" si es raíz */
    char    servicio[24];
    char    operacion[48];
    int64_t ini;                /* ns, CLOCK_MONOTONIC */
    int64_t fin;                /* ns */
    double  dur;                /* ms */
    int     estado;             /* 0 = OK, 1 = ERROR */
    char    msg[64];            /* mensaje de error o "" */
    char    anomalia[24];       /* "", "latencia_alta", "error_simulado" */
    int     pid;
} span_t;

#endif /* SPAN_H */
