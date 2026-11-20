# SRB – Sistema de Reserva de Boletos (segunda entrega: escalable y tolerante a fallas)

Pontificia Universidad Javeriana · Introducción a Sistemas Distribuidos · 2026-30.
C++17 + ZeroMQ + PostgreSQL.

| Documento | Qué contiene |
|---|---|
| **`README.md`** (este archivo) | Cómo instalar, configurar, ejecutar y medir |
| **`ARQUITECTURA_FINAL.md`** | Cómo está compuesto el proyecto, patrones nuevos, mejoras frente a la primera entrega, ventajas y desventajas |
| **`CAMBIOS.md`** | Qué cambió en cada versión y por qué |
| `DISENO_Y_PRUEBAS.txt` | Diseño detallado y protocolo de pruebas (para el informe) |

Operaciones: **consulta, reserva, modificación** (número de asientos o cambio de fecha a otra ocurrencia del mismo evento en el
mismo mes) — síncronas — y **cancelación** — asíncrona, con notificación al cliente cuando se efectúa en la BD.

## Topología (5 VMs)

| VM | Capa | Procesos | Puertos |
|---|---|---|---|
| **VM1** | Consumidores **y control** | `bin/generador` / `bin/cliente`; desde aquí se arranca, mide y detiene todo por ssh | — |
| **VM2** | Almacenamiento | PostgreSQL + `bin/persistencia` | 5559 |
| **VM3** | Lógica de negocio | `bin/gestor` (GT) + `bin/cancelaciones` + `bin/rmc` (nivel `base`) | 5555, 5556, 5557, 5558 |
| **VM4** | Lógica de negocio | `bin/rmc` (nivel `vm4`); en la variante `escalable` también el GT de relevo | 6555, 6556 (solo relevo) |
| **VM5** | Lógica de negocio | `bin/rmc` (nivel `vm5`) | — |

**VM1 manda los procesos a las otras cuatro:** con `scripts/prueba_empirica.sh` (y `test1`, `test2`) arranca por ssh la BD en VM2,
el GT/Cancelaciones/RMC base en VM3 y los RMC de VM4 y VM5; genera la carga desde VM1 y recoge los resultados. Los procesos corren
cada uno en su VM. Con el mínimo del enunciado (3 máquinas) se omiten VM4/VM5.

Los RMC **no escuchan**: se conectan ellos al GT (y a la persistencia) y avisan `READY`. Por eso en VM4/VM5 no hay puertos de
entrada que abrir para el RMC.

## Guía rápida: de cero a las pruebas (hágalo en este orden)

**1. Compilar en cada VM** (los binarios no se copian entre máquinas):

```bash
sudo apt install -y build-essential libzmq3-dev            # todas las VMs
sudo apt install -y libpq-dev libpqxx-dev postgresql       # solo VM2 (BD) y VM1 si compila todo
# cppzmq: dos headers (zmq.hpp y zmq_addon.hpp). Si la VM no los tiene, cópielos a srb/src/ (el Makefile usa -Isrc):
scp /usr/include/zmq.hpp /usr/include/zmq_addon.hpp USUARIO@IP_VM:~/RUTA/srb/src/
cd ~/RUTA/srb && make                    # VM1 y VM2: todo   ·   VM3: make bin/gestor bin/rmc bin/cancelaciones
                                         # VM4: make bin/gestor bin/rmc   ·   VM5: make bin/rmc
```

En VM2, un rol de PostgreSQL con el mismo usuario de Linux (autenticación por socket, sin contraseña):

```bash
sudo -u postgres createuser $USER && sudo -u postgres createdb -O $USER srb
psql -d srb -c "select 1"                # debe imprimir 1
```

**2. Mantener las 5 VMs con la misma versión** (en la máquina donde se edita, luego desde VM1):

```bash
python3 aplicar_cambios.py ~/RUTA/srb    # si recibió un instalador nuevo (un solo archivo; hace respaldo y no borra nada)
git add -A && git commit -m "SRB vX" && git push
scripts/actualizar_vms.sh                # desde VM1: git pull + make de lo que necesita cada VM; muestra la versión de cada una
```

**3. Configurar VM1** (una sola vez): ssh sin contraseña a VM2..VM5 y `scripts/vms.conf`:

```bash
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
for ip in IP_VM2 IP_VM3 IP_VM4 IP_VM5; do ssh-copy-id USUARIO@$ip; done      # con las IP reales
cp scripts/vms.conf.ejemplo scripts/vms.conf && nano scripts/vms.conf
```

`scripts/vms.conf` (ejemplo con el laboratorio NBIG; cada VM puede tener su propio usuario y carpeta):

