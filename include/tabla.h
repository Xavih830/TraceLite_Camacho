#ifndef TABLA_H
#define TABLA_H

#include "span.h"

/* Impresión reutilizable de trazas en árbol y resumen para colectores e historial */
int  imprimir_trazas_spans(const span_t *spans, int n_spans);
void imprimir_resumen_spans(const span_t *spans, int n_spans, int num_trazas);

#endif /* TABLA_H */
