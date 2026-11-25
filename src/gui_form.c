#include "gui_form.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tabla estática de campos y constructor literal de argv[] para tracelite */

static const char *const OPS_FORMA[]   = { "Observación", "Instantánea", "Resumen", NULL };
static const char *const OPS_SINO[]    = { "No", "Sí", NULL };
static const char *const OPS_DESTINO[] = { "programa", "backend", "app", NULL };
static const char *const OPS_MODO[]    = { "respuesta", "estabilidad", "escalado", "fallo", NULL };
static const char *const OPS_TIEMPO[]  = { "normal", "alto", "muy-alto", NULL };
static const char *const OPS_VIVO_BK[] = { "Vivo", "Nuevo", NULL };
static const char *const OPS_VIVA_AP[] = { "Viva", "Nueva", NULL };
static const char *const OPS_FIN[]     = { "ninguno", "error", "senal", NULL };

static int siempre(const gui_form_valores_t *v) { (void)v; return 1; }
static int es_obs(const gui_form_valores_t *v) { return v->forma == FORMA_OBSERVACION; }
static int es_obs_durante(const gui_form_valores_t *v) {
    return v->forma == FORMA_OBSERVACION && v->obs_durante != 0;
}
static int no_es_obs(const gui_form_valores_t *v) { return v->forma != FORMA_OBSERVACION; }
static int es_prog(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_PROGRAMA;
}
static int es_bk_vivo(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_BACKEND && v->bk_nuevo == 0;
}
static int es_bk_nuevo(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_BACKEND && v->bk_nuevo != 0;
}
static int es_bk(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_BACKEND;
}
static int es_app_viva(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_APP && v->app_nueva == 0;
}
static int es_app_nueva(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_APP && v->app_nueva != 0;
}
static int es_app(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->destino == DEST_APP;
}
static int es_slo(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION &&
           (v->modo == MODO_FORM_RESPUESTA || v->modo == MODO_FORM_ESTABILIDAD);
}
static int es_entradas(const gui_form_valores_t *v) {
    return v->forma == FORMA_RESUMEN &&
           (v->modo == MODO_FORM_ESTABILIDAD || v->modo == MODO_FORM_ESCALADO);
}
static int es_tamanos(const gui_form_valores_t *v) {
    return v->forma == FORMA_RESUMEN && v->modo == MODO_FORM_ESCALADO;
}
static int es_fallo_en(const gui_form_valores_t *v) {
    return v->forma != FORMA_OBSERVACION && v->modo == MODO_FORM_FALLO;
}

static const campo_form_t TABLA_CAMPOS[] = {
    { C_FORMA,        "--resumen",        "Forma",             CAMPO_DROPDOWN, 0,     2,     1, NULL,      OPS_FORMA,   siempre },
    { C_OBS_OBJETIVO, "<pid|nombre>",     "PID o nombre",      CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_obs },
    { C_OBS_DURANTE,  "--resumen",        "Durante un tiempo", CAMPO_DROPDOWN, 0,     1,     1, NULL,      OPS_SINO,    es_obs },
    { C_OBS_DURACION, "-d",               "Duración (s)",      CAMPO_SPIN,     1,  3600,     5, NULL,      NULL,        es_obs_durante },
    { C_OBS_INTERVAL, "--interval",       "Intervalo (ms)",    CAMPO_SPIN,    50, 10000,   500, NULL,      NULL,        es_obs_durante },
    { C_DESTINO,      "<destino>",        "Destino",           CAMPO_DROPDOWN, 0,     2,     0, NULL,      OPS_DESTINO, no_es_obs },
    { C_MODO,         "--modo",           "Modo",              CAMPO_DROPDOWN, 0,     3,     0, NULL,      OPS_MODO,    no_es_obs },
    { C_TIEMPO,       "--tiempo",         "Tiempo",            CAMPO_DROPDOWN, 0,     2,     0, NULL,      OPS_TIEMPO,  no_es_obs },
    { C_PROG_BIN,     "-- <prog>",        "Programa",          CAMPO_ENTRY,    0,     0,     0, "echo",    NULL,        es_prog },
    { C_PROG_ARG1,    "<arg1>",           "Argumento 1",       CAMPO_ENTRY,    0,     0,     0, "hola",    NULL,        es_prog },
    { C_PROG_ARG2,    "<arg2>",           "Argumento 2",       CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_prog },
    { C_PROG_ARG3,    "<arg3>",           "Argumento 3",       CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_prog },
    { C_BK_MODO,      "<bk_tipo>",        "Backend",           CAMPO_DROPDOWN, 0,     1,     0, NULL,      OPS_VIVO_BK, es_bk },
    { C_BK_ADDR,      "--backend",        "Dirección",         CAMPO_ENTRY,    0,     0,     0, "127.0.0.1:8080", NULL, es_bk_vivo },
    { C_BK_UNIDAD,    "--unidad",         "Unidad",            CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_bk_vivo },
    { C_BK_CMD,       "--lanzar-backend", "Comando backend",   CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_bk_nuevo },
    { C_BK_RUTA,      "--ruta",           "Ruta HTTP",         CAMPO_ENTRY,    0,     0,     0, "/proceso", NULL,       es_bk },
    { C_APP_MODO,     "<app_tipo>",       "App",               CAMPO_DROPDOWN, 0,     1,     0, NULL,      OPS_VIVA_AP, es_app },
    { C_APP_TARGET,   "--app",            "Nombre o PID",      CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_app_viva },
    { C_APP_UNIDAD,   "--unidad",         "Unidad",            CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_app_viva },
    { C_APP_CMD,      "--lanzar-app",     "Comando app",       CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_app_nueva },
    { C_APP_ORDEN,    "-- <orden>",       "Orden",             CAMPO_ENTRY,    0,     0,     0, "op",      NULL,        es_app },
    { C_APP_FIN,      "--fin",            "Fin",               CAMPO_DROPDOWN, 0,     2,     0, NULL,      OPS_FIN,     es_app },
    { C_OPT_SLO_MS,   "--slo-ms",         "SLO (ms)",          CAMPO_SPIN,     0, 600000,    0, NULL,      NULL,        es_slo },
    { C_OPT_ENTRADAS, "--entradas",       "Entradas",          CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_entradas },
    { C_OPT_TAMANOS,  "--tamanos",        "Tamaños (MB)",      CAMPO_ENTRY,    0,     0,     0, "",        NULL,        es_tamanos },
    { C_OPT_FALLO_EN, "--fallo-en",       "Fallo en",          CAMPO_SPIN,     0, 10000,     0, NULL,      NULL,        es_fallo_en }
};