```bash
USUARIO=estudiante
RUTA=/home/estudiante/vm1/proyecto-distribuidos/srb          # carpeta del proyecto en VM1 (y valor por defecto)
IP_GT=10.43.99.204   IP_BD=10.43.99.193   IP_VM4=10.43.99.188   IP_VM5=10.43.99.195     # VM3, VM2, VM4, VM5
RUTA_BD=/home/estudiante/vm2/proyecto-distribuidos/srb
RUTA_GT=/home/estudiante/vm3/proyecto-distribuidos/srb
RUTA_VM4=/home/estudiante/vm4/proyecto-distribuidos/srb
RUTA_VM5=/home/estudiante/vm5/proyecto-distribuidos/srb
ET_GT="VM3-NBIG263"  ET_BD="VM2-NBIG252"  ET_VM4="VM4-NBIG247"  ET_VM5="VM5-NBIG254"  ET_GEN="VM1-NBIG250"   # etiquetas de los informes
```

**4. Comprobar** (desde VM1):

```bash
scripts/prueba_empirica.sh mapa          # quién es quién
scripts/prueba_empirica.sh verificar     # ssh, binarios compilados, puertos (los puertos salen "cerrados" hasta arrancar el SRB)
```

**5. Abrir los monitores** (una terminal por VM; son opcionales pero son lo que se muestra en la sustentación):

```bash
scripts/monitor_vm.sh        # en VM3, VM4 y VM5: CPU total y por proceso, RAM, procesos vivos
scripts/monitor_bd.sh        # en VM2: peticiones en curso, transacciones/s, RAM%, CPU
```

**6. Correr las pruebas** (desde VM1; cada una arranca la topología, genera la carga, da el veredicto y deja la topología arriba):

```bash
scripts/test1.sh | tee test1.txt         # UNA VM alcanza: carga ligera, VM3 sola, sin scale-out        (~1 min)
scripts/test2.sh | tee test2.txt         # carga creciente hasta ocupar VM3+VM4+VM5 y el leaky bucket  (~3 min)
scripts/prueba_empirica.sh parar         # detiene todo en las 5 VMs
```

**Contador en tiempo real (en VM1).** Mientras corre cada paso, la terminal de VM1 muestra una línea por segundo con lo que
están haciendo los clientes en ese instante:

```
>>> paso 3 de 11: 8 clientes durante 10s (solicitudes en vivo, una línea por segundo)
   21:34:41 EN VIVO t=  7s clientes=8 | enviadas=1530 respondidas=1522 ahora=218/s en_curso=8 | consulta=470 reserva=760 modif=292 | rechazadas=0 fallas=0
```

`enviadas`/`respondidas` son acumuladas del paso, `ahora` es el ritmo del último segundo, `en_curso` las que esperan respuesta,
`rechazadas` las que el leaky bucket del GT rechazó con `SOBRECARGA` y `fallas` los timeouts y errores. Al terminar el paso sale
una línea `FIN` con el ritmo medio. Estas líneas salen por **stderr**: solo se ven en pantalla y `| tee test1.txt` no las guarda
(use `2>&1 | tee` si las quiere en el archivo). La línea de cada paso queda siempre en `resultados_test*/paso*.gen.log`.
`EN_VIVO=0` las apaga; `EN_VIVO=2` imprime cada 2 s. El mismo contador existe en el generador:
`bin/generador ... --en-vivo 1`.

Los resultados quedan en `resultados_test1/` y `resultados_test2/`: `escalamiento.png` (gráfico de 4 paneles), `veredicto.md`,
`escalamiento_resumen.md`, `escala_gt.csv` (una fila por segundo), `pasos_resumen.csv`, `config.txt` y las CPU de VM1 y de la BD.
Para llevarse el PNG a su computador: `git add -f resultados_test2/escalamiento.png && git commit -m png && git push` (los
`resultados*/` están en `.gitignore`, por eso `-f`) o, desde una terminal de **su computador**,
`scp USUARIO@IP_VM1:~/RUTA/srb/resultados_test2/escalamiento.png .`

### Qué hacen test1 y test2

| | test1 | test2 |
|---|---|---|
| Objetivo | Con poco tráfico **una VM alcanza**: no debe haber scale-out | Con tráfico creciente se **ocupan las 3 VMs** |
| Carga | 3, 6 y 9 clientes a 600 solicitudes/min cada uno (10 s por paso) | 2, 4, 8, 12, 16, 24, 32, 48, 64, 96 y 128 clientes sin pausa (10 s por paso) + 20 s de reposo |
| Debe verse | `niveles_activos = 1`, VM3 bajo 50 % de CPU, VM4/VM5 sin trabajo, sin cubeta, sin errores, p95 < 1 s | Orden 1 → 2 → 3 → 2 → 1, las 3 VMs al límite, más ops/s con más VMs, cubeta con rechazos `SOBRECARGA`, sin errores |
| Veredicto | `APROBADO` / `REVISAR` en `veredicto.md` | igual |

**Velocidad.** Desde v3.5 las dos pruebas son 3 veces más rápidas: pasos de 10 s (antes 30 s) y escalado más ágil (`T_SOSTENIDO_IN=5`,
`T_ENFRIAR_S=5`; antes 10 s y 8 s). La versión lenta de antes:
`T1_PASO_S=30 T2_PASO_S=30 T1_REPOSO_S=15 T2_REPOSO_S=45 T_SOSTENIDO_IN=10 T_ENFRIAR_S=8 scripts/test2.sh`.

