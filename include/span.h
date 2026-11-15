#ifndef SPAN_H
#define SPAN_H

#include <stdint.h>

/* Estructura de un span de trazabilidad */
typedef struct {
    char    trace_id[33];       
    char    span_id[17];        
    char    parent[17];         
    char    servicio[24];
    char    operacion[48];
    int64_t ini;                
    int64_t fin;                
    double  dur;                
    int     estado;             
    char    msg[64];            
    char    anomalia[24];       
    int     pid;
} span_t;

#endif /* SPAN_H */
