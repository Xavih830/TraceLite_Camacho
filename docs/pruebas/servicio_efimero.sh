#!/usr/bin/env bash
# Servicio que lanza un trabajador efimero (sleep 0.2) de forma continua.
# Cada trabajador vive menos que la ventana de 250 ms de monitor_pid,
# por lo que termina durante la observacion y produce ERROR [error_proceso].
while true; do
    sleep 0.2
done