Ajustes: `T1_RPM`, `T1_PASOS`, `T1_PASO_S`; `T2_PASOS`, `T2_PASO_S`, `T2_REPOSO_S`, `CUBETA_T2`; `CARGA_T` (ms de CPU por solicitud) y
`HILOS_T` (hilos de RMC por VM) para ambas. Ejemplo: `T1_RPM=300 scripts/test1.sh`.

**Por qué hay carga sintética.** Con el RMC real, cada solicitud cuesta pocos ms de CPU en VM4/VM5 y la BD se satura primero (~1100
ops/s con la BD al 70–90 %), así que VM4 y VM5 nunca pasarían del 25 % y "ocupar las 3 VMs" no sería alcanzable. `CARGA_T`
(por defecto 10 ms) hace que cada solicitud consuma ese tiempo de CPU en el RMC —simula lógica de negocio costosa— y mueve el
cuello de botella a la capa lógica, que es lo que el escalado puede aliviar. Con `CARGA_T=0` se mide el RMC sin carga extra. El
informe debe decir que `test1` y `test2` usan esta carga sintética.

**Si el veredicto sale en REVISAR:** (test1) la VM pasó del umbral → baje `T1_RPM`; (test2) una VM no llegó al 90 % → suba `CARGA_T`
o `T2_PASOS`; si la BD llegó a ≥ 85 % su techo es de la BD, no de la capa lógica; si la cubeta no rechazó nada → baje `CUBETA_T2`.

## Estructura

```
srb/
├── Makefile
├── README.md  ARQUITECTURA_FINAL.md  CAMBIOS.md  DISENO_Y_PRUEBAS.txt  VERSION
├── ejemplos/caso_ejemplo.txt      operaciones de ejemplo para bin/cliente ("caso del profesor")
├── analisis/
│   ├── analisis.py                Tabla 1 y gráficos (pandas + matplotlib) a partir de los CSV
│   ├── escalamiento.py            gráfico de 4 paneles y resumen por paso (scale-out/in + leaky bucket)
│   └── veredicto.py               veredicto automático de test1 y test2
├── scripts/
│   ├── prueba_empirica.sh         DESDE VM1: arranca la topología por ssh, genera carga, mide; subcomandos mapa, verificar,
│   │                              correr, escalamiento, test1, test2, procedimientos, parar
│   ├── test1.sh  test2.sh         atajos de prueba_empirica.sh test1 / test2
│   ├── monitor_vm.sh              en VM3/VM4/VM5: CPU y RAM en vivo
│   ├── monitor_bd.sh              en VM2: peticiones en curso, tx/s y RAM en vivo
│   ├── pruebas_automaticas.sh     batería completa en UNA máquina (incluye pruebas_escalado.sh)
│   ├── pruebas_escalado.sh        escalado, leaky bucket, carga sintética, monitores y contador en vivo (41 comprobaciones)
│   ├── prueba_local.sh            demostración rápida de toda la arquitectura en una máquina
│   ├── tabla1.sh                  corridas de la Tabla 1 (base/escalable; 3, 5, 8 clientes; N repeticiones)
│   ├── muestreo_cpu.sh  cpu_serie.sh   medición de CPU (los usa prueba_empirica.sh)
│   ├── quien_soy.sh               en cada VM: dice qué máquina es y qué rol cumple
│   ├── actualizar_vms.sh          git pull + compilar en todas las VMs (desde VM1)
│   ├── vms.conf.ejemplo           IP de las VMs (copiar a vms.conf)
│   └── supervisor.sh              relanza un servicio si muere
├── src/
│   ├── common.hpp                 Mensaje, serialización, EnlaceGT (Lazy Pirate), MedidorCPU, utilidades ZeroMQ
│   ├── gestor_transacciones.cpp   GestorTransacciones  (ROUTER clientes, ROUTER RMC, PUSH, escalado y leaky bucket)
│   ├── servicio_rmc.cpp           ServicioRMC          (DEALER -> GT, REQ -> Persistencia, --carga-cpu-ms)
│   ├── servicio_persistencia.cpp  ServicioPersistencia (ROUTER/DEALER + hilos REP + PostgreSQL)
│   ├── servicio_cancelaciones.cpp ServicioCancelaciones(PULL <- GT, REQ -> Persistencia, PUB)
│   ├── generador_carga.cpp        GeneradorCarga, Cliente, Metricas
│   └── cliente_archivo.cpp        cliente que ejecuta un archivo de operaciones
└── tests/
    ├── test_mensaje.cpp           18 pruebas unitarias de la serialización
    └── prueba_funcional.cpp       41 pruebas de extremo a extremo
```

## Pruebas en una sola máquina (sin desplegar)

```bash
make
scripts/pruebas_automaticas.sh 15     # batería completa; 15 s por prueba de carga (~3 min)
scripts/prueba_local.sh 20 8 4        # demo: 20 s, 8 clientes, 4 solo consulta, 2 "VMs" de RMC y 2 de generador
```

