#!/usr/bin/env bash
# Cuatro solicitudes TSP consecutivas. Una de ellas, elegida al azar, usa una
# instancia mayor (n=11) y provoca un incremento aleatorio del tiempo de respuesta.
cd "$(dirname "$0")"
pesada=$(( RANDOM % 4 + 1 ))
echo "[carga] la solicitud pesada sera la numero $pesada"
for i in 1 2 3 4; do
    n=10
    [ "$i" -eq "$pesada" ] && n=11
    echo "[carga] solicitud $i: n=$n"
    python3 tsp_np.py "$n" "$i"
    sleep 0.5
done
echo "[carga] fin"
