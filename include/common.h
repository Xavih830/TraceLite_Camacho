#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include "span.h"

/* Reloj, esperas, E/S binaria por fd, IDs y ciclo de vida de spans */
int64_t ahora_ns(void);
void    dormir_ms(int ms);
int     escribir_exacto(int fd, const void *buf, size_t n);
int     leer_exacto(int fd, void *buf, size_t n);
void    id_hex(char *out, int bytes);
void    span_ini(span_t *s, const char *trace_id, const char *parent,
                 const char *servicio, const char *operacion);
void    span_fin(span_t *s, int estado, const char *msg, const char *anomalia);
int     ruta_hermana(const char *nombre, char *out, size_t out_sz);
int     crear_dir_privado(char *out, size_t out_sz);
int     validar_ruta_socket(const char *sock_path);
int     crear_servidor_unix(const char *sock_path, int backlog);
int     conectar_unix(const char *sock_path, int intentos, int espera_ms);
int     aceptar_unix(int server_fd, int timeout_ms);
void    nombre_base_cmd(const char *linea, char *out, size_t out_sz);
int     resolver_pid_puerto_tcp(int puerto);
int     resolver_pid_unidad(const char *unidad);
int     resolver_pid_nombre_proceso(const char *nombre);

#endif /* COMMON_H */
