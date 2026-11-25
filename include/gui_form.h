#ifndef GUI_FORM_H
#define GUI_FORM_H

#include <stddef.h>

/* Formas, destinos, modos y tipos de campo del formulario G2 */

typedef enum {
    FORMA_OBSERVACION = 0,
    FORMA_INSTANTANEA = 1,
    FORMA_RESUMEN     = 2
} gui_forma_t;

typedef enum {
    DEST_PROGRAMA = 0,
    DEST_BACKEND  = 1,
    DEST_APP      = 2
} gui_destino_t;

typedef enum {
    MODO_FORM_RESPUESTA   = 0,
    MODO_FORM_ESTABILIDAD = 1,
    MODO_FORM_ESCALADO    = 2,
    MODO_FORM_FALLO       = 3
} gui_modo_t;

typedef enum {
    CAMPO_DROPDOWN = 0,
    CAMPO_SPIN     = 1,
    CAMPO_ENTRY    = 2
} gui_campo_tipo_t;

typedef enum {
    C_FORMA = 0,
    C_OBS_OBJETIVO,
    C_OBS_DURANTE,
    C_OBS_DURACION,
    C_OBS_INTERVAL,
    C_DESTINO,
    C_MODO,
    C_TIEMPO,
    C_PROG_BIN,
    C_PROG_ARG1,
    C_PROG_ARG2,
    C_PROG_ARG3,
    C_BK_MODO,
    C_BK_ADDR,
    C_BK_UNIDAD,
    C_BK_CMD,
    C_BK_RUTA,
    C_APP_MODO,
    C_APP_TARGET,
    C_APP_UNIDAD,
    C_APP_CMD,
    C_APP_ORDEN,
    C_APP_FIN,
    C_OPT_SLO_MS,
    C_OPT_ENTRADAS,
    C_OPT_TAMANOS,
    C_OPT_FALLO_EN,
    C_TOTAL
} gui_campo_id_t;

typedef struct {
    gui_forma_t   forma;
    gui_destino_t destino;
    gui_modo_t    modo;
    int           tiempo;      /* 0=normal, 1=alto, 2=muy-alto */
    int           obs_durante; /* 0=no, 1=si (--resumen) */
    int           obs_duracion_s;
    int           obs_interval_ms;
    int           bk_nuevo;    /* 0=vivo, 1=nuevo */
    int           app_nueva;   /* 0=viva, 1=nueva */
    int           app_fin;     /* 0=ninguno, 1=error, 2=senal */
    int           slo_ms;
    int           fallo_en;
    char          obs_objetivo[128];
    char          prog_bin[256];
    char          prog_arg1[256];
    char          prog_arg2[256];
    char          prog_arg3[256];
    char          bk_addr[128];
    char          bk_unidad[128];
    char          bk_cmd[256];
    char          bk_ruta[128];
    char          app_target[128];
    char          app_unidad[128];
    char          app_cmd[256];
    char          app_orden[128];
    char          entradas[64];
    char          tamanos[64];
} gui_form_valores_t;

typedef struct {
    gui_campo_id_t    id;
    const char       *opcion;
    const char       *etiqueta;
    gui_campo_tipo_t  tipo;
    int               min_val;
    int               max_val;
    int               def_val;
    const char       *def_str;
    const char *const *opciones;
    int (*aplica)(const gui_form_valores_t *v);
} campo_form_t;

const campo_form_t *gui_form_tabla(int *out_n);
void                gui_form_valores_defecto(gui_form_valores_t *v);
int                 gui_form_construir_argv(const gui_form_valores_t *v,
                                            const char *ruta_tracelite,
                                            char ***out_argv, int *out_argc);
void                gui_form_liberar_argv(char **argv, int argc);
void                gui_form_formatear_preview(char *const *argv, int argc,
                                               char *buf, size_t buf_sz);

#endif /* GUI_FORM_H */
