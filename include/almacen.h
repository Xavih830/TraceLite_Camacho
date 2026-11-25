#ifndef ALMACEN_H
#define ALMACEN_H

#include "span.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* Almacén diario JSONL con índice en memoria, retención y consulta de historial */

#define MAX_ALMACEN_SPANS 100000

typedef struct {
    char    tipo[16];
    char    modo[16];
    char    destino[16];
    char    tiempo[16];
    int     plazo_s;
    int     solicitud;
    int64_t ini_real_ns;
    int64_t fin_real_ns;
} almacen_meta_t;

typedef struct {
    almacen_meta_t meta;
    span_t         span;
} almacen_registro_t;

typedef struct {
    almacen_registro_t *regs;
    int                 n;
    int                 cap;
    int                 corruptas;
    char                dir[PATH_MAX];
} almacen_indice_t;

int  almacen_resolver_dir(char *out, size_t out_sz);
int  almacen_cargar(almacen_indice_t *idx, const char *dir_opc, int ret_dias, int ret_seg);
int  almacen_guardar_span(almacen_indice_t *idx, const span_t *s, const almacen_meta_t *meta);
void almacen_aplicar_retencion(almacen_indice_t *idx, int ret_dias, int ret_seg);
void almacen_liberar(almacen_indice_t *idx);
int  almacen_mostrar_historial(const almacen_indice_t *idx, const char *trace_id_filtro);

#endif /* ALMACEN_H */
