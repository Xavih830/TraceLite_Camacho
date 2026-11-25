#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"
DATOS="$RAIZ/datos"

if [ ! -f "$DATOS/normal.bin" ]; then
    bash "$RAIZ/docs/pruebas/preparar_datos.sh"
fi

TMP_DIR="$(mktemp -d /tmp/tl_norm_XXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

OUT=$("$BIN" --resumen -n 4 -- gzip -kfc "$DATOS/normal.bin")
echo "$OUT"
echo "$OUT" | grep -q "solicitudes=4 spans=8 alertas=0 errores=0"
[ "$(wc -l < "$TMP_DIR"/*.jsonl)" -eq 8 ]
echo "[OK] caso_normal.sh superado"
