#!/usr/bin/env bash
# test1: UNA VM alcanza. Rampa de 16 a 256 clientes, cada uno con POCA carga (T1_RPM solicitudes/min), de modo que
# VM3 sola atiende todo: no debe haber scale-out ni leaky bucket. Veredicto automático al final.
# Uso (desde VM1):   scripts/test1.sh | tee test1.txt
# Ajustes:           T1_PASOS="16 32 64 128 256"  T1_RPM=12  T1_PASO_S=10   (si VM3 pasa del 50 % de CPU: baje T1_RPM)
# Ver la carga sin correr nada:   scripts/test1.sh --mostrar
cd "$(dirname "$0")/.." || exit 1
export T1_PASOS="${T1_PASOS:-16 32 64 128 256}"
export T1_RPM="${T1_RPM:-12}"
if [ "${1:-}" = "--mostrar" ]; then
    echo "test1 - clientes por paso: $T1_PASOS   solicitudes/min por cliente: $T1_RPM   segundos por paso: ${T1_PASO_S:-10}"
    echo "$T1_PASOS" | tr ' ' '\n' | awk -v rpm="$T1_RPM" -v s="${T1_PASO_S:-10}" \
        '{r=$1*rpm/60; t+=r*s; c+=$1; printf "  %4d clientes -> %6.1f solicitudes/s ofrecidas (%d en el paso)\n",$1,r,r*s} END{printf "  TOTAL: %d clientes en %d pasos, ~%d solicitudes\n",c,NR,t}'
    exit 0
fi
exec bash scripts/prueba_empirica.sh test1 "$@"
