#!/usr/bin/env bash
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$RAIZ/bin/tracelite"

TMP_RUN="$(mktemp -d /tmp/tl_base_XXXX)"
trap 'rm -rf "$TMP_RUN"' EXIT

echo "[1/5] Instantánea y --resumen desde / con ruta absoluta..."
(
    cd /
    export XDG_RUNTIME_DIR="$TMP_RUN"
    sleep 1.5 &
    SPID=$!
    OUT1=$("$BIN" "$SPID")
    echo "$OUT1" | grep -q "TRACE ID:"
    wait "$SPID" 2>/dev/null || true

    sleep 3 &
    SPID2=$!
    OUT2=$("$BIN" --resumen -d 2 "$SPID2")
    echo "$OUT2" | grep -q "resumen_arbol_so"
    wait "$SPID2" 2>/dev/null || true
)
echo "  OK"

echo "[2/5] Dos ejecuciones simultáneas y verificación de permisos 700..."
export XDG_RUNTIME_DIR="$TMP_RUN"
sleep 2 & P1=$!
sleep 2 & P2=$!
"$BIN" "$P1" > "$TMP_RUN/o1.txt" &
T1=$!
"$BIN" "$P2" > "$TMP_RUN/o2.txt" &
T2=$!
sleep 0.08
DIRS=$(find "$TMP_RUN" -maxdepth 1 -type d -name "tracelite-*")
[ -n "$DIRS" ] || { echo "ERROR: no se creó directorio privado"; exit 1; }
for d in $DIRS; do
    PERM=$(stat -c "%a" "$d")
    [ "$PERM" = "700" ] || { echo "ERROR: permiso esperado 700, obtenido $PERM"; exit 1; }
done
wait "$T1" "$T2"
wait "$P1" "$P2" 2>/dev/null || true
grep -q "TRACE ID:" "$TMP_RUN/o1.txt"
grep -q "TRACE ID:" "$TMP_RUN/o2.txt"
[ -z "$(find "$TMP_RUN" -maxdepth 1 -name 'tracelite-*')" ] || { echo "ERROR: quedaron directorios"; exit 1; }
echo "  OK"

echo "[3/5] Cierre limpio tras SIGINT y SIGTERM en instantánea y --resumen..."
for sig in INT TERM; do
    sleep 3 & SP=$!
    "$BIN" "$SP" > /dev/null 2>&1 &
    TP=$!
    sleep 0.06
    kill -s "$sig" "$TP" 2>/dev/null || true
    wait "$TP" 2>/dev/null || true
    wait "$SP" 2>/dev/null || true
    [ -z "$(find "$TMP_RUN" -maxdepth 1 -name 'tracelite-*')" ] || { echo "ERROR: quedó dir tras SIG$sig (inst)"; exit 1; }

    sleep 4 & SP=$!
    "$BIN" --resumen -d 10 "$SP" > /dev/null 2>&1 &
    TP=$!
    sleep 0.2
    kill -s "$sig" "$TP" 2>/dev/null || true
    wait "$TP" 2>/dev/null || true
    wait "$SP" 2>/dev/null || true
    [ -z "$(find "$TMP_RUN" -maxdepth 1 -name 'tracelite-*')" ] || { echo "ERROR: quedó dir tras SIG$sig (resumen)"; exit 1; }
done
echo "  OK"

echo "[4/5] Verificando ausencia de hijos residuales..."
! pgrep -x "collector" >/dev/null 2>&1 || { echo "ERROR: collector vivo"; exit 1; }
! pgrep -x "monitor_pid" >/dev/null 2>&1 || { echo "ERROR: monitor_pid vivo"; exit 1; }
echo "  OK"

echo "[5/5] XDG_RUNTIME_DIR demasiado largo da error claro y no bloquea..."
LARGO="/tmp/$(printf 'a%.0s' {1..110})"
set +e
ERR=$(XDG_RUNTIME_DIR="$LARGO" "$BIN" 1 2>&1)
RC=$?
set -e
[ "$RC" -ne 0 ] || { echo "ERROR: se esperaba código distinto de 0"; exit 1; }
echo "$ERR" | grep -qiE "larga|excede|No such"
echo "  OK ($ERR)"

echo "[OK] prueba_base.sh superada con exito"