## Arranque manual (sin `prueba_empirica.sh`)

Útil para depurar un componente o para la demostración de fallas. Una terminal por proceso (o `nohup … &`). Defina primero las
IP **reales** en cada máquina (no deje `IP_...` literal: ZeroMQ no resuelve ese nombre y los procesos "se quedan esperando"):

```bash
export IP_GT=10.x.x.x   IP_BD=10.x.x.x   IP_RELEVO=10.x.x.x        # VM3, VM2, VM4
```

Orden de arranque: **VM2 → VM3 → VM4/VM5 → VM1**. Abra en el firewall: VM3 → 5555, 5556, 5557, 5558 · VM2 → 5559 · VM4 → 6555, 6556.

```bash
# ---- VM2: almacenamiento -------------------------------------------------------------
./bin/persistencia --reset --hilos 4                      # --reset recarga los 50 eventos

# ---- VM3: entrada del negocio --------------------------------------------------------
./bin/cancelaciones --persistencia tcp://$IP_BD:5559
./bin/gestor --nombre GT1 --cancel tcp://localhost:5557 --escalar base,vm4,vm5 --linea-tiempo logs/escala.csv
./bin/rmc --nombre base --hilos 4 --gt tcp://localhost:5556 --persistencia tcp://$IP_BD:5559

# ---- VM4 y VM5: réplicas (se registran solas en el GT) --------------------------------
scripts/supervisor.sh ./bin/rmc --nombre vm4 --hilos 4 --gt tcp://$IP_GT:5556 --persistencia tcp://$IP_BD:5559    # VM4
scripts/supervisor.sh ./bin/rmc --nombre vm5 --hilos 4 --gt tcp://$IP_GT:5556 --persistencia tcp://$IP_BD:5559    # VM5

# ---- VM1: clientes (8 clientes, 4 solo consulta, 120 s) -----------------------------------
./bin/generador --gt tcp://$IP_GT:5555 --notif tcp://$IP_GT:5558 \
        --clientes 8 --solo-consulta 4 --rpm 60 --duracion 120 --calentamiento 10 --csv resultados.csv
```

**Variante `escalable` (con GT de relevo, para la demostración de fallas):** sin `--escalar`, un segundo GT en VM4 y los RMC de
VM4 y VM5 registrados en ambos GT:

```bash
# VM3: ./bin/gestor --nombre GT1 --cancel tcp://localhost:5557           (sin --escalar)
# VM4: ./bin/gestor --nombre GT2 --bind tcp://*:6555 --bind-rmc tcp://*:6556 --cancel tcp://$IP_GT:5557
scripts/supervisor.sh ./bin/rmc --nombre vm4 --hilos 2 \
        --gt tcp://$IP_GT:5556,tcp://$IP_RELEVO:6556 --persistencia tcp://$IP_BD:5559      # VM4 (y vm5 en VM5)
./bin/generador --gt tcp://$IP_GT:5555,tcp://$IP_RELEVO:6555 --notif tcp://$IP_GT:5558 ...  # VM1
```

**Generador repartido en 2 VMs** (los clientes 1..N se reparten por paridad; los ids no se pisan):
`VM1: … --vm-indice 0 --vm-total 2` y `VM5: … --vm-indice 1 --vm-total 2` (resto de argumentos iguales).

## Mantener todas las VMs al día (git)

Un solo clon por VM. Cuando cambie algo: `git add -A && git commit && git push` en la máquina donde edita y, desde VM1,
`scripts/actualizar_vms.sh` (git pull + make de lo que necesita cada VM + versión de cada una; avisa con `FALTA` si algo no
compiló). Después `scripts/prueba_empirica.sh parar` y volver a arrancar para usar los binarios nuevos. Los binarios (`bin/`),
logs, resultados y `scripts/vms.conf` NO van al repositorio (`.gitignore`). A mano, en una VM: `git pull && make bin/rmc`.

## ¿Qué máquina es cuál? (`quien_soy.sh`)

En cada VM (no necesita configuración; funciona aunque no haya nada corriendo):

```bash
cd ~/srb && scripts/quien_soy.sh          # nombre, IP, procesos, puertos en escucha y ROL deducido
scripts/quien_soy.sh --fijar VM4          # si no hay nada corriendo, anota una vez el rol en ~/.srb_rol
```

El rol se deduce de lo que corre: PostgreSQL/persistencia = VM2; `gestor` con puerto 5555 = GT principal (VM3);
`gestor` en 6555/6556 = GT de relevo; `rmc --nombre vm4` = réplica A; `generador` = VM1. Desde VM1 se ven todas
de una vez con `scripts/prueba_empirica.sh mapa` (necesita ssh sin contraseña y `scripts/vms.conf`).

## Preparar VM4 y VM5 (las réplicas de RMC)

**Los RMC no "escuchan" la carga: se conectan ellos al GT** (y a la persistencia) y avisan `READY`; el GT les envía
trabajo. Por eso en VM4/VM5 no hay puertos de entrada que abrir para el RMC (solo para el GT de relevo en VM4), y
el GT incorpora cualquier RMC nuevo en el momento en que arranca, incluso en plena carga.

