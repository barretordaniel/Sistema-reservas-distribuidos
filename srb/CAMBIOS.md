# Cambios del SRB por versión

La versión instalada en cada VM se ve con `cat VERSION`. Cada entrada dice **qué cambió** y **por qué**.

## v3.5 (2026-11-18) – test1 y test2 tres veces más rápidos

- **test1 ~1 min (antes ~2 min) y test2 ~3 min (antes ~7-8 min):** pasos de 10 s en vez de 30 s (`T1_PASO_S`, `T2_PASO_S`), reposo final
  de 5 s / 20 s (`T1_REPOSO_S`, `T2_REPOSO_S`) y escalado más ágil en estas dos pruebas: el último nivel se libera tras 5 s con poca
  CPU (`T_SOSTENIDO_IN`, antes 10 s) y el enfriamiento entre cambios es de 5 s (`T_ENFRIAR_S`, antes 8 s). El scale-out sigue pidiendo
  3 s sostenidos. La corrida `escalamiento` conserva los tiempos de antes (`SOSTENIDO_OUT`, `SOSTENIDO_IN`, `ENFRIAR_S`).
- `config.txt` guarda `sostenido_out_s`, `sostenido_in_s` y `enfriar_s`.
- Para repetir la versión lenta: `T1_PASO_S=30 T2_PASO_S=30 T1_REPOSO_S=15 T2_REPOSO_S=45 T_SOSTENIDO_IN=10 T_ENFRIAR_S=8 scripts/test2.sh`.

## v3.4 (2026-11-18) – pruebas test1/test2, monitores y carga sintética

**Nuevo**
- `scripts/test1.sh` y `scripts/test2.sh` (subcomandos `test1` y `test2` de `prueba_empirica.sh`), con **veredicto automático**
  (`analisis/veredicto.py`, `veredicto.md`) y gráfico de cuatro paneles:
  - **test1** – una VM es suficiente: carga ligera (por defecto 3, 6 y 9 clientes a 600 solicitudes/min cada uno). Comprueba que
    no hubo scale-out, que la CPU de VM3 se mantuvo bajo el umbral, que VM4 y VM5 no recibieron trabajo, que no hubo cubeta ni
    errores y que p95 < 1 s.
  - **test2** – rampa pesada hasta ocupar las 3 VMs (por defecto 2 → 128 clientes sin pausa). Comprueba el orden de
    activación 1 → 2 → 3, que las tres VMs llegan a la saturación, que la capacidad crece al sumar VMs, que entra el leaky
    bucket, que no hay errores y que en el reposo final se liberan en orden inverso.
- `scripts/monitor_vm.sh` (VM3, VM4, VM5): CPU total con barra, CPU por proceso (`gestor`, `rmc`, `cancelaciones`), RAM, procesos
  vivos y *load average*, una línea por segundo; `--csv archivo` para guardar la serie.
- `scripts/monitor_bd.sh` (VM2): **peticiones en curso** en PostgreSQL (promedio y máximo de 20 muestras por segundo, porque
  cada petición dura ~1 ms), bloqueadas por *locks*, conexiones, transacciones por segundo, **RAM usada en %**, CPU y memoria
  de `persistencia` y `postgres`.
- `bin/rmc --carga-cpu-ms N`: cada solicitud consume N ms de CPU (cálculo, no espera). Es una **carga sintética** de prueba que
  simula lógica de negocio costosa. Sin ella la BD se satura (~1100 ops/s) antes que la capa lógica y VM4/VM5 nunca pasan del
  25 % de CPU, así que "ocupar el 100 % de las 3 VMs" no sería alcanzable. Por defecto vale 0 (comportamiento de siempre).
- `ARQUITECTURA_FINAL.md` (composición, patrones nuevos, mejoras, ventajas y desventajas) y este `CAMBIOS.md`.
- La corrida de escalamiento guarda `config.txt` (parámetros usados) y `pasos_resumen.csv` (ops/s y p95 por paso) para el informe.
- **Contador de solicitudes en tiempo real en VM1:** `bin/generador --en-vivo SEG` imprime cada SEG segundos una línea `[EN VIVO]`
  con enviadas, respondidas, ritmo por segundo, en curso, rechazadas por `SOBRECARGA` y fallas (y una línea `FIN` con el ritmo
  medio). `prueba_empirica.sh` (`test1`, `test2`, `escalamiento`, `correr`) lo muestra por defecto en la terminal (por stderr,
  así `| tee` no lo guarda); `EN_VIVO=0` lo apaga. Los contadores son atómicos: no frenan a los clientes.
