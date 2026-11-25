#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATOS="$RAIZ/datos"
mkdir -p "$DATOS"

mb=4
while [ "$mb" -le 64 ]; do
    dd if=/dev/urandom of="$DATOS/normal.bin" bs=1M count="$mb" status=none
    t0=$(date +%s%N)
    gzip -kfc "$DATOS/normal.bin" > /dev/null
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    echo "[preparar_datos] normal.bin=${mb} MiB -> gzip=${ms} ms"
    if [ "$ms" -ge 220 ]; then
        break
    fi
    mb=$((mb * 2))
done

rm -f "$DATOS/pesado.bin"
for _ in $(seq 1 10); do
    cat "$DATOS/normal.bin" >> "$DATOS/pesado.bin"
done

echo "[OK] Datos calibrados en $DATOS: normal.bin=$(stat -c %s "$DATOS/normal.bin") B, pesado.bin=$(stat -c %s "$DATOS/pesado.bin") B"