1. **Copiar el proyecto** (desde VM1, o desde donde esté `srb/`). Si VM4/VM5 no tienen `zmq.hpp` en
   `/usr/local/include`, cópielo dentro de `srb/src/` (el Makefile usa `-Isrc`):
   ```bash
   cp /usr/local/include/zmq.hpp /usr/local/include/zmq_addon.hpp ~/srb/src/     # en la VM que los tenga
   rsync -a --exclude bin --exclude logs ~/srb/ USUARIO@IP_VM4:~/srb/              # idem para IP_VM5
   ```
2. **Compilar solo lo que esa VM usa** (no necesita PostgreSQL ni libpqxx; si falta `zmq.h`, instale `libzmq3-dev`):
   ```bash
   sudo apt install -y build-essential libzmq3-dev
   cd ~/srb && make bin/rmc bin/gestor          # VM4;  en VM5 basta  make bin/rmc
   ```
3. **Firewall** (si `sudo ufw status` dice *active*; en nubes con grupos de seguridad ábralos allí):
   `VM4: sudo ufw allow 6555/tcp && sudo ufw allow 6556/tcp` (solo para el GT de relevo) · `VM3: 5555 5556 5557 5558` · `VM2: 5559`.
4. **Comprobar que VM4/VM5 alcanzan al GT y a la persistencia** (con el GT y la persistencia ya arrancados):
   ```bash
   timeout 3 bash -c "</dev/tcp/$IP_GT/5556" && echo "GT ok"        # debe decir ok
   timeout 3 bash -c "</dev/tcp/$IP_BD/5559" && echo "BD ok"
   ```
5. **Arrancar el RMC** (comando completo en "Arranque manual"). Para saber que quedó bien:
   en VM3 `grep "RMC registrado" logs/gt1.log` (una línea por hilo: `vm4/h0`, `vm4/h1`…) y en VM4 `scripts/quien_soy.sh`.
6. **Dar de alta la carga en las dos a la vez**: no hay nada más que configurar; el reparto es automático (LRU) y el GT
   lo imprime: `kill -USR1 $(pgrep -f '[b]in/gestor')` → `reparto entre VMs: vm4=… (50%) vm5=… (50%)`.

## Prueba empírica desde el nodo generador (VM1)

Qué consume más CPU y si VM4/VM5 ayudan: un solo comando por arquitectura, ejecutado en VM1.

```bash
# 0) ssh sin contraseña desde VM1 hacia VM2..VM5 (una sola vez)
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
for ip in IP_VM2 IP_VM3 IP_VM4 IP_VM5; do ssh-copy-id USUARIO@$ip; done      # con las IP reales
# 1) configuración
cd ~/srb && cp scripts/vms.conf.ejemplo scripts/vms.conf && nano scripts/vms.conf   # IP reales y USUARIO/RUTA
# 2) comprobaciones
scripts/prueba_empirica.sh mapa           # quién es quién
scripts/prueba_empirica.sh verificar      # ssh, binarios, puertos
# 3) corridas (CLIENTES="3 5 8" por defecto; DUR=60 s medidos; ~1.5 min por escenario)
scripts/prueba_empirica.sh correr base      | tee emp_base.txt
scripts/prueba_empirica.sh correr escalable | tee emp_escalable.txt
scripts/prueba_empirica.sh correr elastica  | tee emp_elastica.txt     # VM4/VM5 se unen en plena carga
scripts/prueba_empirica.sh parar
```

Por cada escenario imprime: tiempos y TPM vistos por los clientes, reparto de solicitudes entre VMs (según el GT),
**CPU de cada componente en cada nodo** (`gestor`, `rmc`, `persistencia`, `postgres`, `generador`), CPU total del nodo
con aviso si pasa de 85 % (saturado), **ms de CPU por transacción** y el componente que más CPU consume.
En `elastica` muestra además los ops/s cada 5 s para ver el salto cuando se unen las réplicas. Los CSV de muestras
quedan en `resultados_empiricos/` y sirven para `analisis/analisis.py` (Tabla 1 y gráficos).

Cómo leerlo: si `postgres`+`persistencia` suman la mayor parte de la CPU y el nodo de la BD (VM2) se acerca al 100 %, el cuello
de botella es la base de datos y más RMC no mejora más; si VM2 tiene CPU de sobra y el throughput sube al añadir
trabajadores, el límite era la concurrencia del RMC (cada solicitud ocupa un trabajador mientras espera a la BD).

### Variante de 3 VMs (VM1 carga · VM2 BD · VM3 capa lógica) y cuello de botella por procedimiento

Si toda la capa lógica (GT, RMC y cancelaciones) está en **una** VM, ponga en `scripts/vms.conf` las IP de esa VM en
`IP_GT`, `IP_VM4` e `IP_VM5`; la de la BD en `IP_BD`; y las etiquetas que saldrán en los informes:

