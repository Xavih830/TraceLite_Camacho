#!/usr/bin/env python3
"""Problema del viajante (TSP) por fuerza bruta: evalua las (n-1)! rutas posibles.

Uso: python3 tsp_np.py <n_ciudades> [semilla]
Se usa como carga NP-hard en las pruebas de TraceLite: pasar de n=10 a n=11
multiplica el tiempo de respuesta aproximadamente por 10.
"""
import itertools
import math
import random
import sys
import time

n = int(sys.argv[1]) if len(sys.argv) > 1 else 10
semilla = int(sys.argv[2]) if len(sys.argv) > 2 else 2026
random.seed(semilla)

ciudades = [(random.uniform(0, 100), random.uniform(0, 100)) for _ in range(n)]
dist = [[math.dist(a, b) for b in ciudades] for a in ciudades]

t0 = time.perf_counter()
mejor_costo, mejor_ruta = float("inf"), None
for perm in itertools.permutations(range(1, n)):
    costo = dist[0][perm[0]] + dist[perm[-1]][0]
    for i in range(n - 2):
        costo += dist[perm[i]][perm[i + 1]]
    if costo < mejor_costo:
        mejor_costo, mejor_ruta = costo, perm

print(f"[tsp] n={n} rutas={math.factorial(n - 1)} costo={mejor_costo:.2f} "
      f"tiempo={time.perf_counter() - t0:.2f} s")
