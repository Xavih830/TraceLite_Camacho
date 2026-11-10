#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Servicio api.
 * Descriptores que conserva (recibidos por argv):
 *   argv[1] = preq[0] (lectura de req_t desde launcher)
 *   argv[2] = pin[1]  (escritura de ctx_t hacia inventario)
 *   argv[3] = pout[0] (lectura de resp_t desde inventario)
 *   argv[4] = psp[1]  (escritura de span_t hacia collector)
 */
int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Uso: %s <preq_r> <pin_w> <pout_r> <psp_w>\n", argv[0]);
        return 1;
    }

    int preq_r = atoi(argv[1]);
    int pin_w  = atoi(argv[2]);
    int pout_r = atoi(argv[3]);
    int psp_w  = atoi(argv[4]);

    req_t req;
    int r;
    while ((r = leer_exacto(preq_r, &req, sizeof(req_t))) == 1) {
        /* Span raíz de la petición (sin padre) */
        span_t s;
        span_ini(&s, NULL, NULL, "api", req.ruta);

        /* Trabajo propio de la API: 3-9 ms antes de invocar a inventario */
        dormir_ms(3 + (rand() % 7));

        if (strcmp(req.ruta, "GET /health") == 0) {
            span_fin(&s, 0, "", "");
            if (escribir_exacto(psp_w, &s, sizeof(span_t)) != 1) {
                close(preq_r);
                close(pin_w);
                close(pout_r);
                close(psp_w);
                return 1;
            }
            continue;
        }

        ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        snprintf(ctx.trace_id, sizeof(ctx.trace_id), "%s", s.trace_id);
        snprintf(ctx.parent, sizeof(ctx.parent), "%s", s.span_id);
        if (strcmp(req.ruta, "GET /products") == 0) {
            snprintf(ctx.op, sizeof(ctx.op), "consultar_catalogo");
        } else {
            snprintf(ctx.op, sizeof(ctx.op), "reservar_stock");
        }
        ctx.producto = req.producto;
        ctx.cantidad = req.cantidad;
        ctx.anomalia = req.anomalia;

        if (escribir_exacto(pin_w, &ctx, sizeof(ctx_t)) != 1) {
            span_fin(&s, 1, "inv: pipe_cerrado", "");
            escribir_exacto(psp_w, &s, sizeof(span_t));
            close(preq_r);
            close(pin_w);
            close(pout_r);
            close(psp_w);
            return 1;
        }

        resp_t resp;
        if (leer_exacto(pout_r, &resp, sizeof(resp_t)) != 1) {
            span_fin(&s, 1, "inv: eof_inesperado", "");
            escribir_exacto(psp_w, &s, sizeof(span_t));
            close(preq_r);
            close(pin_w);
            close(pout_r);
            close(psp_w);
            return 1;
        }

        if (resp.estado != 0) {
            char msg_err[64];
            snprintf(msg_err, sizeof(msg_err), "inv: %.40s", resp.msg);
            span_fin(&s, 1, msg_err, "");
        } else {
            span_fin(&s, 0, "", "");
        }

        if (escribir_exacto(psp_w, &s, sizeof(span_t)) != 1) {
            close(preq_r);
            close(pin_w);
            close(pout_r);
            close(psp_w);
            return 1;
        }
    }

    close(preq_r);
    close(pin_w);
    close(pout_r);
    close(psp_w);
    return (r == 0) ? 0 : 1;
}