```bash
IP_GT=<IP VM3>  IP_BD=<IP VM2>  IP_VM4=<IP VM3>  IP_VM5=<IP VM3>
ET_GT="VM3-logica"  ET_BD="VM2-BD"  ET_VM4=""  ET_VM5=""  ET_GEN="VM1-carga"
```

`correr escalable` arranca entonces el GT de relevo y dos réplicas RMC (`vm4`, `vm5`) en esa misma VM, así que mide si
más réplicas ayudan cuando comparten los núcleos de la VM lógica. La CPU de **VM1** también se mide: si el generador se
acerca al 85 %, el límite sería la carga y no el SRB.

```bash
DUR=40 CAL=8 CLIENTES_PROC=8 scripts/prueba_empirica.sh procedimientos base      | tee proc_base.txt
DUR=40 CAL=8 CLIENTES_PROC=8 scripts/prueba_empirica.sh procedimientos escalable | tee proc_escalable.txt
```

`procedimientos` corre por separado consulta, reserva, modificación, cancelación y un perfil mixto (BD al estado inicial
antes de cada uno) y, para cada uno, imprime tiempos y TPM, CPU por componente y nodo, ms de CPU por transacción y el
componente que limita. Los CSV quedan en `resultados_procedimientos/`. En el perfil de cancelación el costo por
transacción es por pareja reserva + cancelación (la cancelación es asíncrona: el cliente no mide su tiempo).

## Escalado automático por CPU (scale-out / scale-in) y leaky bucket

El GT decide cuántas VMs de RMC reciben trabajo según la **CPU de cada VM**, que cada RMC informa en su heartbeat
(`/proc/stat`, % de todos los núcleos). Las VMs se declaran en orden de activación con `--escalar base,vm4,vm5` (el nombre es
el `--nombre` del RMC de cada VM). Todos los RMC están arrancados y registrados desde el principio; "activar" una VM es que el
GT empiece a repartirle solicitudes y "liberarla" es dejar de hacerlo (termina lo que tiene y queda en espera, sin consumir).

| Evento | Regla (valores por defecto) |
|---|---|
| **Scale-out** | la CPU de la **VM más cargada de las activas** se mantiene ≥ `--umbral-out` (**50 %**) durante `--sostenido-out` (3 s) → se activa el siguiente nivel. Con 2 o más niveles activos el reparto entre ellos es parejo (el RMC libre que lleva más tiempo ocioso). (`--escala-metrica ultimo` usa solo la CPU del último nivel activo; no sirve cuando el GT comparte VM con el primer nivel, ver la nota de abajo) |
| **Scale-in** | la CPU del último nivel activo queda < `--umbral-in` (20 %) durante `--sostenido-in` (10 s) **y** los niveles anteriores tienen holgura (< umbral-out − 10) → se libera el último. Mientras la VM del GT siga caliente no se libera nada |
| **Anti-oscilación** | umbrales distintos de entrada y salida + tiempo sostenido + `--enfriar` (8 s) mínimos entre dos cambios |
| **Leaky bucket** | cuando ya están activos todos los niveles disponibles y la VM más cargada sigue ≥ umbral-out, la cubeta (cola de espera acotada a `--cubeta N` solicitudes) se vacía hacia los RMC; si llega una solicitud con la cubeta llena se responde de inmediato `SOBRECARGA` (el cliente no espera ni reintenta, y espera `--backoff-ms` antes de pedir de nuevo). Con `--fuga R` la salida además se limita a R solicitudes/s. Se libera cuando la CPU baja de umbral-out − 10 |
| **Tolerancia a fallas** | si el primer nivel se queda sin RMC (cae la VM), se activa automáticamente el primero que tenga; si cae una VM activa, el GT sigue con las demás. Las cancelaciones no pasan por la cubeta |

Todo es opcional: sin `--escalar` el GT se comporta como antes (reparto parejo entre todos los RMC).

```bash
# Demostración desde VM1 (VM2 = BD, VM3 = GT + RMC "base", VM4 y VM5 = RMC "vm4" y "vm5" en espera):
scripts/prueba_empirica.sh verificar
scripts/prueba_empirica.sh escalamiento | tee escalamiento.txt          # rampa de clientes 4->160->4 (~5 min), RMC real
# Las versiones con veredicto automático y carga sintética son scripts/test1.sh y scripts/test2.sh (ver la guía rápida)
# resultados_escalamiento/escalamiento.png  y  escalamiento_resumen.md
UMBRAL_OUT=30 CUBETA=40 FUGA=800 scripts/prueba_empirica.sh escalamiento   # variantes (ver cabecera del script)
```

El script arranca la topología, sube y baja los clientes en pasos (`PASOS="4 12 24 48 96 160 96 48 12 4"`, `PASO_S=25`
s cada uno) y registra, segundo a segundo, los niveles activos, la CPU de cada VM, el reparto, la cola y los rechazos
(`escala_gt.csv`); `analisis/escalamiento.py` lo convierte en un gráfico de 4 paneles y termina con `REPOSO_S` (40 s) sin
carga, donde se ve el scale-in y la CPU base de cada VM.

