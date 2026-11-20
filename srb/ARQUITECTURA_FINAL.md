# SRB – Arquitectura final (segunda entrega)

Sistema de Reserva de Boletos · Pontificia Universidad Javeriana · Introducción a Sistemas Distribuidos · 2026-30.
C++17 + ZeroMQ + PostgreSQL. Versión de este documento: ver `VERSION`.

Este archivo explica **cómo está compuesto el proyecto, qué patrones se añadieron, qué mejoró respecto a la primera
entrega y cuáles son las ventajas y desventajas del diseño**. Las instrucciones para ejecutarlo están en `README.md`; el
protocolo de pruebas, en `DISENO_Y_PRUEBAS.txt`; la lista de cambios por versión, en `CAMBIOS.md`.

---

## 1. Resumen en un minuto

- Tres capas, como pide el enunciado: **consumidores** (generador de carga / cliente), **lógica de negocio** (GT, RMC,
  Cancelaciones) y **almacenamiento** (Persistencia + PostgreSQL). Toda la comunicación es ZeroMQ.
- La primera entrega era una cadena `cliente → GT → RMC → Persistencia` sin réplicas ni fallas. Ahora la capa lógica
  tiene **réplicas de RMC en varias VMs**, el GT las reparte por turnos (LRU), detecta las que caen y **reasigna** su
  trabajo; el cliente **reintenta con el mismo id** y la BD es **idempotente**, así que ningún reintento duplica una reserva.
- El GT **escala solo según la CPU de cada VM** (scale-out / scale-in) y, cuando ya no hay más VMs que activar, un
  **leaky bucket** acota la cola y rechaza el exceso de inmediato con `SOBRECARGA`.
- Toda la topología se **opera desde una sola máquina** (VM1): arranca, mide y detiene los procesos de las otras cuatro por
  ssh según `scripts/vms.conf`. Los procesos corren distribuidos, cada uno en su VM.

## 2. Cómo está compuesto

### 2.1 Topología de las 5 VMs (laboratorio NBIG)

```
 VM1 (NBIG250)  generador + control por ssh
   │  REQ (Lazy Pirate)                SUB (notificaciones)
   ▼                                          ▲
 VM3 (NBIG263)  ┌──────────────────────────────┴───────────────────────────┐
  GT :5555/5556 │ GT  ──PUSH──▶ Cancelaciones :5557 (PULL) / :5558 (PUB)   │
  RMC "base"    │  ▲ ROUTER :5556                                          │
                └──┼───────────────────────────────────────────────────────┘
                   │ DEALER (READY / heartbeat+CPU / REP)
        ┌──────────┴───────────┐
   VM4 (NBIG247) RMC "vm4"   VM5 (NBIG254) RMC "vm5"        (los RMC se conectan ellos al GT)
        └──────────┬───────────┘
                   │ REQ                       (también desde el RMC base y desde Cancelaciones)
                   ▼
 VM2 (NBIG252)  Persistencia :5559  (ROUTER ─▶ DEALER ─▶ N hilos REP)  ─▶  PostgreSQL "srb"
```

| VM | Capa | Procesos | Puertos |
|---|---|---|---|
| VM1 | Consumidores | `bin/generador` (o `bin/cliente`), scripts de control | — |
| VM2 | Almacenamiento | `bin/persistencia` + PostgreSQL | 5559 |
| VM3 | Lógica de negocio | `bin/gestor` (GT), `bin/cancelaciones`, `bin/rmc` (nivel `base`) | 5555, 5556, 5557, 5558 |
| VM4 | Lógica de negocio | `bin/rmc` (nivel `vm4`); GT de relevo en la variante `escalable` | 6555, 6556 (solo relevo) |
| VM5 | Lógica de negocio | `bin/rmc` (nivel `vm5`) | — |

### 2.2 Componentes del código (`src/`)

