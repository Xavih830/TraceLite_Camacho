#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_DIR="$(mktemp -d /tmp/tl_f9_XXXX)"
python3 "$RAIZ/docs/pruebas/backend_demo.py" 18086 &
BE_PID=$!
trap 'kill $BE_PID 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT
sleep 0.3

export XDG_RUNTIME_DIR="$TMP_DIR"

echo "[1/5] Modo respuesta (normal=0 alertas, --slo-ms 20=5 alertas)..."
TRACELITE_DIR="$TMP_DIR/r1" "$BIN" --resumen --fuente proc --linea-base 0 --modo respuesta --backend 127.0.0.1:18086 --ruta "/espera?ms=50" | grep -q "alertas=0"
TRACELITE_DIR="$TMP_DIR/r2" "$BIN" --resumen --fuente proc --linea-base 0 --modo respuesta --slo-ms 20 --backend 127.0.0.1:18086 --ruta "/espera?ms=50" | grep -q "alertas=5"
echo "  OK"

echo "[2/5] Modo estabilidad (5 iguales=0 alertas, 4x64+640=1 alerta)..."
OE1=$(TRACELITE_DIR="$TMP_DIR/e1" "$BIN" --resumen --fuente proc --linea-base 0 --modo estabilidad --backend 127.0.0.1:18086 --ruta "/hash?mb=64")
echo "$OE1" | grep -q "alertas=0" || { echo "FALLO E1:"; echo "$OE1"; exit 1; }
OE2=$(TRACELITE_DIR="$TMP_DIR/e2" "$BIN" --resumen --fuente proc --linea-base 0 --modo estabilidad --backend 127.0.0.1:18086 --ruta /hash --entradas "mb=64,mb=64,mb=64,mb=64,mb=640")
echo "$OE2" | grep -q "alertas=1" || { echo "FALLO E2:"; echo "$OE2"; exit 1; }
echo "  OK"

echo "[3/5] Modo escalado (mb=32,64,128,256,512 con tamanos 1,2,4,8,16 -> 0 alertas)..."
TRACELITE_DIR="$TMP_DIR/s1" "$BIN" --resumen --fuente proc --linea-base 0 --modo escalado --backend 127.0.0.1:18086 --ruta /hash --entradas "mb=32,mb=64,mb=128,mb=256,mb=512" --tamanos "1,2,4,8,16" | grep -q "alertas=0"
echo "  OK"

echo "[4/5] Modo fallo (--fallo-en 5 -> 1 alerta error_proceso)..."
TRACELITE_DIR="$TMP_DIR/f1" "$BIN" --resumen --fuente proc --linea-base 0 --modo fallo --fallo-en 5 --backend 127.0.0.1:18086 --ruta "/espera?ms=20" | grep -q "alertas=1"
echo "  OK"

echo "[5/5] Niveles de tiempo (--tiempo normal vs alto) y rechazo de combinaciones (código 2)..."
OUT_N=$(TRACELITE_DIR="$TMP_DIR/t1" "$BIN" --fuente proc --linea-base 0 --modo respuesta --tiempo normal --backend 127.0.0.1:18086 --ruta "/espera?ms=15000")
echo "$OUT_N" | grep -q "errores=2"
OUT_A=$(TRACELITE_DIR="$TMP_DIR/t2" "$BIN" --fuente proc --linea-base 0 --modo respuesta --tiempo alto --backend 127.0.0.1:18086 --ruta "/espera?ms=15000")
echo "$OUT_A" | grep -q "tipo=slo_excedido"

set +e
"$BIN" --tiempo alto --plazo 5 -- true 2>/dev/null; RC1=$?
"$BIN" --modo escalado -- true 2>/dev/null; RC2=$?
"$BIN" --modo fallo -- true 2>/dev/null; RC3=$?
set -e
[ "$RC1" -eq 2 ] && [ "$RC2" -eq 2 ] && [ "$RC3" -eq 2 ]
echo "  OK"

echo "[OK] matriz_modos.sh superada con exito"