**Por qué la regla mira la VM más cargada.** El GT vive en la primera VM (VM3) y atiende *todo* el tráfico (red, reparto,
respuestas), así que esa VM se calienta primero; VM4 y VM5 solo ejecutan RMC (unos ms de CPU por solicitud) y en las
mediciones reales no pasan del 10-25 %. Si el scale-out mirara solo la CPU del último nivel (VM4), VM5 y la cubeta no se
activarían nunca. Si el escalado se completa ya con pocos clientes (porque VM3 supera el 50 % desde el primer paso), suba
`UMBRAL_OUT` (por ejemplo 70) para que dependa de la carga; el gráfico muestra la CPU real de cada VM para decidirlo.

Pruebas automáticas (CPU simulada con `--cpu-simulada`, determinista): `scripts/pruebas_escalado.sh` (32 comprobaciones:
scale-out base→vm4→vm5, scale-in en orden inverso, reparto parejo con 3 niveles, la cubeta limita a la fuga configurada,
la cola nunca pasa de la capacidad, lo rechazado no deja rastro en la BD, y el caso real donde el GT calienta la VM base y vm4/vm5 apenas trabajan).

## Demostración de las fallas (sustentación)

Con el generador corriendo (`--rpm 0` para ver el efecto):

| Qué se hace | Qué se observa |
|---|---|
| `kill -9` al RMC de VM4 (el proceso `bin/rmc`) | GT1: `RMC vm4/h0 sin señales: se da por caído … se reasigna`; los clientes no ven errores; el supervisor arranca un proceso nuevo y el GT lo registra otra vez |
| `kill -9` al GT1 (VM3), en la variante `escalable` | El generador imprime `cambios_de_GT=N`; el trabajo sigue por GT2 (VM4); `errores=0` |
| Al terminar | `reservas_confirmadas` del generador = `select count(*) from reserva` (no hay reservas perdidas ni duplicadas) |

## Ver el estado de la BD y de cada proceso

- **BD completa:** `kill -USR1 $(pgrep -f '[b]in/persistencia')` imprime la tabla; también al arrancar y al apagar.
  `--volcar-cada 5` imprime un resumen cada 5 s. Con SQL: `psql -d srb -c "select * from reserva order by 1"`.
- **Restablecer la BD al estado inicial entre corridas:** `kill -USR2 $(pgrep -f '[b]in/persistencia')`.
- **Operaciones de cada proceso:** una línea por operación con su resultado (EXITO/RECHAZO/ERROR) salvo con `--silencioso`.
- **GT:** `kill -USR1 $(pgrep -f '[b]in/gestor')` imprime `GT-STATS` (totales, TPM por tipo, reparto por VM,
  reasignadas, inválidas), lo añade a `--stats-csv` y pone los contadores en cero.

## Reproducir el caso de prueba del profesor

Escriba las operaciones en un archivo (ver `ejemplos/caso_ejemplo.txt` y el formato en `src/cliente_archivo.cpp`) y:

```bash
psql -d srb -c "select sum(asientos_ocupados) from evento"          # BD antes (en VM2)
./bin/cliente mi_caso.txt --gt tcp://$IP_GT:5555 --notif tcp://$IP_GT:5558
psql -d srb -c "select * from reserva order by 1"                    # BD después
```

## Medidas de rendimiento (Tabla 1)

1. **Arquitectura base**: GT + 1 RMC de **1 hilo** (en VM3, `--gt tcp://localhost:5556`) + persistencia con
   `--hilos 1`. Sin VM4/VM5 ni relevo.
2. **Arquitectura escalable**: la de arriba (VM4+VM5 con RMC de 2 hilos, persistencia `--hilos 4`, relevo).
3. Persistencia siempre con más asientos para que no se agoten durante la corrida y no distorsionen los
   tiempos: `./bin/persistencia --reset --silencioso --hilos <1|4> --asientos 5000`. Servicios con `--silencioso`.
4. Desde una máquina con `ssh` sin contraseña a las demás (ver variables al inicio del script):

```bash
ARQ=base      MODO=ssh GEN0=u@IP_VM1 BD_HOST=u@IP_BD GT_HOST=u@IP_GT GT_EPS=tcp://$IP_GT:5555 \
              NOTIF=tcp://$IP_GT:5558 scripts/tabla1.sh
# (cambie la topología a la escalable) y luego:
ARQ=escalable MODO=ssh GEN0=u@IP_VM1 GEN1=u@IP_VM5 BD_HOST=u@IP_BD GT_HOST=u@IP_GT \
              GT_EPS=tcp://$IP_GT:5555,tcp://$IP_RELEVO:6555 NOTIF=tcp://$IP_GT:5558 scripts/tabla1.sh
python3 analisis/analisis.py 'resultados/*muestras*.csv' --salida resultados
```

   Cada corrida: restablece la BD, 15 s de calentamiento descartados + 120 s medidos, 3 repeticiones por
   escenario (3, 5 y 8 clientes). Sale `resultados/tabla1.csv|md` y PNG de tiempo promedio, p95 y TPM.
   Si no tiene `ssh` entre las VMs, ejecute a mano en cada VM los comandos de `scripts/tabla1.sh`.

