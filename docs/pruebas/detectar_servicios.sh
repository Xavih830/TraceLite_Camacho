#!/usr/bin/env bash
set -u

echo "=== Detección de servicios del SO (solo lectura, sin sudo) ==="

echo "--- Puertos TCP escuchando (ss -ltn) ---"
ss -ltn 2>/dev/null | head -n 20 || true

echo "--- Sockets UNIX relevantes (ss -lx) ---"
ss -lx 2>/dev/null | grep -E "snapd|dbus|cups|resolved" | head -n 15 || true

medir_5() {
    local etiqueta="$1"
    shift
    local tiempos=()
    for _ in 1 2 3 4 5; do
        local t0 t1 ms
        t0=$(date +%s%N)
        "$@" >/dev/null 2>&1 || true
        t1=$(date +%s%N)
        ms=$(awk "BEGIN { printf \"%.2f\", ($t1 - $t0)/1000000 }")
        tiempos+=("$ms")
    done
    local med
    med=$(printf "%s\n" "${tiempos[@]}" | sort -n | sed -n '3p')
    echo "  latencia_5 (${etiqueta}): [${tiempos[*]}] ms -> mediana=${med} ms"
}

echo "--- Servicios systemd ---"
for u in cups.service systemd-resolved.service snapd.service systemd-hostnamed.service; do
    est=$(systemctl is-active "$u" 2>/dev/null || true)
    pid=$(systemctl show -p MainPID --value "$u" 2>/dev/null || echo "0")
    usr=$(ps -o user= -p "$pid" 2>/dev/null | xargs || echo "-")
    echo "Servicio $u: estado=$est pid=$pid usuario=$usr"
    if [ "$est" = "active" ] || [ "${pid:-0}" -gt 0 ] 2>/dev/null; then
        case "$u" in
            cups.service)
                command -v curl >/dev/null 2>&1 && medir_5 "cups" curl -s -m 2 -o /dev/null http://127.0.0.1:631/
                ;;
            systemd-resolved.service)
                command -v resolvectl >/dev/null 2>&1 && medir_5 "resolved" resolvectl query localhost
                ;;
            snapd.service)
                if [ -S /run/snapd.socket ] && command -v curl >/dev/null 2>&1; then
                    medir_5 "snapd" curl -s -m 2 --unix-socket /run/snapd.socket http://localhost/v2/system-info
                fi
                ;;
            systemd-hostnamed.service)
                command -v hostnamectl >/dev/null 2>&1 && medir_5 "hostnamed" hostnamectl
                ;;
        esac
    fi
done

if ss -ltn 2>/dev/null | grep -q ":8000 "; then
    echo "Puerto 127.0.0.1:8000: activo"
    command -v curl >/dev/null 2>&1 && medir_5 "python8000" curl -s -m 2 -o /dev/null http://127.0.0.1:8000/
else
    echo "Puerto 127.0.0.1:8000: inactivo"
fi

exit 0