| Archivo | Responsabilidad |
|---|---|
| `common.hpp` | Mensaje de 14 campos, serialización, `EnlaceGT` (Lazy Pirate), utilidades ZeroMQ, `MedidorCPU` (/proc/stat) |
| `gestor_transacciones.cpp` | **GT**: ROUTER de clientes y de RMC, reparto LRU, heartbeats, reasignación, PUSH a Cancelaciones, **escalado por CPU y leaky bucket** |
| `servicio_rmc.cpp` | **RMC** (Reservas, Modificaciones, Consultas): una réplica por hilo, DEALER hacia el GT, REQ hacia Persistencia; informa su CPU en cada heartbeat; `--carga-cpu-ms` para pruebas de estrés |
| `servicio_persistencia.cpp` | **Persistencia**: ROUTER/DEALER interno + N hilos REP, una conexión PostgreSQL por hilo, transacciones idempotentes |
| `servicio_cancelaciones.cpp` | Cancelaciones asíncronas: PULL desde el GT, REQ a Persistencia, PUB de notificaciones |
| `generador_carga.cpp` | Generador de clientes (promedio, desviación, p95, TPM, muestras crudas, rechazos por sobrecarga) |
| `cliente_archivo.cpp` | Cliente que ejecuta un archivo de operaciones (el "caso del profesor") |

### 2.3 Herramientas de operación y medida (`scripts/`, `analisis/`)

| Para qué | Archivo |
|---|---|
| Operar las 5 VMs desde VM1 (arrancar, medir, parar) | `prueba_empirica.sh`, `vms.conf` |
| **Prueba "una VM alcanza"** y **prueba "ocupar las 3 VMs"**, con veredicto automático | `test1.sh`, `test2.sh`, `analisis/veredicto.py` |
| Ver en vivo la CPU/RAM de VM3, VM4, VM5 | `monitor_vm.sh` |
| Ver en vivo las peticiones en curso y la RAM de la BD (VM2) | `monitor_bd.sh` |
| Ver en vivo, en VM1, cuántas solicitudes envían los clientes (enviadas, respondidas, por segundo, en curso, rechazadas) | `bin/generador --en-vivo 1` (lo activa `test1`/`test2` por defecto, `EN_VIVO`) |
| Pruebas automáticas en una máquina (93 comprobaciones) | `pruebas_automaticas.sh`, `pruebas_escalado.sh` |
| Mantener las VMs al día | `actualizar_vms.sh`, `aplicar_cambios.py` |
| Tabla 1 y gráficos | `tabla1.sh`, `analisis/analisis.py`, `analisis/escalamiento.py` |

### 2.4 Qué hace una solicitud

**Reserva (síncrona).** Cliente → GT (REQ/ROUTER) → un RMC libre (ROUTER/DEALER, el que lleva más tiempo ocioso) →
Persistencia: (1) consulta de disponibilidad, (2) transacción de reserva que vuelve a verificar dentro de la BD → respuesta
por el mismo camino. Si el RMC o el GT caen a mitad, la solicitud se reasigna o el cliente la reintenta **con el mismo
`idMensaje`** y la BD devuelve el resultado ya guardado.

**Cancelación (asíncrona).** Cliente → GT (ACK inmediato) → PUSH/PULL → Cancelaciones → Persistencia; al efectuarse en la BD,
Cancelaciones publica la notificación (PUB/SUB, tópico `c<idCliente>|`) y el cliente la recibe.

**Escalado.** Cada RMC informa la CPU de su nodo en el heartbeat. El GT lleva una lista de niveles (`base → vm4 → vm5`): solo
reparte entre los activos. Cuando la VM más cargada de las activas sostiene ≥ 50 % de CPU se activa el siguiente nivel; si
todos están activos y sigue ≥ 50 %, entra el leaky bucket. Al bajar la carga se libera todo en orden inverso.

## 3. Patrones

### 3.1 Patrones que ya estaban o se afianzaron respecto a la primera entrega

| Patrón | Dónde | Para qué |
|---|---|---|
| **Lazy Pirate** (cliente con timeout y reintento) | Cliente → GT | Recuperarse de pérdidas de mensajes y de la caída del GT; rota al GT de relevo |
| **Paranoid Pirate** (READY, heartbeats, LRU) | GT ↔ RMC | Registro dinámico de trabajadores, detección de caídas, reparto equitativo |
| **Broker / ROUTER-DEALER** | GT (hacia RMC), Persistencia | Balancear solicitudes entre trabajadores sin que el cliente conozca a ninguno |
| **Multithreaded server** (pool de hilos) | Persistencia, RMC | Concurrencia: cada hilo es un trabajador con su propia conexión |
| **Pipeline PUSH/PULL** | GT → Cancelaciones | Aceptar la cancelación de inmediato y procesarla después |
| **Publish/Subscribe** con tópico por cliente | Cancelaciones → clientes | Notificar solo a quien corresponde |
| **Receptor idempotente** (`transaccion` con clave `idCliente+idMensaje`) | Persistencia | Un reintento o una reasignación nunca duplican una reserva |
| **Servicios sin estado** | RMC | Cualquier réplica atiende cualquier solicitud: el estado vive solo en la BD |

