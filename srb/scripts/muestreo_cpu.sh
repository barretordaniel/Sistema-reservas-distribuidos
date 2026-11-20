#!/usr/bin/env bash
# Mide cuánta CPU consume cada componente del SRB en ESTA máquina durante una ventana de tiempo.
# Lo lanza prueba_empirica.sh por ssh en cada VM; también se puede usar a mano.
#
# Uso: scripts/muestreo_cpu.sh <espera_s> <ventana_s> <etiqueta_nodo>
# Salida (una línea por componente + una del nodo):
#   MUESTREO <nodo> <componente> <cpu_s> <pct_de_un_nucleo>
#   NODO <nodo> <cpu_total_pct> <iowait_pct> <nucleos>
set -u
export LC_ALL=C
ESPERA=${1:-0}; VENTANA=${2:-10}; NODO=${3:-$(hostname)}
CLK=$(getconf CLK_TCK)
NPROC=$(nproc)

# ticks acumulados (usuario+sistema) por nombre de proceso; PostgreSQL suma todos sus procesos
instantanea() {
    for p in /proc/[0-9]*; do
        c=$(cat "$p/comm" 2>/dev/null) || continue
        case "$c" in
            persistencia|gestor|rmc|cancelaciones|generador|postgres) awk -v c="$c" '{print c, $14+$15}' "$p/stat" 2>/dev/null ;;
        esac
    done | awk '{a[$1]+=$2} END{for(k in a) print k, a[k]}' | sort
}
# aritmética entera en bash: los contadores de /proc/stat pasan de 2^31 y algunos awk los imprimen con 6 cifras
cpu_global() {
    local x u n s i w irq sirq st
    read -r x u n s i w irq sirq st _ < /proc/stat
    echo "$((u + n + s + irq + sirq + st)) $((u + n + s + irq + sirq + st + i + w)) $w"
}

sleep "$ESPERA"
instantanea > /tmp/.srb_muestreo_a.$$
read -r b0 t0 w0 < <(cpu_global)
sleep "$VENTANA"
instantanea > /tmp/.srb_muestreo_b.$$
read -r b1 t1 w1 < <(cpu_global)

join -a1 -a2 -e 0 -o 0,1.2,2.2 /tmp/.srb_muestreo_a.$$ /tmp/.srb_muestreo_b.$$ | \
    awk -v clk="$CLK" -v v="$VENTANA" -v n="$NODO" '{d=($3>$2)?$3-$2:0; s=d/clk; printf "MUESTREO %s %s %.2f %.0f\n", n, $1, s, 100*s/v}'
db=$((b1 - b0)); dt=$((t1 - t0)); dw=$((w1 - w0))
if [ "$dt" -gt 0 ] && [ "$db" -ge 0 ]; then
    echo "NODO $NODO $(( (200 * db + dt) / (2 * dt) )) $(( (200 * dw + dt) / (2 * dt) )) $NPROC"
else
    echo "NODO $NODO nan nan $NPROC"
fi
rm -f /tmp/.srb_muestreo_a.$$ /tmp/.srb_muestreo_b.$$