const campo_form_t *gui_form_tabla(int *out_n) {
    if (out_n != NULL) {
        *out_n = (int)(sizeof(TABLA_CAMPOS) / sizeof(TABLA_CAMPOS[0]));
    }
    return TABLA_CAMPOS;
}

void gui_form_valores_defecto(gui_form_valores_t *v) {
    memset(v, 0, sizeof(*v));
    v->forma           = FORMA_INSTANTANEA;
    v->destino         = DEST_PROGRAMA;
    v->modo            = MODO_FORM_RESPUESTA;
    v->tiempo          = 0;
    v->obs_durante     = 1;
    v->obs_duracion_s  = 5;
    v->obs_interval_ms = 500;
    snprintf(v->prog_bin, sizeof(v->prog_bin), "echo");
    snprintf(v->prog_arg1, sizeof(v->prog_arg1), "hola");
    snprintf(v->bk_addr, sizeof(v->bk_addr), "127.0.0.1:8080");
    snprintf(v->bk_ruta, sizeof(v->bk_ruta), "/proceso");
    snprintf(v->app_orden, sizeof(v->app_orden), "op");
}

static int push_arg(char ***arr, int *n, int *cap, const char *s) {
    if (s == NULL) return 0;
    if (*n + 2 >= *cap) {
        int ncap = (*cap == 0) ? 16 : (*cap * 2);
        char **tmp = realloc(*arr, (size_t)ncap * sizeof(char *));
        if (tmp == NULL) return -1;
        *arr = tmp;
        *cap = ncap;
    }
    (*arr)[*n] = strdup(s);
    if ((*arr)[*n] == NULL) return -1;
    (*n)++;
    (*arr)[*n] = NULL;
    return 0;
}

static int push_num(char ***arr, int *n, int *cap, int val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return push_arg(arr, n, cap, buf);
}

