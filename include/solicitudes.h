#ifndef SOLICITUDES_H
#define SOLICITUDES_H

#include <signal.h>

/* Configuración y punto de entrada de Collector Lite para solicitudes A/B */

typedef enum {
    DEST_PROGRAMA = 0,
    DEST_BACKEND,
    DEST_APP
} destino_t;

typedef enum {
    MODO_RESPUESTA = 0,
    MODO_ESTABILIDAD,
    MODO_ESCALADO,
    MODO_FALLO
} modo_t;

typedef enum {
    TIEMPO_NORMAL = 0,
    TIEMPO_ALTO,
    TIEMPO_MUY_ALTO,
    TIEMPO_MANUAL
} tiempo_nivel_t;

typedef struct {
    int            resumen;
    int            n_solicitudes;
    int            n_sol_fijado;
    int            plazo_s;
    int            ventana_seg;
    int            mostrar_salida;
    const char    *lote_path;
    const char    *cmd_linea;
    const char    *fuente;
    int            ret_dias;
    int            ret_seg;
    destino_t      destino;
    const char    *backend_host;
    int            backend_puerto;
    int            backend_pid;
    const char    *lanzar_backend;
    const char    *ruta_http;
    const char    *unidad;
    int            linea_base_s;
    const char    *app_target;
    const char    *lanzar_app;
    int            fin_por_orden;
    modo_t         modo;
    tiempo_nivel_t tiempo_nivel;
    double         slo_ms;
    const char    *entradas;
    const char    *tamanos;
    int            fallo_en;
} solicitudes_cfg_t;

int ejecutar_solicitudes(const solicitudes_cfg_t *cfg,
                         const char *dir_priv,
                         const char *spans_sock,
                         volatile sig_atomic_t *detener_flag);

#endif /* SOLICITUDES_H */
