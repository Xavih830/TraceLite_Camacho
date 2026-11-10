#include "common.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Generador de spans de prueba (Fase 2).
 * Si se invoca sin argumentos, escribe 2 trazas completas (3 spans) en stdout (fd 1).
 * Si se invoca con "--huerfano", añade además un span cuyo padre no existe.
 */
int main(int argc, char *argv[]) {
    int incluir_huerfano = (argc > 1 && strcmp(argv[1], "--huerfano") == 0);

    /* Traza 1: api (raíz) + inventario (hijo) */
    span_t s1_raiz;
    span_ini(&s1_raiz, NULL, NULL, "api", "POST /buy");
    dormir_ms(5);

    span_t s1_hijo;
    span_ini(&s1_hijo, s1_raiz.trace_id, s1_raiz.span_id, "inventario", "reservar_stock");
    dormir_ms(15);
    span_fin(&s1_hijo, 0, "", "");
    span_fin(&s1_raiz, 0, "", "");

    /* Traza 2: api GET /health (1 solo span raíz) */
    span_t s2_raiz;
    span_ini(&s2_raiz, NULL, NULL, "api", "GET /health");
    dormir_ms(4);
    span_fin(&s2_raiz, 0, "", "");

    /* Escribir primero el hijo y luego la raíz para verificar reconstrucción del árbol */
    if (escribir_exacto(STDOUT_FILENO, &s1_hijo, sizeof(span_t)) != 1) return 1;
    if (escribir_exacto(STDOUT_FILENO, &s1_raiz, sizeof(span_t)) != 1) return 1;
    if (escribir_exacto(STDOUT_FILENO, &s2_raiz, sizeof(span_t)) != 1) return 1;

    if (incluir_huerfano) {
        span_t huerfano;
        span_ini(&huerfano, NULL, "deadbeefcafebabe", "inventario", "span_huerfano");
        dormir_ms(3);
        span_fin(&huerfano, 1, "padre_inexistente", "");
        if (escribir_exacto(STDOUT_FILENO, &huerfano, sizeof(span_t)) != 1) return 1;
    }

    return 0;
}