int gui_form_construir_argv(const gui_form_valores_t *v, const char *ruta_tracelite,
                            char ***out_argv, int *out_argc) {
    char **argv = NULL;
    int argc = 0;
    int cap = 0;

    if (push_arg(&argv, &argc, &cap, ruta_tracelite ? ruta_tracelite : "tracelite") < 0) goto err;

    if (v->forma == FORMA_OBSERVACION) {
        if (v->obs_durante != 0) {
            if (push_arg(&argv, &argc, &cap, "--resumen") < 0) goto err;
            if (push_arg(&argv, &argc, &cap, "-d") < 0) goto err;
            if (push_num(&argv, &argc, &cap, v->obs_duracion_s) < 0) goto err;
            if (push_arg(&argv, &argc, &cap, "--interval") < 0) goto err;
            if (push_num(&argv, &argc, &cap, v->obs_interval_ms) < 0) goto err;
        }
        if (v->obs_objetivo[0] != '\0') {
            if (push_arg(&argv, &argc, &cap, v->obs_objetivo) < 0) goto err;
        }
        *out_argv = argv;
        *out_argc = argc;
        return 0;
    }

    if (v->forma == FORMA_RESUMEN) {
        if (push_arg(&argv, &argc, &cap, "--resumen") < 0) goto err;
    }
    if (push_arg(&argv, &argc, &cap, "--modo") < 0) goto err;
    if (push_arg(&argv, &argc, &cap, OPS_MODO[v->modo % 4]) < 0) goto err;
    if (push_arg(&argv, &argc, &cap, "--tiempo") < 0) goto err;
    if (push_arg(&argv, &argc, &cap, OPS_TIEMPO[v->tiempo % 3]) < 0) goto err;

    if (es_slo(v) && v->slo_ms > 0) {
        if (push_arg(&argv, &argc, &cap, "--slo-ms") < 0) goto err;
        if (push_num(&argv, &argc, &cap, v->slo_ms) < 0) goto err;
    }
    if (es_entradas(v) && v->entradas[0] != '\0') {
        if (push_arg(&argv, &argc, &cap, "--entradas") < 0) goto err;
        if (push_arg(&argv, &argc, &cap, v->entradas) < 0) goto err;
    }
    if (es_tamanos(v) && v->tamanos[0] != '\0') {
        if (push_arg(&argv, &argc, &cap, "--tamanos") < 0) goto err;
        if (push_arg(&argv, &argc, &cap, v->tamanos) < 0) goto err;
    }
    if (es_fallo_en(v) && v->fallo_en > 0) {
        if (push_arg(&argv, &argc, &cap, "--fallo-en") < 0) goto err;
        if (push_num(&argv, &argc, &cap, v->fallo_en) < 0) goto err;
    }

    if (v->destino == DEST_PROGRAMA) {
        if (push_arg(&argv, &argc, &cap, "--") < 0) goto err;
        if (v->prog_bin[0] != '\0' && push_arg(&argv, &argc, &cap, v->prog_bin) < 0) goto err;
        if (v->prog_arg1[0] != '\0' && push_arg(&argv, &argc, &cap, v->prog_arg1) < 0) goto err;
        if (v->prog_arg2[0] != '\0' && push_arg(&argv, &argc, &cap, v->prog_arg2) < 0) goto err;
        if (v->prog_arg3[0] != '\0' && push_arg(&argv, &argc, &cap, v->prog_arg3) < 0) goto err;
    } else if (v->destino == DEST_BACKEND) {
        if (v->bk_nuevo == 0) {
            if (v->bk_addr[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--backend") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->bk_addr) < 0) goto err;
            }
            if (v->bk_unidad[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--unidad") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->bk_unidad) < 0) goto err;
            }
        } else {
            if (v->bk_cmd[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--lanzar-backend") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->bk_cmd) < 0) goto err;
            }
        }
        if (v->bk_ruta[0] != '\0') {
            if (push_arg(&argv, &argc, &cap, "--ruta") < 0) goto err;
            if (push_arg(&argv, &argc, &cap, v->bk_ruta) < 0) goto err;
        }
    } else if (v->destino == DEST_APP) {
        if (v->app_nueva == 0) {
            if (v->app_target[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--app") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->app_target) < 0) goto err;
            }
            if (v->app_unidad[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--unidad") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->app_unidad) < 0) goto err;
            }
        } else {
            if (v->app_cmd[0] != '\0') {
                if (push_arg(&argv, &argc, &cap, "--lanzar-app") < 0) goto err;
                if (push_arg(&argv, &argc, &cap, v->app_cmd) < 0) goto err;
            }
        }
        if (v->app_fin > 0) {
            if (push_arg(&argv, &argc, &cap, "--fin") < 0) goto err;
            if (push_arg(&argv, &argc, &cap, OPS_FIN[v->app_fin % 3]) < 0) goto err;
        }
        if (v->app_orden[0] != '\0') {
            if (push_arg(&argv, &argc, &cap, "--") < 0) goto err;
            if (push_arg(&argv, &argc, &cap, v->app_orden) < 0) goto err;
        }
    }

    *out_argv = argv;
    *out_argc = argc;
    return 0;

err:
    gui_form_liberar_argv(argv, argc);
    *out_argv = NULL;
    *out_argc = 0;
    return -1;
}

void gui_form_liberar_argv(char **argv, int argc) {
    if (argv == NULL) return;
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

void gui_form_formatear_preview(char *const *argv, int argc, char *buf, size_t buf_sz) {
    size_t pos = 0;
    if (buf_sz == 0) return;
    buf[0] = '\0';
    for (int i = 0; i < argc && pos + 4 < buf_sz; i++) {
        if (i > 0) buf[pos++] = ' ';
        const char *a = argv[i];
        int necesita_comillas = (a[0] == '\0' || strpbrk(a, " \t\n;'\"$`\\") != NULL);
        if (necesita_comillas && pos + 1 < buf_sz) buf[pos++] = '\'';
        for (size_t j = 0; a[j] != '\0' && pos + 2 < buf_sz; j++) {
            buf[pos++] = a[j];
        }
        if (necesita_comillas && pos + 1 < buf_sz) buf[pos++] = '\'';
        buf[pos] = '\0';
    }
}
