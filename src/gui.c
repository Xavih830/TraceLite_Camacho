#include "almacen.h"
#include "common.h"
#include "gui_form.h"
#include "tabla.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Visor y lanzador gráfico (GTK 4 + Cairo) en modo solo lectura sobre JSONL */

typedef struct {
    char    trace_id[33];
    char    hora[24];
    char    modo[16];
    char    destino[16];
    char    operacion[48];
    char    tipo[16];
    double  dur_ms;
    int     solicitud;
    int     tiene_error;
    int     tiene_alerta;
    char    anomalia[24];
    int64_t ini_ns;
    int64_t ini_real_ns;
    int     idx_spans[128];
    int     n_spans;
} traza_resumen_t;

typedef struct {
    almacen_indice_t alm;
    traza_resumen_t *trazas;
    int              n_trazas;
    int              sel_traza;

    GtkWidget       *win;
    GtkWidget       *lista_trazas;
    GtkWidget       *lista_alertas;
    GtkWidget       *area_cascada;
    GtkWidget       *lbl_detalle;
    GtkWidget       *lbl_estado;
    GtkWidget       *ent_buscar;
    GtkWidget       *dd_estado;
    GtkWidget       *dd_modo;
    GtkWidget       *chk_alertas;
    GFileMonitor    *monitor;

    /* Formulario G2 y control de subproceso */
    GtkWidget       *w_campos[C_TOTAL];
    GtkWidget       *fila_campos[C_TOTAL];
    GtkWidget       *lbl_comando;
    GtkWidget       *btn_ejecutar;
    GtkWidget       *btn_detener;
    GtkTextBuffer   *buf_salida;
    GSubprocess     *proc_activo;
    GInputStream    *stream_salida;
    char             read_buf[2048];
    gboolean         cerrando_confirmado;
} visor_app_t;

static void formatear_hora_real(int64_t real_ns, char *buf, size_t sz) {
    if (real_ns <= 0) {
        snprintf(buf, sz, "--:--:--");
        return;
    }
    time_t seg = (time_t)(real_ns / 1000000000LL);
    long ms = (long)((real_ns % 1000000000LL) / 1000000LL);
    struct tm tm_info;
    localtime_r(&seg, &tm_info);
    size_t u = strftime(buf, sz, "%H:%M:%S", &tm_info);
    if (u > 0 && u + 5 <= sz) {
        snprintf(buf + u, sz - u, ".%03ld", ms);
    }
}

static const char *explicar_atribucion(const char *atr) {
    if (strcmp(atr, "exacta") == 0) {
        return "exacta (descendencia registrada en el kernel vía eBPF)";
    }
    if (strcmp(atr, "conexion") == 0) {
        return "conexion (hilo marcado por socket TCP en 127.0.0.1 vía eBPF)";
    }
    if (strcmp(atr, "ventana") == 0) {
        return "ventana (muestreo /proc durante la solicitud; no prueba causa)";
    }
    return "sin atribución";
}

static void construir_indice_trazas(visor_app_t *app) {
    free(app->trazas);
    app->trazas = NULL;
    app->n_trazas = 0;

    if (app->alm.n <= 0) return;

    app->trazas = (traza_resumen_t *)calloc((size_t)app->alm.n, sizeof(traza_resumen_t));
    if (app->trazas == NULL) return;

    for (int i = 0; i < app->alm.n; i++) {
        const almacen_registro_t *r = &app->alm.regs[i];
        int idx = -1;
        for (int t = 0; t < app->n_trazas; t++) {
            if (strcmp(app->trazas[t].trace_id, r->span.trace_id) == 0) {
                idx = t;
                break;
            }
        }
        if (idx < 0) {
            idx = app->n_trazas++;
            traza_resumen_t *tr = &app->trazas[idx];
            snprintf(tr->trace_id, sizeof(tr->trace_id), "%s", r->span.trace_id);
            snprintf(tr->modo, sizeof(tr->modo), "%s", r->meta.modo[0] ? r->meta.modo : "-");
            snprintf(tr->destino, sizeof(tr->destino), "%s", r->meta.destino[0] ? r->meta.destino : "-");
            snprintf(tr->tipo, sizeof(tr->tipo), "%s", r->meta.tipo[0] ? r->meta.tipo : "solicitud");
            snprintf(tr->operacion, sizeof(tr->operacion), "%s", r->span.operacion);
            tr->ini_ns = r->span.ini;
            tr->ini_real_ns = r->meta.ini_real_ns;
            tr->solicitud = r->meta.solicitud;
            formatear_hora_real(r->meta.ini_real_ns, tr->hora, sizeof(tr->hora));
        }

        traza_resumen_t *tr = &app->trazas[idx];
        if (tr->n_spans < 128) {
            tr->idx_spans[tr->n_spans++] = i;
        }
        if (r->span.ini < tr->ini_ns) tr->ini_ns = r->span.ini;
        if (r->span.parent[0] == '\0') {
            snprintf(tr->operacion, sizeof(tr->operacion), "%s", r->span.operacion);
            tr->dur_ms = r->span.dur;
        } else if (r->span.dur > tr->dur_ms) {
            tr->dur_ms = r->span.dur;
        }
        if (r->span.estado != 0) tr->tiene_error = 1;
        if (r->span.anomalia[0] != '\0') {
            tr->tiene_alerta = 1;
            snprintf(tr->anomalia, sizeof(tr->anomalia), "%s", r->span.anomalia);
        }
    }
}

