#!/usr/bin/env bash
# arreglar_tests.sh - deja test1 y test2 listos (SRB): repara permisos y fija la rampa de 16 a 256 clientes.
#
# Uso (desde VM1, no necesita permiso de ejecución):
#     bash arreglar_tests.sh [CARPETA_DEL_PROYECTO]
# Qué hace:
#   1. Encuentra la carpeta del proyecto (la que tiene scripts/prueba_empirica.sh).
#   2. Reescribe scripts/test1.sh y scripts/test2.sh (guarda copia de lo anterior en scripts/test1.sh.bak / test2.sh.bak):
#        test1 = 16 32 64 128 256 clientes con carga LIGERA (12 solicitudes/min cada uno -> máx. 51 solicitudes/s):
#                una VM alcanza, sin elasticidad.
#        test2 = 16 32 64 128 256 clientes SIN pausa: sube hasta ocupar las 3 VMs y el leaky bucket.
#   3. Pone permiso de ejecución a todos los scripts (soluciona "Permission denied") y quita saltos de línea de Windows.
# Se puede ejecutar varias veces sin problema.
set -u
raiz="${1:-}"
if [ -z "$raiz" ]; then
    for base in "$PWD" "$(dirname "$0")" "$HOME/vm1/proyecto-distribuidos/srb"; do
        d=$(cd "$base" 2>/dev/null && pwd) || continue
        while [ -n "$d" ] && [ "$d" != "/" ]; do
            if [ -f "$d/scripts/prueba_empirica.sh" ]; then raiz=$d; break 2; fi
            d=$(dirname "$d")
        done
    done
fi
if [ -z "$raiz" ] || [ ! -f "$raiz/scripts/prueba_empirica.sh" ]; then
    echo "No encuentro la carpeta del proyecto (la que tiene scripts/prueba_empirica.sh)."
    echo "Use:  cd ~/vm1/proyecto-distribuidos/srb && bash arreglar_tests.sh"
    exit 1
fi
cd "$raiz" || exit 1
echo "Proyecto: $raiz"

for f in test1 test2; do
    [ -f "scripts/$f.sh" ] && [ ! -f "scripts/$f.sh.bak" ] && cp -p "scripts/$f.sh" "scripts/$f.sh.bak"
done

cat > scripts/test1.sh <<'FIN_TEST1'
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
FIN_TEST1

cat > scripts/test2.sh <<'FIN_TEST2'
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
FIN_TEST2

sed -i 's/\r$//' scripts/*.sh 2>/dev/null
chmod +x scripts/*.sh
mal=0
for f in scripts/test1.sh scripts/test2.sh scripts/prueba_empirica.sh; do
    bash -n "$f" || { echo "ERROR de sintaxis en $f"; mal=1; }
done
[ "$mal" = 0 ] || exit 1
echo "Listo: permisos reparados y test1/test2 con la rampa 16 -> 256."
echo
scripts/test1.sh --mostrar
echo
scripts/test2.sh --mostrar
echo
echo "Ahora:   scripts/test1.sh | tee test1.txt      y      scripts/test2.sh | tee test2.txt"
