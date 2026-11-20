#!/usr/bin/env bash
# Serie de la CPU de ESTA máquina (% de todos sus núcleos), una muestra por segundo, en formato CSV "t_s,cpu_pct".
# La usa prueba_empirica.sh escalamiento en VM1 y en la VM de la BD (las demás informan su CPU al GT).
# Toda la aritmética es entera en bash (sin awk): los contadores de /proc/stat pasan de 2^31 en máquinas con
# muchos días encendidas y algunos awk (mawk antiguo) los imprimen con 6 cifras y falsean la diferencia.
# Si una muestra no se puede calcular escribe "nan" (el análisis la ignora) en vez de un valor inventado.
#
# Uso: scripts/cpu_serie.sh <segundos>
export LC_ALL=C
N=${1:-60}
leer() {  # deja en B y T los jiffies ocupados y totales (usuario+nice+sistema+irq+softirq+steal / + idle + iowait)
    local x u n s i w irq sirq st
    read -r x u n s i w irq sirq st _ < /proc/stat
    B=$((u + n + s + irq + sirq + st)); T=$((B + i + w))
}
echo "t_s,cpu_pct"
leer; b0=$B; t0=$T
for ((i = 1; i <= N; i++)); do
    sleep 1
    leer
    db=$((B - b0)); dt=$((T - t0))
    if [ "$dt" -gt 0 ] && [ "$db" -ge 0 ] && [ "$db" -le "$dt" ]; then
        v=$((1000 * db / dt))
        printf '%d,%d.%d\n' "$i" $((v / 10)) $((v % 10))
    else
        printf '%d,nan\n' "$i"
    fi
    b0=$B; t0=$T
done