static void actualizar_detalle(visor_app_t *app) {
    if (app->sel_traza < 0 || app->sel_traza >= app->n_trazas) {
        gtk_label_set_text(GTK_LABEL(app->lbl_detalle), "Selecciona una traza para ver el detalle.");
        gtk_widget_queue_draw(app->area_cascada);
        return;
    }

    const traza_resumen_t *tr = &app->trazas[app->sel_traza];
    GString *gs = g_string_new("");
    if (tr->solicitud > 0) {
        g_string_append_printf(gs, "Solicitud #%d | ", tr->solicitud);
    }
    g_string_append_printf(gs, "Trace ID: %s | Tipo: %s | Modo: %s | Destino: %s | Hora: %s\n",
                           tr->trace_id, tr->tipo, tr->modo, tr->destino, tr->hora);
    if (tr->tiene_alerta) {
        g_string_append_printf(gs, "[ALERTA] %s\n", tr->anomalia);
    }

    if (strcmp(tr->tipo, "observacion") == 0) {
        g_string_append(gs, "\nTabla de procesos (observación):\n");
        g_string_append(gs, "PID      | Servicio         | Relación             | Operación        | CPU (ms) | Procs | Dur (ms) | Estado\n");
        g_string_append(gs, "---------+------------------+----------------------+------------------+----------+-------+----------+--------\n");
        for (int i = 0; i < tr->n_spans; i++) {
            const almacen_registro_t *r = &app->alm.regs[tr->idx_spans[i]];
            const span_t *s = &r->span;
            const char *est = s->estado ? "ERROR" : (s->anomalia[0] ? "ANOMALIA" : "OK");
            char rel[48];
            tabla_formatear_relacion(s, tr->destino, tr->tipo, rel, sizeof(rel));
            g_string_append_printf(gs, "%-8d | %-16.16s | %-20.20s | %-16.16s | %8.2f | %5d | %8.2f | %s\n",
                                   s->pid, s->servicio, rel, s->operacion, s->cpu_ms, s->n_procs, s->dur, est);
        }
    }

    for (int i = 0; i < tr->n_spans; i++) {
        const almacen_registro_t *r = &app->alm.regs[tr->idx_spans[i]];
        const span_t *s = &r->span;
        const char *est = s->estado ? "ERROR" : (s->anomalia[0] ? "ANOMALIA" : "OK");
        char rel[48];
        tabla_formatear_relacion(s, tr->destino, tr->tipo, rel, sizeof(rel));
        g_string_append_printf(gs,
                               "\n• %s (%s) | relación=%s | span=%s parent=%s | pid=%d | dur=%.2f ms | cpu=%.2f ms | procs=%d | estado=%s",
                               s->servicio, s->operacion, rel, s->span_id,
                               s->parent[0] ? s->parent : "(raiz)",
                               s->pid, s->dur, s->cpu_ms, s->n_procs, est);
        if (s->atribucion[0] != '\0') {
            g_string_append_printf(gs, "\n  Atribución: %s", explicar_atribucion(s->atribucion));
        }
        if (s->msg[0] != '\0') {
            g_string_append_printf(gs, "\n  Detalle: %s", s->msg);
        }
    }

    gtk_label_set_text(GTK_LABEL(app->lbl_detalle), gs->str);
    g_string_free(gs, TRUE);
    int alto = tr->n_spans * 26 + 24;
    if (alto < 180) alto = 180;
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->area_cascada), alto);
    gtk_widget_queue_draw(app->area_cascada);
}

