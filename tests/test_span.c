#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int es_hex_valido(const char *s, size_t largo_esperado) {
    if (strlen(s) != largo_esperado) {
        return 0;
    }
    for (size_t i = 0; i < largo_esperado; i++) {
        char c = s[i];
        int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    /* Verificación de tamaño de span_t (Sección 0.1: <= 512 bytes) */
    assert(sizeof(span_t) <= 512);

    /* 1. Span raíz: trace_id de 32 hex, span_id de 16 hex, parent vacío */
    span_t raiz;
    span_ini(&raiz, NULL, NULL, "api", "POST /buy");
    assert(es_hex_valido(raiz.trace_id, 32));
    assert(es_hex_valido(raiz.span_id, 16));
    assert(strcmp(raiz.parent, "") == 0);
    assert(strcmp(raiz.servicio, "api") == 0);
    assert(strcmp(raiz.operacion, "POST /buy") == 0);
    assert(raiz.pid > 0);

    /* 2. Span hijo: hereda trace_id y copia parent del contexto */
    span_t hijo;
    span_ini(&hijo, raiz.trace_id, raiz.span_id, "inventario", "reservar_stock");
    assert(es_hex_valido(hijo.trace_id, 32));
    assert(es_hex_valido(hijo.span_id, 16));
    assert(strcmp(hijo.trace_id, raiz.trace_id) == 0);
    assert(strcmp(hijo.parent, raiz.span_id) == 0);

    /* 3. Dos span_id consecutivos son distintos */
    assert(strcmp(raiz.span_id, hijo.span_id) != 0);

    /* 4. Medición de tiempo: dormir_ms(20) deja fin > ini y dur entre 19 y 60 ms */
    dormir_ms(20);
    span_fin(&hijo, 0, "", "");
    span_fin(&raiz, 0, "", "");

    assert(hijo.fin > hijo.ini);
    assert(hijo.dur >= 19.0 && hijo.dur <= 60.0);
    assert(raiz.fin > raiz.ini);
    assert(raiz.dur >= hijo.dur);

    /* 5. Prueba de E/S exacta por pipe anónimo */
    int p[2];
    assert(pipe(p) == 0);
    assert(escribir_exacto(p[1], &raiz, sizeof(span_t)) == 1);
    close(p[1]);

    span_t leido;
    assert(leer_exacto(p[0], &leido, sizeof(span_t)) == 1);
    assert(strcmp(leido.trace_id, raiz.trace_id) == 0);
    assert(strcmp(leido.span_id, raiz.span_id) == 0);
    assert(leer_exacto(p[0], &leido, sizeof(span_t)) == 0); /* EOF limpio */
    close(p[0]);

    printf("[OK] tests/test_span superado con exito (sizeof(span_t) = %zu bytes, dur = %.2f ms)\n",
           sizeof(span_t), hijo.dur);
    return 0;
}
