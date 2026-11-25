#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_so_XXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

echo "=== Demostración sobre servicios existentes del SO (sección 4.9) ==="

if systemctl is-active -q cups.service 2>/dev/null; then
    echo "--- CUPS (cups.service) ---"
    "$BIN" --resumen --fuente proc --linea-base 1 --backend 127.0.0.1:631 --unidad cups.service --ruta /
else
    echo "CUPS: inactivo o ausente"
fi

if systemctl is-active -q systemd-resolved.service 2>/dev/null; then
    echo "--- DNS local (systemd-resolved.service) ---"
    "$BIN" --resumen --fuente proc --linea-base 1 --app systemd-resolved --unidad systemd-resolved.service --fin orden -- resolvectl query localhost
else
    echo "systemd-resolved: inactivo o ausente"
fi

echo "[OK] demo_servicios_so.sh completado"