static void ordenar_dfs_cascada(const visor_app_t *app, const traza_resumen_t *tr,
                                int idx_pos, int prof, int *usado,
                                int *orden, int *prof_out, int *n_out) {
    if (usado[idx_pos] || *n_out >= tr->n_spans) return;
    usado[idx_pos] = 1;
    orden[*n_out] = idx_pos;
    prof_out[*n_out] = prof;
    (*n_out)++;
    const char *sid = app->alm.regs[tr->idx_spans[idx_pos]].span.span_id;
    for (int i = 0; i < tr->n_spans; i++) {
        if (!usado[i] && strcmp(app->alm.regs[tr->idx_spans[i]].span.parent, sid) == 0) {
            ordenar_dfs_cascada(app, tr, i, prof + 1, usado, orden, prof_out, n_out);
        }
    }
}

static void dibujar_cascada(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    (void)area;
    visor_app_t *app = (visor_app_t *)data;

    cairo_set_source_rgb(cr, 0.11, 0.12, 0.14);
    cairo_paint(cr);

    if (app->sel_traza < 0 || app->sel_traza >= app->n_trazas) return;
    const traza_resumen_t *tr = &app->trazas[app->sel_traza];
    if (tr->n_spans <= 0) return;

    int64_t min_ini = tr->ini_ns;
    int64_t max_fin = min_ini + 1000000LL;
    for (int i = 0; i < tr->n_spans; i++) {
        const span_t *s = &app->alm.regs[tr->idx_spans[i]].span;
        if (s->ini < min_ini) min_ini = s->ini;
        if (s->fin > max_fin) max_fin = s->fin;
    }
    double total_ms = (double)(max_fin - min_ini) / 1000000.0;
    if (total_ms <= 0.0) total_ms = 1.0;

    double margen_izq = 210.0;
    double ancho_bar = (double)w - margen_izq - 84.0;
    if (ancho_bar < 40.0) ancho_bar = 40.0;
    double escala = ancho_bar / total_ms;
    double alto_fila = 26.0;

    int orden[128], profs[128], usado[128];
    int n_ord = 0;
    memset(usado, 0, sizeof(usado));
    for (int i = 0; i < tr->n_spans; i++) {
        const span_t *s = &app->alm.regs[tr->idx_spans[i]].span;
        int tiene_padre = 0;
        for (int j = 0; j < tr->n_spans; j++) {
            if (i != j && strcmp(s->parent, app->alm.regs[tr->idx_spans[j]].span.span_id) == 0) {
                tiene_padre = 1;
                break;
            }
        }
        if (!tiene_padre) {
            ordenar_dfs_cascada(app, tr, i, 0, usado, orden, profs, &n_ord);
        }
    }
    for (int i = 0; i < tr->n_spans; i++) {
        if (!usado[i]) {
            ordenar_dfs_cascada(app, tr, i, 0, usado, orden, profs, &n_ord);
        }
    }

    cairo_select_font_face(cr, "Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);

    for (int row = 0; row < n_ord && (row + 1) * alto_fila < (double)h + alto_fila; row++) {
        const span_t *s = &app->alm.regs[tr->idx_spans[orden[row]]].span;
        double y = 12.0 + (double)row * alto_fila;
        int sangria = profs[row] * 10;
        if (sangria > 70) sangria = 70;

        cairo_save(cr);
        cairo_rectangle(cr, 0.0, y, margen_izq - 8.0, alto_fila);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0.88, 0.90, 0.93);
        cairo_move_to(cr, 10.0 + sangria, y + 14.0);
        char lbl[80];
        snprintf(lbl, sizeof(lbl), "%.18s (%.14s)", s->servicio, s->operacion);
        cairo_show_text(cr, lbl);
        cairo_restore(cr);

        double off_ms = (double)(s->ini - min_ini) / 1000000.0;
        if (off_ms < 0.0) off_ms = 0.0;
        double x = margen_izq + off_ms * escala;
        double bw = s->dur * escala;
        if (bw < 4.0) bw = 4.0;

        if (s->estado != 0) {
            cairo_set_source_rgb(cr, 0.85, 0.25, 0.25);
        } else if (s->anomalia[0] != '\0') {
            cairo_set_source_rgb(cr, 0.92, 0.65, 0.15);
        } else {
            cairo_set_source_rgb(cr, 0.20, 0.65, 0.78);
        }
        cairo_rectangle(cr, x, y + 2.0, bw, 16.0);
        cairo_fill(cr);

        cairo_set_source_rgb(cr, 0.78, 0.82, 0.86);
        char dur_txt[32];
        snprintf(dur_txt, sizeof(dur_txt), "%.2f ms", s->dur);
        cairo_move_to(cr, x + bw + 6.0, y + 14.0);
        cairo_show_text(cr, dur_txt);
    }
}

