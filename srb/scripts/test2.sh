#!/usr/bin/env bash
# test2: la carga crece de 16 a 256 clientes SIN pausa hasta ocupar las 3 VMs (scale-out a VM4 y VM5 + leaky bucket).
# Uso (desde VM1):   scripts/test2.sh | tee test2.txt
# Ajustes:           T2_PASOS="16 32 64 128 256"  T2_PASO_S=10  CARGA_T=10  CUBETA_T2=40
# Ver la carga sin correr nada:   scripts/test2.sh --mostrar
cd "$(dirname "$0")/.." || exit 1
export T2_PASOS="${T2_PASOS:-16 32 64 128 256}"
if [ "${1:-}" = "--mostrar" ]; then
    echo "test2 - clientes por paso: $T2_PASOS (sin pausa entre solicitudes)   segundos por paso: ${T2_PASO_S:-10}"
    echo "$T2_PASOS" | tr ' ' '\n' | awk '{c+=$1; printf "  %4d clientes\n",$1} END{printf "  TOTAL: %d clientes en %d pasos (pico %d)\n",c,NR,m}' m="$(echo "$T2_PASOS" | tr ' ' '\n' | sort -n | tail -1)"
    exit 0
fi
exec bash scripts/prueba_empirica.sh test2 "$@"