### 3.2 Patrones nuevos de esta entrega

| Patrón | Dónde | Qué aporta |
|---|---|---|
| **Autoescalado por umbrales con histéresis** (scale-out / scale-in) | GT | La capa lógica crece y decrece según la CPU real de cada VM; umbrales distintos de entrada y salida, tiempo sostenido y enfriamiento evitan oscilaciones |
| **Leaky bucket / load shedding (contrapresión)** | GT | Con todas las VMs al límite, la cola queda acotada y el exceso recibe `SOBRECARGA` al instante: la latencia de lo aceptado no se dispara; el cliente espera `--backoff-ms` y no lo cuenta como error |
| **Heartbeat con carga incorporada** (piggyback de CPU) | RMC → GT | La métrica viaja en el mensaje que ya existía: no hay un canal ni un monitor aparte |
| **Descubrimiento de servicios por registro** | RMC → GT (`READY`) | Un RMC nuevo, incluso en plena carga, se incorpora solo; los niveles inactivos están registrados y en espera |
| **Failover del escalado** | GT | Si el primer nivel se queda sin RMC se activa el primero que tenga; si cae una VM activa, el GT sigue con las demás |
| **Orquestación centralizada** | VM1 | Un solo punto de control (ssh + `vms.conf`) arranca, mide y detiene los procesos distribuidos |
| **Carga sintética de CPU** (`--carga-cpu-ms`) | RMC | Simula una lógica de negocio costosa para poder saturar la capa lógica antes que la BD y observar el escalado |
| **Supervisor de procesos** | `supervisor.sh` | Relanza un servicio que muere; el GT lo vuelve a registrar |

No se implementó **circuit breaker**: es una mejora posible, no un requisito del enunciado.

## 4. Mejoras respecto a la entrega anterior

| Aspecto | Primera entrega | Ahora |
|---|---|---|
| Estructura | Cadena `cliente → GT → RMC → Persistencia` (REQ/REP) | GT como broker; **N RMC en varias VMs**; Persistencia con N hilos |
| Operaciones | Consulta, reserva, cancelación | + **Modificación** (asientos o fecha a otra ocurrencia) |
| Fallas | No toleradas | Detección por heartbeat, **reasignación**, GT de relevo, reintentos con el mismo id, idempotencia en la BD |
| Escalabilidad | Un hilo en cada capa | RMC y Persistencia multihilo; reparto LRU; generador repartible entre VMs |
| Elasticidad | — | **Scale-out / scale-in automático por CPU** entre VM3 → VM4 → VM5 |
| Protección de sobrecarga | — | **Leaky bucket** con rechazo inmediato `SOBRECARGA` |
| Pruebas | Funcionales básicas | **84 comprobaciones automáticas** (unitarias, funcionales, concurrencia, fallas, escalado), `test1` y `test2` con veredicto automático |
| Medición | Tiempos del cliente | CPU por componente y por nodo, ms de CPU por transacción, p95, TPM, monitores en vivo de CPU/RAM y de peticiones en curso |
| Operación | Arranque manual en cada máquina | Arranque, medida y parada **desde VM1** por ssh; instalador de un solo archivo; `actualizar_vms.sh` |

## 5. Ventajas

- **Sin sobreventa ni pérdidas:** el descuento de asientos es atómico en la BD (`UPDATE … WHERE libres >= n` más restricciones
  `CHECK`) y las reservas confirmadas siempre coinciden con las filas de la tabla (se comprueba en cada prueba).