static void limpiar_listbox(GtkWidget *box) {
    GtkWidget *child = gtk_widget_get_first_child(box);
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(box), child);
        child = next;
    }
}

static void poblar_listas(visor_app_t *app) {
    limpiar_listbox(app->lista_trazas);
    limpiar_listbox(app->lista_alertas);

    const char *q = gtk_editable_get_text(GTK_EDITABLE(app->ent_buscar));
    guint est_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dd_estado));
    guint modo_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(app->dd_modo));
    gboolean solo_al = gtk_check_button_get_active(GTK_CHECK_BUTTON(app->chk_alertas));
    const char *modos_f[] = { "", "respuesta", "estabilidad", "escalado", "fallo", "observacion" };

    for (int i = app->n_trazas - 1; i >= 0; i--) {
        const traza_resumen_t *tr = &app->trazas[i];

        if (tr->tiene_alerta) {
            char al_txt[160];
            snprintf(al_txt, sizeof(al_txt), "[ALERTA] %s | %s | %.2f ms | %s",
                     tr->anomalia, tr->operacion, tr->dur_ms, tr->trace_id);
            GtkWidget *lbl = gtk_label_new(al_txt);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
            GtkWidget *row = gtk_list_box_row_new();
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
            g_object_set_data(G_OBJECT(row), "idx_traza", GINT_TO_POINTER(i));
            gtk_list_box_append(GTK_LIST_BOX(app->lista_alertas), row);
        }

        if (solo_al && !tr->tiene_alerta) continue;
        if (est_sel == 1 && (tr->tiene_error || tr->tiene_alerta)) continue;
        if (est_sel == 2 && !tr->tiene_error) continue;
        if (est_sel == 3 && !tr->tiene_alerta) continue;
        if (modo_sel > 0 && modo_sel < 6 && strcmp(tr->modo, modos_f[modo_sel]) != 0) continue;
        if (q != NULL && q[0] != '\0') {
            if (!strstr(tr->trace_id, q) && !strstr(tr->operacion, q) &&
                !strstr(tr->destino, q) && !strstr(tr->modo, q)) {
                continue;
            }
        }

        char txt[192];
        const char *est = tr->tiene_error ? "ERROR" : (tr->tiene_alerta ? "ANOMALIA" : "OK");
        snprintf(txt, sizeof(txt), "%s | %s | %s | %s | %.2f ms | %s%s%s",
                 tr->hora, tr->modo, tr->destino, tr->operacion, tr->dur_ms, est,
                 tr->tiene_alerta ? " [" : "", tr->tiene_alerta ? tr->anomalia : "");
        if (tr->tiene_alerta) strncat(txt, "]", sizeof(txt) - strlen(txt) - 1);

        GtkWidget *lbl = gtk_label_new(txt);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        g_object_set_data(G_OBJECT(row), "idx_traza", GINT_TO_POINTER(i));
        gtk_list_box_append(GTK_LIST_BOX(app->lista_trazas), row);
    }

    char info[256];
    snprintf(info, sizeof(info), "Trazas: %d | Spans: %d | Líneas corruptas ignoradas: %d (%.80s)",
             app->n_trazas, app->alm.n, app->alm.corruptas, app->alm.dir);
    gtk_label_set_text(GTK_LABEL(app->lbl_estado), info);
}

static void recargar_datos(visor_app_t *app) {
    almacen_liberar(&app->alm);
    /* Modo solo lectura: ret_dias=0 y ret_seg=0 para no borrar ni modificar archivos */
    almacen_cargar(&app->alm, NULL, 0, 0);
    construir_indice_trazas(app);
    poblar_listas(app);
    if (app->sel_traza >= app->n_trazas) {
        app->sel_traza = (app->n_trazas > 0) ? (app->n_trazas - 1) : -1;
    }
    actualizar_detalle(app);
}

static void on_traza_sel(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    if (row == NULL) return;
    visor_app_t *app = (visor_app_t *)data;
    app->sel_traza = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx_traza"));
    actualizar_detalle(app);
}

static void on_filtro_cambio(GtkWidget *w, gpointer data) {
    (void)w;
    poblar_listas((visor_app_t *)data);
}

