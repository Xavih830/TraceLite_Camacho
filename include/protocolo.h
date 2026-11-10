#ifndef PROTOCOLO_H
#define PROTOCOLO_H

/*
 * Mensajes de tamaño fijo intercambiados por pipes entre procesos.
 * anomalia: 0 = ninguna, 1 = latencia alta (+300 ms), 2 = error simulado.
 */
typedef struct { int id; char ruta[24]; int producto; int cantidad; int anomalia; } req_t;
typedef struct { char trace_id[33]; char parent[17]; char op[24];
                 int producto; int cantidad; int anomalia; } ctx_t;   /* contexto propagado */
typedef struct { int estado; char msg[64]; } resp_t;

#endif /* PROTOCOLO_H */
