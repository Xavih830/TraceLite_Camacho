# Checklist — TraceLite (entrega final con eBPF)

## Requisitos del enunciado
- [x] **R1:** Dos procesos hijos, Servicio A y Servicio B, con `fork()`; `exec()` en al menos uno; IPC Sockets UNIX
- [x] **R2:** Al menos 5 solicitudes, 2 spans cada una (A raíz, B hijo), campos completos, contexto propagado, duración medida
- [x] **R3:** Proceso principal es Collector Lite: recibe por IPC y muestra por Trace ID con padre-hijo
- [x] **R4:** Persistencia justificada y demostrable; resumen final de solicitudes, spans y alertas
- [x] **R5:** Anomalía detectada con criterio justificado; alerta con servicio y Trace ID

## Pruebas obligatorias
- [x] **P1:** 4 o más solicitudes normales, sin alerta, spans almacenados
- [x] **P2:** 1 o más solicitudes anómalas, con detección, alerta y persistencia
- [x] **P3:** Ambos spans comparten Trace ID y `B.parent == A.span_id`
- [x] **E1:** Informe, código, Makefile, README, checklist y video de 10 min (guion listo; grabación pendiente por el usuario)

## Objetivos del usuario
- [x] **U1:** Programas reales y cotidianos, no servicios simulados
- [x] **U2:** Comando arbitrario después de `--`
- [x] **U3:** Atribución de procesos por solicitud con eBPF
- [x] **U4:** Observar servicios ya en ejecución (`backend` y `app`)
- [x] **U5:** Ring buffer eBPF (`BPF_MAP_TYPE_RINGBUF`)
- [x] **U6:** Retención de últimos 7 días en almacenamiento diario
- [ ] **U7:** Interfaz gráfica en C (`tracelite-gui`)
- [x] **U8:** Modos por objetivo con 5 solicitudes y plazo justificado
- [x] **U9:** Modos aplicables a servicios nuevos y vivos
- [x] **U10:** Reutilizar instantánea y resumen como base
- [x] **U11:** Niveles de tiempo (`--tiempo`)

## Fases
- [x] **F0:** Base y diagnóstico
- [x] **F1:** Núcleo compartido
- [x] **F2:** Solicitudes A/B con `/proc`
- [x] **F3:** Persistencia y resumen
- [x] **F4:** Anomalías, alertas y pruebas obligatorias
- [x] **F5:** eBPF para destino `programa`
- [x] **F6:** Destinos `backend` y `app` con `/proc`
- [x] **F7:** eBPF por conexión (`backend`)
- [x] **F8:** Servicios existentes del SO
- [x] **F9:** Modos, niveles de tiempo y lotes
- [x] **F10:** Integración, README, informe y checklist
- [x] **G1:** Interfaz gráfica base (`tracelite-gui`)
- [x] **G2:** Interfaz gráfica completa

## Recalibración de servicios del SO (sección 4.9)

| Servicio | Presente | PID | Usuario | Canal | Latencia mediana (5 consultas) | Línea base |
|---|---|---|---|---|---|---|
| CUPS (`cups.service`) | Sí | 1356 | root | TCP `127.0.0.1:631` / `/run/cups/cups.sock` | 14.04 ms | |
| DNS local (`systemd-resolved.service`) | Sí | 788 | systemd-resolve | `127.0.0.53:53` / D-Bus | 10.55 ms | |
| snapd (`snapd.service`) | Sí | 982 | root | `/run/snapd.socket` | 14.74 ms | |
| hostnamed (`systemd-hostnamed.service`) | Inactivo (bajo demanda) | 0 | - | D-Bus | - | |
| Servidor Python (`127.0.0.1:8000`) | Inactivo | - | - | TCP `127.0.0.1:8000` | - | |