static void on_dd_cambio(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)obj;
    (void)pspec;
    poblar_listas((visor_app_t *)data);
}

static void on_dir_cambio(GFileMonitor *mon, GFile *f, GFile *other,
                          GFileMonitorEvent ev, gpointer data) {
    (void)mon;
    (void)f;
    (void)other;
    if (ev == G_FILE_MONITOR_EVENT_CHANGED || ev == G_FILE_MONITOR_EVENT_CREATED ||
        ev == G_FILE_MONITOR_EVENT_DELETED) {
        recargar_datos((visor_app_t *)data);
    }
}

static void leer_valores_form(const visor_app_t *app, gui_form_valores_t *v) {
    gui_form_valores_defecto(v);
    v->forma     = (gui_forma_t)gtk_drop_down_get_selected(GTK_DROP_DOWN(app->w_campos[C_FORMA]));
    v->obs_durante = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(app->w_campos[C_OBS_DURANTE]));
    v->obs_duracion_s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_OBS_DURACION]));
    v->obs_interval_ms = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_OBS_INTERVAL]));
    v->app_fin   = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(app->w_campos[C_APP_FIN]));
    v->linea_base_s = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_APP_LINEA_BASE]));
    v->modo      = (gui_modo_t)gtk_drop_down_get_selected(GTK_DROP_DOWN(app->w_campos[C_MODO]));
    v->plazo_s   = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_PLAZO_S]));
    v->slo_ms    = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_OPT_SLO_MS]));
    v->fallo_en  = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->w_campos[C_OPT_FALLO_EN]));

    snprintf(v->obs_objetivo, sizeof(v->obs_objetivo), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_OBS_OBJETIVO])));
    snprintf(v->app_target, sizeof(v->app_target), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_APP_TARGET])));
    snprintf(v->app_unidad, sizeof(v->app_unidad), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_APP_UNIDAD])));
    snprintf(v->app_orden, sizeof(v->app_orden), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_APP_ORDEN])));
    snprintf(v->entradas, sizeof(v->entradas), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_OPT_ENTRADAS])));
    snprintf(v->tamanos, sizeof(v->tamanos), "%s", gtk_editable_get_text(GTK_EDITABLE(app->w_campos[C_OPT_TAMANOS])));
}

static void actualizar_form_ui(visor_app_t *app) {
    gui_form_valores_t v;
    leer_valores_form(app, &v);

    int n_tab = 0;
    const campo_form_t *tab = gui_form_tabla(&n_tab);
    for (int i = 0; i < n_tab; i++) {
        gboolean vis = (tab[i].aplica != NULL) ? tab[i].aplica(&v) : TRUE;
        gtk_widget_set_visible(app->fila_campos[i], vis);
    }

    char ruta_tl[512];
    if (ruta_hermana("tracelite", ruta_tl, sizeof(ruta_tl)) < 0) {
        snprintf(ruta_tl, sizeof(ruta_tl), "tracelite");
    }
    char **argv = NULL;
    int argc = 0;
    if (gui_form_construir_argv(&v, ruta_tl, &argv, &argc) == 0) {
        char prev[1024];
        gui_form_formatear_preview(argv, argc, prev, sizeof(prev));
        gtk_label_set_text(GTK_LABEL(app->lbl_comando), prev);
        gui_form_liberar_argv(argv, argc);
    }
}

static void on_form_cambio(GtkWidget *w, gpointer data) {
    (void)w;
    actualizar_form_ui((visor_app_t *)data);
}

static void on_form_dd_cambio(GObject *obj, GParamSpec *pspec, gpointer data) {
    (void)obj;
    (void)pspec;
    actualizar_form_ui((visor_app_t *)data);
}

static void agregar_log_salida(visor_app_t *app, const char *txt, gssize len) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(app->buf_salida, &end);
    gtk_text_buffer_insert(app->buf_salida, &end, txt, (gint)len);
}

static void leer_salida_cb(GObject *src, GAsyncResult *res, gpointer data) {
    visor_app_t *app = (visor_app_t *)data;
    GError *err = NULL;
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, &err);
    if (n > 0) {
        agregar_log_salida(app, app->read_buf, n);
        g_input_stream_read_async(G_INPUT_STREAM(src), app->read_buf, sizeof(app->read_buf),
                                  G_PRIORITY_DEFAULT, NULL, leer_salida_cb, app);
    } else if (err != NULL) {
        g_error_free(err);
    }
}

