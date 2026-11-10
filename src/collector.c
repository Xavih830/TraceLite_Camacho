#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Imprime un span en el nivel de indentación indicado y luego
 * busca recursivamente a todos sus hijos dentro de la misma traza.
 */
static void imprimir_arbol(const span_t *spans, int n, int *impresos,
                           int idx, int nivel) {
    if (idx < 0 || idx >= n || impresos[idx] || nivel > 32) {
        return;
    }
    impresos[idx] = 1;

    const span_t *s = &spans[idx];
    if (nivel == 0) {
        printf("  ");
    } else {
        printf("  ");
        for (int i = 0; i < nivel; i++) {
            printf("   ");
        }
        printf("`- ");
    }

    const char *est_str = (s->estado == 0) ? "OK" : "ERROR";
    printf("%-10s %-20s %6.2f ms  pid %-4d %s",
           s->servicio, s->operacion, s->dur, s->pid, est_str);

    if (s->anomalia[0] != '\0') {
        printf("  <-- anomalia");
    }
    printf("\n");

    for (int i = 0; i < n; i++) {
        if (!impresos[i] &&
            strcmp(spans[i].trace_id, s->trace_id) == 0 &&
            s->span_id[0] != '\0' &&
            strcmp(spans[i].parent, s->span_id) == 0) {
            imprimir_arbol(spans, n, impresos, i, nivel + 1);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <fd_lectura>\n", argv[0]);
        return 1;
    }

    int fd = atoi(argv[1]);
    int cap = 16;
    int n = 0;
    span_t *spans = (span_t *)malloc((size_t)cap * sizeof(span_t));
    if (spans == NULL) {
        return 1;
    }

    span_t temp;
    int r;
    while ((r = leer_exacto(fd, &temp, sizeof(span_t))) == 1) {
        if (n >= cap) {
            cap *= 2;
            span_t *nuevo = (span_t *)realloc(spans, (size_t)cap * sizeof(span_t));
            if (nuevo == NULL) {
                free(spans);
                close(fd);
                return 1;
            }
            spans = nuevo;
        }
        spans[n++] = temp;
    }
    close(fd);

    if (r < 0) {
        fprintf(stderr, "[collector] error o lectura parcial en pipe de spans\n");
        free(spans);
        return 1;
    }

    int *impresos = NULL;
    int *traza_vista = NULL;
    if (n > 0) {
        impresos = (int *)malloc((size_t)n * sizeof(int));
        traza_vista = (int *)malloc((size_t)n * sizeof(int));
        if (impresos == NULL || traza_vista == NULL) {
            free(impresos);
            free(traza_vista);
            free(spans);
            return 1;
        }
        memset(impresos, 0, (size_t)n * sizeof(int));
        memset(traza_vista, 0, (size_t)n * sizeof(int));
    }

    int num_trazas = 0;
    for (int i = 0; i < n; i++) {
        if (traza_vista[i]) {
            continue;
        }
        const char *tid = spans[i].trace_id;
        for (int j = i; j < n; j++) {
            if (strcmp(spans[j].trace_id, tid) == 0) {
                traza_vista[j] = 1;
            }
        }

        num_trazas++;
        printf("TRACE %s\n", tid);

        /* 1. Imprimir spans raíz (parent vacío) y sus descendientes */
        for (int j = 0; j < n; j++) {
            if (!impresos[j] &&
                strcmp(spans[j].trace_id, tid) == 0 &&
                spans[j].parent[0] == '\0') {
                imprimir_arbol(spans, n, impresos, j, 0);
            }
        }

        /* 2. Si quedó algún span huérfano (su parent no existe), imprimirlo sin fallar */
        for (int j = 0; j < n; j++) {
            if (!impresos[j] && strcmp(spans[j].trace_id, tid) == 0) {
                imprimir_arbol(spans, n, impresos, j, 1);
            }
        }
    }

    printf("-- %d trazas, %d spans\n", num_trazas, n);

    free(impresos);
    free(traza_vista);
    free(spans);
    return 0;
}
