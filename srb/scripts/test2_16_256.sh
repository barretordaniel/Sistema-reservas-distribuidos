#!/usr/bin/env bash
# test2 con una lista fija de clientes: 16, 32, 64, 128 y 256 (pasos de T2_PASO_S segundos, 10 por defecto).
# Uso (desde VM1, en la carpeta del proyecto):
#     scripts/test2_16_256.sh | tee test2.txt
# Otra lista:   T2_PASOS="16 64 256" scripts/test2_16_256.sh | tee test2.txt
# Revisar sin correr nada:   scripts/test2_16_256.sh --mostrar
# Funciona aunque el script esté en otra carpeta: busca la raíz del proyecto (la que tiene scripts/prueba_empirica.sh).

LISTA="${T2_PASOS:-16 32 64 128 256}"

raiz=""
for base in "$(dirname "$0")" "$PWD" "$HOME/vm1/proyecto-distribuidos/srb"; do
    d=$(cd "$base" 2>/dev/null && pwd) || continue
    while [ -n "$d" ] && [ "$d" != "/" ]; do
        if [ -f "$d/scripts/prueba_empirica.sh" ]; then raiz=$d; break 2; fi
        d=$(dirname "$d")
    done
done
if [ -z "$raiz" ]; then
    echo "No encuentro la carpeta del proyecto (la que tiene scripts/prueba_empirica.sh)."
    echo "Ejecute el script desde esa carpeta:  cd ~/vm1/proyecto-distribuidos/srb && scripts/test2_16_256.sh"
    exit 1
fi

if [ "${1:-}" = "--mostrar" ]; then
    echo "Carpeta del proyecto : $raiz"
    echo "Clientes por paso    : $LISTA"
    exit 0
fi

cd "$raiz" || exit 1
export T2_PASOS="$LISTA"
exec bash "$raiz/scripts/prueba_empirica.sh" test2 "$@"   # directo: no llama a test2.sh (así no importa qué tenga ese archivo)