## Parámetros

- **persistencia:** `--bind tcp://*:5559` `--db "dbname=srb"` `--hilos 4` `--reset` `--mes 202611` `--eventos 50`
  `--dobles 20` `--asientos 100` `--volcar-cada SEG` `--silencioso`
- **gestor:** `--bind tcp://*:5555` `--bind-rmc tcp://*:5556` `--cancel tcp://host:5557` `--hb-ms 500`
  `--liveness 4` `--ttl 15` `--stats-csv archivo` `--nombre GT1` `--silencioso`
  Escalado y cubeta (opcionales): `--escalar base,vm4,vm5` `--umbral-out 50` `--umbral-in 20` `--sostenido-out 3`
  `--sostenido-in 10` `--enfriar 8` `--escala-metrica max|ultimo` `--cubeta N` `--fuga R` `--linea-tiempo archivo.csv`
- **rmc:** `--gt tcp://h:5556[,tcp://h2:6556]` `--persistencia tcp://h:5559` `--hilos 2` `--nombre vm4`
  `--hb-ms 500` `--liveness 4` `--timeout-ms 3000` `--cpu-simulada archivo` `--carga-cpu-ms N` (CPU sintética por solicitud,
  solo para pruebas de estrés) `--silencioso`
- **cancelaciones:** `--bind-pull tcp://*:5557` `--bind-pub tcp://*:5558` `--persistencia tcp://h:5559` `--silencioso`
- **generador:** `--gt tcp://a:5555[,tcp://b:6555]` `--notif tcp://h:5558` `--clientes N` `--solo-consulta K`
  `--rpm N` (0 = sin pausa; el enunciado exige ≥ 20) `--duracion SEG` `--calentamiento SEG` `--timeout-ms 4000`
  `--reintentos 3` `--pct-consulta 40 --pct-reserva 40 --pct-modificar 20 --pct-cancelar 0` (suman 100)
  `--max-asientos 4` `--eventos 50 --dobles 20 --mes 202611` `--semilla` `--vm-indice I --vm-total T`
  `--backoff-ms 50` (espera tras un rechazo por SOBRECARGA)
  `--arquitectura base|escalable --repeticion R` `--csv resumen.csv` `--muestras muestras.csv`
- **cliente:** `archivo.txt` `--gt` `--notif` `--todo` `--esperar-notif MS` `--pausa-ms MS`

## Mensajes y patrones

Texto de una línea, 14 campos separados por `|` (ver `common.hpp`): `idMensaje|tipo|idCliente|endpoint|idEvento|
idOcurrencia|mes|numAsientos|idReserva|fechaNueva|numAsientosNuevo|tsEnvio|resultado|eventos`.

| Canal | Patrón ZeroMQ |
|---|---|
| Cliente → GT (síncrono) | REQ / ROUTER + *Lazy Pirate* (timeout, reintento con el mismo id, cambio de GT) |
| GT ↔ RMC (síncrono) | ROUTER / DEALER, *Paranoid Pirate* (READY, heartbeats, LRU, reasignación) |
| RMC → Persistencia (síncrono) | REQ → ROUTER/DEALER → hilos REP |
| Cancelación (asíncrono) | Cliente → GT (REQ/REP, ACK inmediato) → PUSH/PULL → Cancelaciones → Persistencia (REQ/REP) |
| Notificación | PUB/SUB (tópico `c<idCliente>|`) |

## Limitaciones conocidas

- PostgreSQL y el servicio de persistencia son una sola instancia (el enunciado exige tolerar fallas del GT y del RMC).
- El servicio de Cancelaciones es una sola instancia: si cae toda la VM3 los GT dejan de poder entregar cancelaciones
  hasta que se relance (el supervisor lo reinicia si solo cae el proceso).
- Las cancelaciones aceptadas y aún no efectuadas viven en la cola del GT → PULL; si el proceso GT muere en ese instante
  pueden perderse (el cliente ya recibió el ACK).
- El escalado es lógico: las réplicas de VM4/VM5 ya están arrancadas y el GT las activa o libera; no arranca ni apaga
  máquinas. Cada GT (principal y relevo) lleva su propio estado de escalado. La CPU que se mide es la del nodo completo.
- Si la BD es el límite, activar más VMs de RMC no sube el rendimiento (solo añade concurrencia); la cubeta protege la latencia.
- La batería automática (93 comprobaciones) se corre en una sola máquina; el rendimiento y el escalado reales salen de las corridas en las
  5 VMs (`test1`, `test2`, `correr`, `escalamiento`). `test1` y `test2` usan carga sintética de CPU en el RMC (ver arriba).
- Detalle de la arquitectura, de sus patrones y de sus ventajas y desventajas: `ARQUITECTURA_FINAL.md`.