static void on_proc_wait_cb(GObject *src, GAsyncResult *res, gpointer data) {
    visor_app_t *app = (visor_app_t *)data;
    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (err != NULL) g_error_free(err);

    if (g_subprocess_get_if_exited(G_SUBPROCESS(src))) {
        char fin_msg[64];
        snprintf(fin_msg, sizeof(fin_msg), "\n[tracelite terminó con código %d]\n",
                 g_subprocess_get_exit_status(G_SUBPROCESS(src)));
        agregar_log_salida(app, fin_msg, -1);
    } else if (g_subprocess_get_if_signaled(G_SUBPROCESS(src))) {
        char fin_msg[64];
        snprintf(fin_msg, sizeof(fin_msg), "\n[tracelite detenido por señal %d]\n",
                 g_subprocess_get_term_sig(G_SUBPROCESS(src)));
        agregar_log_salida(app, fin_msg, -1);
    }

    g_clear_object(&app->stream_salida);
    g_clear_object(&app->proc_activo);
    gtk_widget_set_sensitive(app->btn_ejecutar, TRUE);
    gtk_widget_set_sensitive(app->btn_detener, FALSE);
    recargar_datos(app);
}

static void on_ejecutar_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    visor_app_t *app = (visor_app_t *)data;
    if (app->proc_activo != NULL) return;

    gui_form_valores_t v;
    leer_valores_form(app, &v);
    char ruta_tl[512];
    if (ruta_hermana("tracelite", ruta_tl, sizeof(ruta_tl)) < 0) {
        snprintf(ruta_tl, sizeof(ruta_tl), "tracelite");
    }

    char **argv = NULL;
    int argc = 0;
    if (gui_form_construir_argv(&v, ruta_tl, &argv, &argc) < 0 || argv == NULL) return;

    gtk_text_buffer_set_text(app->buf_salida, "", 0);

    GError *err = NULL;
    app->proc_activo = g_subprocess_newv((const char * const *)argv,
                                         G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_MERGE,
                                         &err);
    gui_form_liberar_argv(argv, argc);

    if (app->proc_activo == NULL) {
        if (err != NULL) {
            agregar_log_salida(app, err->message, -1);
            g_error_free(err);
        }
        return;
    }

    gtk_widget_set_sensitive(app->btn_ejecutar, FALSE);
    gtk_widget_set_sensitive(app->btn_detener, TRUE);

    app->stream_salida = g_object_ref(g_subprocess_get_stdout_pipe(app->proc_activo));
    g_input_stream_read_async(app->stream_salida, app->read_buf, sizeof(app->read_buf),
                              G_PRIORITY_DEFAULT, NULL, leer_salida_cb, app);
    g_subprocess_wait_async(app->proc_activo, NULL, on_proc_wait_cb, app);
}

static void on_detener_clicked(GtkButton *btn, gpointer data) {
    (void)btn;
    visor_app_t *app = (visor_app_t *)data;
    if (app->proc_activo != NULL) {
        /* Enviar SIGINT solo al proceso tracelite lanzado por la interfaz */
        g_subprocess_send_signal(app->proc_activo, SIGINT);
    }
}

static void on_cerrar_confirm_cb(GObject *src, GAsyncResult *res, gpointer data) {
    visor_app_t *app = (visor_app_t *)data;
    GError *err = NULL;
    int resp = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
    if (err != NULL) g_error_free(err);
    if (resp == 1) {
        if (app->proc_activo != NULL) {
            g_subprocess_send_signal(app->proc_activo, SIGINT);
        }
        app->cerrando_confirmado = TRUE;
        gtk_window_close(GTK_WINDOW(app->win));
    }
}

static gboolean on_close_request(GtkWindow *win, gpointer data) {
    (void)win;
    visor_app_t *app = (visor_app_t *)data;
    if (app->proc_activo != NULL && !app->cerrando_confirmado) {
        GtkAlertDialog *dlg = gtk_alert_dialog_new("Hay una ejecución de tracelite en curso.\n¿Deseas detenerla y cerrar?");
        const char *botones[] = { "Cancelar", "Detener y cerrar", NULL };
        gtk_alert_dialog_set_buttons(dlg, botones);
        gtk_alert_dialog_set_cancel_button(dlg, 0);
        gtk_alert_dialog_set_default_button(dlg, 0);
        gtk_alert_dialog_choose(dlg, GTK_WINDOW(app->win), NULL, on_cerrar_confirm_cb, app);
        g_object_unref(dlg);
        return TRUE;
    }
    return FALSE;
}