- Pruebas automáticas nuevas (sección 13): carga sintética, los dos monitores y el contador en vivo. La batería pasa de 84 a 93
  comprobaciones.

**Cambiado**
- README reescrito con las instrucciones de la topología real (VM1 control y carga · VM2 BD · VM3 GT + RMC base · VM4 y VM5 RMC).
- Con carga sintética el RMC de VM3 arranca con `nice -n 10`: el GT (un solo hilo que atiende todo el tráfico) tiene prioridad sobre
  los hilos de cálculo de su propia VM y no se queda sin CPU.

## v3.3 (2026-11-18) – correcciones de la primera corrida real en las 5 VMs

- **El escalado mira la VM más cargada de las activas** (`--escala-metrica max`, por defecto). Con la regla anterior (CPU del
  último nivel) VM5 y el leaky bucket nunca se activaban: el GT vive en VM3 y la calienta (58–84 %), mientras VM4 y VM5, que
  solo ejecutan RMC, no pasaban del 10–25 %. `--escala-metrica ultimo` conserva la regla vieja.
- **Medición de CPU sin `awk`** (`cpu_serie.sh`, `muestreo_cpu.sh`): la CPU de la BD salió de 12 000–36 000 % porque algunos `awk`
  imprimen con 6 cifras los contadores de `/proc/stat` (> 2^31). Ahora es aritmética entera de bash, y lo que no se puede
  calcular sale como `nan`; el análisis descarta lo que no sea un porcentaje válido (0–100) y lo avisa.
- **Último paso del resumen sin diluir:** contaba los segundos de reposo posteriores (410 ops/s en vez de ~1091). Ahora el paso termina
  donde termina.
- La corrida de escalamiento termina con un **reposo sin carga** (`REPOSO_S`, 40 s) para ver el scale-in y la CPU base de cada VM.
- `actualizar_vms.sh` ya no dice "todas actualizadas" cuando una VM no pudo compilar (detecta `FALTA`).

## v3.2 (2026-11-18) – escalado por CPU y leaky bucket

- **Scale-out / scale-in por CPU** en el GT (`--escalar base,vm4,vm5`): cada RMC informa la CPU de su nodo en el heartbeat; el GT
  activa el siguiente nivel con ≥ 50 % sostenido 3 s y lo libera con < 20 % sostenido 10 s; mínimo 8 s entre cambios.
- **Leaky bucket** (`--cubeta N --fuga R`): con todas las VMs al límite la cola queda acotada y el exceso recibe `SOBRECARGA`
  de inmediato; el generador lo cuenta aparte (no es error) y espera `--backoff-ms`.
- Modo `escalamiento` de `prueba_empirica.sh` (rampa de clientes) y `analisis/escalamiento.py`; `pruebas_escalado.sh`
  (CPU simulada con `--cpu-simulada`, determinista).

## v3.1 – cuello de botella por procedimiento

- Modo `procedimientos` (consulta, reserva, modificación, cancelación, mixto) con CPU por componente y ms de CPU por transacción.
- Etiquetas de VM en los informes (`ET_GT`, `ET_BD`, `ET_VM4`, `ET_VM5`, `ET_GEN`) y usuario/ruta propios por VM.

## v3.0 – segunda entrega: escalable y tolerante a fallas

- GT ↔ RMC con ROUTER/DEALER (registro `READY`, heartbeats, reparto LRU, reasignación); RMC multihilo.
- Persistencia multihilo (ROUTER → DEALER → N hilos REP con una conexión PostgreSQL por hilo).
- Tolerancia a fallas: GT de relevo, cliente *Lazy Pirate* con reintento del mismo `idMensaje`, idempotencia en la BD (`transaccion`).
- Operación nueva: modificación de reservas. Generador de carga con métricas (promedio, desviación, p95, TPM) y reparto entre VMs.
- Herramientas: `bin/cliente`, batería automática (52 comprobaciones en esa versión), `quien_soy.sh`, `actualizar_vms.sh`,
  `prueba_empirica.sh`, supervisor y análisis de la Tabla 1.
