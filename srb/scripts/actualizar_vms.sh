#!/usr/bin/env bash
# Actualiza TODAS las VMs desde el repositorio: "git pull" y compila solo lo que cada VM necesita.
# Se ejecuta desde VM1 (o desde cualquier máquina con ssh sin contraseña hacia las demás).
# Flujo de trabajo:  (1) en su máquina modifica srb/ y hace  git add -A && git commit && git push
#                    (2) scripts/actualizar_vms.sh      <- este script
#                    (3) scripts/prueba_empirica.sh parar  y volver a arrancar los servicios
#
# Usa scripts/vms.conf (el mismo de prueba_empirica.sh: USUARIO, RUTA, IP_GT, IP_BD, IP_VM4, IP_VM5).
# Requiere que RUTA sea un clon del repositorio (git clone) en cada VM.
set -u
cd "$(dirname "$0")/.."
CONF=${CONF:-scripts/vms.conf}
# shellcheck disable=SC1090
[ -f "$CONF" ] && . "$CONF"
MODO=${MODO:-ssh}
USUARIO=${USUARIO:-$(whoami)}
RUTA=${RUTA:-$PWD}
IP_GT=${IP_GT:-localhost}; IP_BD=${IP_BD:-localhost}; IP_VM4=${IP_VM4:-localhost}; IP_VM5=${IP_VM5:-localhost}
SSH_OPTS=${SSH_OPTS:--o BatchMode=yes -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new}
HOSTS=$(printf '%s\n' "$IP_GT" "$IP_BD" "$IP_VM4" "$IP_VM5" | awk '!v[$0]++')

usuario_de() {   # por defecto USUARIO; si las VMs son cuentas distintas: USUARIO_GT/BD/VM4/VM5 en vms.conf
    local u=$USUARIO
    [ "$1" = "$IP_BD" ] && u=${USUARIO_BD:-$u}; [ "$1" = "$IP_GT" ] && u=${USUARIO_GT:-$u}
    [ "$1" = "$IP_VM4" ] && u=${USUARIO_VM4:-$u}; [ "$1" = "$IP_VM5" ] && u=${USUARIO_VM5:-$u}
    echo "$u"
}
ruta_de() {      # por defecto RUTA; si la carpeta difiere: RUTA_GT/BD/VM4/VM5 en vms.conf
    local r=$RUTA
    [ "$1" = "$IP_BD" ] && r=${RUTA_BD:-$r}; [ "$1" = "$IP_GT" ] && r=${RUTA_GT:-$r}
    [ "$1" = "$IP_VM4" ] && r=${RUTA_VM4:-$r}; [ "$1" = "$IP_VM5" ] && r=${RUTA_VM5:-$r}
    echo "$r"
}
en() {
    local ip=$1; shift
    if [ "$MODO" = "local" ]; then bash -c "cd '$RUTA' && $*"
    else ssh $SSH_OPTS "$(usuario_de "$ip")@$ip" "cd '$(ruta_de "$ip")' && $*"; fi
}
destinos() {  # binarios que necesita una VM según su rol (sin PostgreSQL salvo en la VM de la BD)
    local ip=$1 t=""
    [ "$ip" = "$IP_BD" ] && t="$t persistencia"
    [ "$ip" = "$IP_GT" ] && t="$t gestor cancelaciones rmc"
    [ "$ip" = "$IP_VM4" ] && t="$t gestor rmc"
    [ "$ip" = "$IP_VM5" ] && t="$t rmc"
    echo $t | tr ' ' '\n' | sort -u | sed 's|^|bin/|' | paste -sd' '
}

mal=0
echo "== Esta máquina (VM1, generador) =="
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git pull --ff-only 2>&1 | sed 's/^/   /'
    errores=$(make bin/generador bin/cliente 2>&1 | grep -E 'error|Error|undefined')
    [ -n "$errores" ] && { echo "$errores" | sed 's/^/   /'; mal=1; }
    echo "   versión: $(cat VERSION 2>/dev/null || echo '?')"
else
    echo "   AVISO: esta carpeta no es un repositorio git; no se hizo pull"; mal=1
fi

for ip in $HOSTS; do
    echo "== $ip =="
    if ! en "$ip" "git rev-parse --is-inside-work-tree" >/dev/null 2>&1; then
        echo "   ERROR: no hay ssh o $(ruta_de "$ip") no es un clon git en esa VM (haga allí: git clone <url> y revise RUTA/RUTA_* en vms.conf)"; mal=1; continue
    fi
    t=$(destinos "$ip")
    salida=$(en "$ip" "git pull --ff-only 2>&1; make $t 2>&1 | grep -E 'error|Error|undefined|not found|No such file|No rule' ; echo \"versión: \$(cat VERSION 2>/dev/null || echo '?')  | binarios: $t\"; for b in $t; do test -x \$b || echo \"FALTA \$b: no se compiló (compruebe: cd \$PWD && make \$b)\"; done")
    echo "$salida" | sed 's/^/   /'
    echo "$salida" | grep -qE "error|Error|undefined|fatal|CONFLICT|not found|No such file|FALTA" && { echo "   >>> REVISAR: hubo errores en esta VM"; mal=1; }
done
echo
[ "$mal" = 0 ] && echo "Todas las VMs actualizadas. Reinicie los servicios (scripts/prueba_empirica.sh parar) para usar los binarios nuevos." \
               || echo "Hubo problemas (ver arriba)."
exit $mal