static void on_activate(GtkApplication *gapp, gpointer data) {
    visor_app_t *app = (visor_app_t *)data;

    app->win = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app->win), "TraceLite — Visor y Lanzador");
    gtk_window_set_default_size(GTK_WINDOW(app->win), 1320, 780);
    g_signal_connect(app->win, "close-request", G_CALLBACK(on_close_request), app);

    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(hpaned), 380);
    gtk_window_set_child(GTK_WINDOW(app->win), hpaned);

    /* Panel izquierdo: Formulario G2 generado desde la tabla estática */
    GtkWidget *izq = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(izq, 8);
    gtk_widget_set_margin_end(izq, 8);
    gtk_widget_set_margin_top(izq, 8);
    gtk_widget_set_margin_bottom(izq, 8);
    gtk_paned_set_start_child(GTK_PANED(hpaned), izq);

    GtkWidget *box_campos = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    int n_tab = 0;
    const campo_form_t *tab = gui_form_tabla(&n_tab);
    for (int i = 0; i < n_tab; i++) {
        GtkWidget *fila = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *lbl = gtk_label_new(tab[i].etiqueta);
        gtk_widget_set_size_request(lbl, 120, -1);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_box_append(GTK_BOX(fila), lbl);

        GtkWidget *ctrl = NULL;
        if (tab[i].tipo == CAMPO_DROPDOWN) {
            ctrl = gtk_drop_down_new_from_strings(tab[i].opciones);
            gtk_drop_down_set_selected(GTK_DROP_DOWN(ctrl), (guint)tab[i].def_val);
            g_signal_connect(ctrl, "notify::selected", G_CALLBACK(on_form_dd_cambio), app);
        } else if (tab[i].tipo == CAMPO_SPIN) {
            ctrl = gtk_spin_button_new_with_range(tab[i].min_val, tab[i].max_val, 1);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ctrl), tab[i].def_val);
            g_signal_connect(ctrl, "value-changed", G_CALLBACK(on_form_cambio), app);
        } else {
            ctrl = gtk_entry_new();
            if (tab[i].def_str != NULL) {
                gtk_editable_set_text(GTK_EDITABLE(ctrl), tab[i].def_str);
            }
            g_signal_connect(ctrl, "changed", G_CALLBACK(on_form_cambio), app);
        }
        gtk_widget_set_hexpand(ctrl, TRUE);
        gtk_box_append(GTK_BOX(fila), ctrl);
        gtk_box_append(GTK_BOX(box_campos), fila);
        app->w_campos[i] = ctrl;
        app->fila_campos[i] = fila;
    }

    GtkWidget *sc_form = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sc_form, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_form), box_campos);
    gtk_box_append(GTK_BOX(izq), sc_form);

    app->lbl_comando = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_comando), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(app->lbl_comando), TRUE);
    gtk_label_set_selectable(GTK_LABEL(app->lbl_comando), TRUE);
    gtk_box_append(GTK_BOX(izq), app->lbl_comando);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->btn_ejecutar = gtk_button_new_with_label("Ejecutar");
    app->btn_detener  = gtk_button_new_with_label("Detener");
    gtk_widget_set_sensitive(app->btn_detener, FALSE);
    gtk_widget_set_hexpand(app->btn_ejecutar, TRUE);
    gtk_widget_set_hexpand(app->btn_detener, TRUE);
    gtk_box_append(GTK_BOX(btns), app->btn_ejecutar);
    gtk_box_append(GTK_BOX(btns), app->btn_detener);
    gtk_box_append(GTK_BOX(izq), btns);
    g_signal_connect(app->btn_ejecutar, "clicked", G_CALLBACK(on_ejecutar_clicked), app);
    g_signal_connect(app->btn_detener, "clicked", G_CALLBACK(on_detener_clicked), app);

    GtkWidget *tv_sal = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv_sal), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv_sal), TRUE);
    app->buf_salida = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv_sal));
    GtkWidget *sc_sal = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sc_sal, -1, 150);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_sal), tv_sal);
    gtk_box_append(GTK_BOX(izq), sc_sal);

    /* Panel derecho: Visor G1 */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 8);
    gtk_widget_set_margin_end(vbox, 8);
    gtk_widget_set_margin_top(vbox, 8);
    gtk_widget_set_margin_bottom(vbox, 8);
    gtk_paned_set_end_child(GTK_PANED(hpaned), vbox);

    actualizar_form_ui(app);

    /* Barra de filtros */
    GtkWidget *filtros = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->ent_buscar = gtk_search_entry_new();
    gtk_widget_set_hexpand(app->ent_buscar, TRUE);
    const char *ests[] = { "Estado: Todos", "OK", "ERROR", "ANOMALIA", NULL };
    app->dd_estado = gtk_drop_down_new_from_strings(ests);
    const char *modos[] = { "Modo: Todos", "respuesta", "estabilidad", "escalado", "fallo", "observacion", NULL };
    app->dd_modo = gtk_drop_down_new_from_strings(modos);
    app->chk_alertas = gtk_check_button_new_with_label("Solo alertas");
    GtkWidget *btn_recargar = gtk_button_new_with_label("Recargar");

    gtk_box_append(GTK_BOX(filtros), app->ent_buscar);
    gtk_box_append(GTK_BOX(filtros), app->dd_estado);
    gtk_box_append(GTK_BOX(filtros), app->dd_modo);
    gtk_box_append(GTK_BOX(filtros), app->chk_alertas);
    gtk_box_append(GTK_BOX(filtros), btn_recargar);
    gtk_box_append(GTK_BOX(vbox), filtros);

    g_signal_connect(app->ent_buscar, "search-changed", G_CALLBACK(on_filtro_cambio), app);
    g_signal_connect(app->dd_estado, "notify::selected", G_CALLBACK(on_dd_cambio), app);
    g_signal_connect(app->dd_modo, "notify::selected", G_CALLBACK(on_dd_cambio), app);
    g_signal_connect(app->chk_alertas, "toggled", G_CALLBACK(on_filtro_cambio), app);
    g_signal_connect_swapped(btn_recargar, "clicked", G_CALLBACK(recargar_datos), app);

    /* Panel dividido: Trazas/Alertas arriba, Cascada/Detalle abajo */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), paned);

    GtkWidget *nb_sup = gtk_notebook_new();
    app->lista_trazas = gtk_list_box_new();
    GtkWidget *sc_tr = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_tr), app->lista_trazas);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb_sup), sc_tr, gtk_label_new("Trazas"));

    app->lista_alertas = gtk_list_box_new();
    GtkWidget *sc_al = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_al), app->lista_alertas);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb_sup), sc_al, gtk_label_new("Alertas"));
    gtk_paned_set_start_child(GTK_PANED(paned), nb_sup);

    g_signal_connect(app->lista_trazas, "row-selected", G_CALLBACK(on_traza_sel), app);
    g_signal_connect(app->lista_alertas, "row-selected", G_CALLBACK(on_traza_sel), app);

    GtkWidget *vbox_inf = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    app->area_cascada = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(app->area_cascada), 180);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(app->area_cascada), dibujar_cascada, app, NULL);
    GtkWidget *sc_cas = gtk_scrolled_window_new();
    gtk_widget_set_size_request(sc_cas, -1, 220);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_cas), app->area_cascada);
    gtk_box_append(GTK_BOX(vbox_inf), sc_cas);

    app->lbl_detalle = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_detalle), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(app->lbl_detalle), TRUE);
    GtkWidget *sc_det = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(sc_det, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_det), app->lbl_detalle);
    gtk_box_append(GTK_BOX(vbox_inf), sc_det);
    gtk_paned_set_end_child(GTK_PANED(paned), vbox_inf);

    app->lbl_estado = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->lbl_estado), 0.0f);
    gtk_box_append(GTK_BOX(vbox), app->lbl_estado);

    recargar_datos(app);

    GFile *gdir = g_file_new_for_path(app->alm.dir);
    app->monitor = g_file_monitor_directory(gdir, G_FILE_MONITOR_NONE, NULL, NULL);
    if (app->monitor != NULL) {
        g_signal_connect(app->monitor, "changed", G_CALLBACK(on_dir_cambio), app);
    }
    g_object_unref(gdir);

    gtk_window_present(GTK_WINDOW(app->win));
}

int main(int argc, char *argv[]) {
    visor_app_t app;
    memset(&app, 0, sizeof(app));
    app.sel_traza = -1;

    GtkApplication *gapp = gtk_application_new("org.tracelite.visor", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &app);
    int rc = g_application_run(G_APPLICATION(gapp), argc, argv);

    if (app.monitor != NULL) g_object_unref(app.monitor);
    g_object_unref(gapp);
    free(app.trazas);
    almacen_liberar(&app.alm);
    return rc;
}
