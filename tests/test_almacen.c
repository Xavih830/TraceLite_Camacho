#include "almacen.h"
#include "common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pruebas unitarias de almacen.c: escape JSON, línea truncada, recarga y retención */

int main(void) {
    char dir_tmpl[] = "/tmp/tl_almacen_XXXXXX";
    char *dir = mkdtemp(dir_tmpl);
    assert(dir != NULL);

    /* 1. Guardar span con comillas, barra invertida y caracteres de control */
    almacen_indice_t idx;
    assert(almacen_cargar(&idx, dir, 7, 0) == 0);

    span_t s;
    span_ini(&s, NULL, NULL, "servicio_a", "gzip");
    dormir_ms(5);
    span_fin(&s, 0, "cmd \"test\" \\ ruta\nfin", "");

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t real_ns = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;

    almacen_meta_t m;
    memset(&m, 0, sizeof(m));
    snprintf(m.tipo, sizeof(m.tipo), "solicitud");
    snprintf(m.modo, sizeof(m.modo), "respuesta");
    snprintf(m.destino, sizeof(m.destino), "programa");
    snprintf(m.tiempo, sizeof(m.tiempo), "normal");
    m.plazo_s = 10;
    m.solicitud = 1;
    m.ini_real_ns = real_ns - 5000000LL;
    m.fin_real_ns = real_ns;

    assert(almacen_guardar_span(&idx, &s, &m) == 0);
    almacen_liberar(&idx);

    /* 2. Escribir archivo antiguo (2020-01-01.jsonl), línea truncada y archivo ajeno */
    char ruta_vieja[512], ruta_ajena[512];
    snprintf(ruta_vieja, sizeof(ruta_vieja), "%s/2020-01-01.jsonl", dir);
    FILE *fv = fopen(ruta_vieja, "w");
    assert(fv != NULL);
    fprintf(fv, "{\"v\":1,\"tipo\":\"solicitud\",\"trace_id\":\"0123456789abcdef0123456789abcdef\","
                "\"span_id\":\"0123456789abcdef\",\"parent\":\"\",\"servicio\":\"servicio_a\","
                "\"operacion\":\"gzip\",\"ini_ns\":1,\"fin_ns\":2,\"ini_real_ns\":1000,"
                "\"fin_real_ns\":2000,\"dur_ms\":1.0,\"estado\":0}\n");
    fprintf(fv, "{\"v\":1,\"trace_id\":\"truncado_sin_cierre\n");
    fclose(fv);

    snprintf(ruta_ajena, sizeof(ruta_ajena), "%s/notas.txt", dir);
    FILE *fa = fopen(ruta_ajena, "w");
    assert(fa != NULL);
    fprintf(fa, "no tocar\n");
    fclose(fa);

    /* 3. Recargar con retención de 7 días: debe borrar 2020-01-01.jsonl y conservar notas.txt */
    almacen_indice_t idx2;
    assert(almacen_cargar(&idx2, dir, 7, 0) == 0);
    assert(idx2.n == 1);
    assert(idx2.corruptas == 1);
    assert(strcmp(idx2.regs[0].span.trace_id, s.trace_id) == 0);
    assert(strcmp(idx2.regs[0].span.msg, "cmd \"test\" \\ ruta\nfin") == 0);
    assert(access(ruta_vieja, F_OK) != 0);
    assert(access(ruta_ajena, F_OK) == 0);
    almacen_liberar(&idx2);

    /* Limpiar */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    assert(system(cmd) == 0);

    printf("[OK] tests/test_almacen superado con exito\n");
    return 0;
}
