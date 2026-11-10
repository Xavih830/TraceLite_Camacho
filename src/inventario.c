#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Servicio inventario.
 * Descriptores que conserva (recibidos por argv):
 *   argv[1] = pin[0]  (lectura de ctx_t desde api)
 *   argv[2] = pout[1] (escritura de resp_t hacia api)
 *   argv[3] = psp[1]  (escritura de span_t hacia collector)
 */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Uso: %s <pin_r> <pout_w> <psp_w>\n", argv[0]);
        return 1;
    }

    int pin_r  = atoi(argv[1]);
    int pout_w = atoi(argv[2]);
    int psp_w  = atoi(argv[3]);

    srand((unsigned int)((int)getpid() ^ (int)time(NULL)));

    /* Stock inicial según sección 3.3: productos 0-3 con 10, producto 4 con 5 */
    int stock[5] = {10, 10, 10, 10, 5};

    ctx_t ctx;
    int r;
    while ((r = leer_exacto(pin_r, &ctx, sizeof(ctx_t))) == 1) {
        const char *op_span = ctx.op;
        if (strcmp(ctx.op, "consultar") == 0) {
            op_span = "consultar_catalogo";
        } else if (strcmp(ctx.op, "reservar") == 0) {
            op_span = "reservar_stock";
        }

        span_t s;
        span_ini(&s, ctx.trace_id, ctx.parent, "inventario", op_span);

        /* Latencia base de inventario: 10-40 ms aleatorios */
        int espera_ms = 10 + (rand() % 31);
        const char *anomalia_str = "";
        if (ctx.anomalia == 1) {
            espera_ms += 300;
            anomalia_str = "latencia_alta";
        } else if (ctx.anomalia == 2) {
            anomalia_str = "error_simulado";
        }
        dormir_ms(espera_ms);

        resp_t resp;
        memset(&resp, 0, sizeof(resp));

        if (ctx.anomalia == 2) {
            resp.estado = 1;
            snprintf(resp.msg, sizeof(resp.msg), "error_simulado");
        } else if (strncmp(ctx.op, "consultar", 9) == 0) {
            resp.estado = 0;
            resp.msg[0] = '\0';
        } else if (strncmp(ctx.op, "reservar", 8) == 0) {
            if (ctx.producto < 0 || ctx.producto > 4 ||
                ctx.cantidad <= 0 || ctx.cantidad > stock[ctx.producto]) {
                resp.estado = 1;
                snprintf(resp.msg, sizeof(resp.msg), "stock_insuficiente");
            } else {
                stock[ctx.producto] -= ctx.cantidad;
                resp.estado = 0;
                resp.msg[0] = '\0';
            }
        } else {
            resp.estado = 1;
            snprintf(resp.msg, sizeof(resp.msg), "operacion_desconocida");
        }

        span_fin(&s, resp.estado, resp.msg, anomalia_str);

        if (escribir_exacto(psp_w, &s, sizeof(span_t)) != 1) {
            close(pin_r);
            close(pout_w);
            close(psp_w);
            return 1;
        }
        if (escribir_exacto(pout_w, &resp, sizeof(resp_t)) != 1) {
            close(pin_r);
            close(pout_w);
            close(psp_w);
            return 1;
        }
    }

    close(pin_r);
    close(pout_w);
    close(psp_w);
    return (r == 0) ? 0 : 1;
}
