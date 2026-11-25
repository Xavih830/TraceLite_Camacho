#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>

/* Mensajes de control entre tracelite, Servicio A y Servicio B por AF_UNIX */

typedef enum {
    TL_MSG_INICIAR = 1,
    TL_MSG_LISTO,
    TL_MSG_TERMINAR,
    TL_MSG_RESULTADO,
    TL_MSG_FIN,
    TL_MSG_ERROR
} msg_tipo_t;

typedef struct {
    int  tipo;
    int  solicitud;
    char trace_id[33];
    char parent_span[17];
    int  pid_programa;
    int  pid_app;
    int  puerto_cliente;
    int  estado_salida;
    char operacion[48];
} ctx_t;

typedef struct {
    int     tipo;
    int     solicitud;
    int64_t fin_ns;
    int     estado;
    char    msg[64];
} resp_t;

typedef enum {
    SOL_EJECUTAR = 1,
    SOL_FIN
} sol_tipo_t;

typedef struct {
    int  tipo;
    int  solicitud;
    char linea[512];
} sol_t;

typedef struct {
    int  tipo;
    int  solicitud;
    int  estado;
    char trace_id[33];
} sol_resp_t;

#endif /* PROTOCOLO_H */
