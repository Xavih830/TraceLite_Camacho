#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"
DATOS="$RAIZ/datos"

if [ ! -f "$DATOS/normal.bin" ] || [ ! -f "$DATOS/pesado.bin" ]; then
    bash "$RAIZ/docs/pruebas/preparar_datos.sh"
fi

TMP_DIR="$(mktemp -d /tmp/tl_anom_XXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT
export TRACELITE_DIR="$TMP_DIR"
export XDG_RUNTIME_DIR="$TMP_DIR"

LOTE="$TMP_DIR/lote_anomalo.txt"
cat > "$LOTE" <<EOF
gzip -kfc $DATOS/normal.bin
gzip -kfc $DATOS/normal.bin
gzip -kfc $DATOS/normal.bin
gzip -kfc $DATOS/normal.bin
gzip -kfc $DATOS/pesado.bin
EOF

OUT=$("$BIN" --resumen --lote "$LOTE")
echo "$OUT"
echo "$OUT" | grep -q "\[ALERTA\] servicio=servicio_b trace_id="
echo "$OUT" | grep -q "tipo=latencia_alta"
echo "$OUT" | grep -q "solicitudes=5 spans=10 alertas=1 errores=0"
[ "$(wc -l < "$TMP_DIR"/*.jsonl)" -eq 10 ]
echo "[OK] caso_anomalo.sh superado"