- **Fallas transparentes:** la caída de un RMC (proceso o VM) o del GT principal no produce errores visibles para el cliente.
- **Crecimiento incremental:** agregar capacidad es arrancar un RMC más; no se cambia código ni configuración del GT.
- **Escalado explicable:** cada decisión queda en el log con la CPU que la causó (`ESCALA-OUT`, `ESCALA-IN`, `CUBETA ACTIVA`) y en
  una línea de tiempo por segundo (`escala_gt.csv`) que se convierte en un gráfico de cuatro paneles.
- **Latencia acotada bajo sobrecarga:** el leaky bucket convierte "todo se pone lento" en "se rechaza lo que sobra".
- **Verificable:** una batería automática en una sola máquina y dos pruebas empíricas (`test1`, `test2`) que dan su veredicto.
- **Operable por una persona:** un solo punto de control y monitores en vivo por VM.
- **Sin dependencias pesadas:** solo ZeroMQ y PostgreSQL; el RMC no necesita librerías de BD.

## 6. Desventajas y límites (para decirlos en la sustentación)

- **La BD es el techo.** PostgreSQL y Persistencia son una sola instancia. En las mediciones reales el sistema entero se
  acerca a ~1100 operaciones/s y la BD llega al 70–90 % de CPU: agregar RMC añade **concurrencia**, no capacidad de BD.
  Por eso `test2` usa carga sintética en el RMC: sin ella la capa lógica nunca se satura antes que la BD.
- **Escalado lógico, no físico.** Las réplicas de VM4 y VM5 ya están arrancadas; "activar" es que el GT les envíe trabajo.
  No se encienden ni apagan máquinas.
- **La métrica de CPU es la del nodo completo** (incluye otros procesos de la VM). La VM del GT se calienta primero porque
  atiende todo el tráfico, por eso la regla mira la VM **más cargada** de las activas y no solo la última.
- **Puntos únicos de falla remanentes:** Persistencia/PostgreSQL y Cancelaciones. Una cancelación aceptada que aún no pasó del GT a
  Cancelaciones se pierde si el proceso GT muere justo en ese instante (el cliente ya recibió el ACK).
- **Cada GT lleva su propio estado de escalado** (principal y relevo no lo comparten).
- **Rechazar no es gratis para el usuario:** con la cubeta activa algunos clientes reciben `SOBRECARGA`; es deliberado, pero hay
  que explicarlo como decisión de diseño (priorizar latencia sobre admisión).
- **Los números de la batería automática no son rendimiento:** se obtuvieron en una sola máquina. El rendimiento sale de las
  corridas en las VMs (`correr`, `test1`, `test2`).
- **Complejidad operativa:** más procesos, puertos y parámetros que la primera entrega; se compensa con la operación desde VM1.

## 7. Qué se midió en las VMs reales

Corrida de calibración del escalado (v3.2, 3 pasos de 15 s, sin carga sintética):

| Clientes | ops/s | p95 consulta / reserva / modificación (ms) |
|---|---|---|
| 4 | 554 | 9.5 / 14.5 / 13.7 |
| 24 | 882 | 35.8 / 44.9 / 43.9 |
| 96 | 1091 | 113.6 / 120.4 / 120.1 |

VM3 (GT + RMC) llegó al 58–84 % de CPU y activó VM4; VM4 y VM5 no pasaron del 10–25 %. Esa observación llevó a la regla
actual (VM más cargada) y a la carga sintética de `test2`. Los resultados de `test1` y `test2` se agregan aquí al terminar
(`resultados_test1/veredicto.md`, `resultados_test2/veredicto.md` y los gráficos `escalamiento.png`).

## 8. Cómo se demuestra en la sustentación

1. `scripts/prueba_empirica.sh mapa` y `verificar`: quién es quién y que todo compila.
2. En cada VM, una terminal con `scripts/monitor_vm.sh` (VM3, VM4, VM5) o `scripts/monitor_bd.sh` (VM2). En VM1 las
   solicitudes de los clientes aparecen en vivo, una línea por segundo, al lanzar cada prueba.
3. `scripts/test1.sh`: carga ligera; VM3 sola la atiende, VM4 y VM5 quedan en 0 %.
4. `scripts/test2.sh`: la carga sube; se ven activarse VM4 y VM5, las tres llegan al límite, entra el leaky bucket y, en el
   reposo final, se liberan en orden inverso.
5. Fallas: `kill -9` al RMC de VM4 y al GT (ver `README.md`, "Demostración de las fallas").
