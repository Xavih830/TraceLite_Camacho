#ifndef TABLA_H
#define TABLA_H

#include <stddef.h>
#include "span.h"

/* Impresión reutilizable de trazas en árbol, relación y resumen */
void tabla_formatear_relacion(const span_t *s, const char *destino,
                              const char *tipo, char *out, size_t out_sz);
int  imprimir_trazas_spans(const span_t *spans, int n_spans);
void imprimir_resumen_spans(const span_t *spans, int n_spans, int num_trazas);

#endif /* TABLA_H */
