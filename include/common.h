#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include "span.h"
#include "protocolo.h"

int64_t ahora_ns(void);
void    dormir_ms(int ms);
int     escribir_exacto(int fd, const void *buf, size_t n);
int     leer_exacto(int fd, void *buf, size_t n);
void    id_hex(char *out, int bytes);
void    span_ini(span_t *s, const char *trace_id, const char *parent,
                 const char *servicio, const char *operacion);
void    span_fin(span_t *s, int estado, const char *msg, const char *anomalia);

#endif /* COMMON_H */
