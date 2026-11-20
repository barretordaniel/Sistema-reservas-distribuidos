// gestor_transacciones.cpp  (capa de lógica de negocio)
// Puerta de entrada del sistema y broker de las réplicas del RMC. Un solo hilo con zmq_poll:
//   - ROUTER de clientes: recibe de los REQ de los clientes y les devuelve la respuesta.
//   - ROUTER de RMC: los RMC (sockets DEALER, en una o varias VMs) se registran con READY y
//     envían heartbeats (patrón Paranoid Pirate). El GT reparte las solicitudes entre los
//     RMC libres por turnos (el que lleva más tiempo sin trabajar) y, si un RMC deja de dar
//     señales, reasigna su solicitud en curso a otro RMC: el cliente no se entera.
//   - PUSH hacia Cancelaciones: las cancelaciones se aceptan de inmediato (acuse de recibo).
// El GT de relevo es este mismo programa en otra máquina: los RMC se registran en ambos y
// los clientes pasan al de relevo cuando el principal deja de responder.
//
// Escalado por CPU (scale-out / scale-in) y leaky bucket (opcionales; sin --escalar todo se comporta como antes):
//   --escalar base,vm4,vm5   VMs de RMC en orden de activación (el nombre es el --nombre del RMC). Solo se reparte
//                            entre los RMC de los niveles activos; empieza con el primero. Cada RMC informa la CPU
//                            de su nodo en el heartbeat. Cuando la CPU de la VM MÁS CARGADA de las activas se mantiene
//                            >= --umbral-out (50 %) durante --sostenido-out s, se activa el siguiente (scale-out); cuando
//                            la CPU del último nivel queda < --umbral-in (20 %) durante --sostenido-in s y los niveles
//                            anteriores tienen holgura (< umbral-out - 10), se libera el último (scale-in; sus RMC terminan
//                            lo que tienen y quedan en espera).
//   --escala-metrica max|ultimo   qué CPU decide el scale-out y la cubeta: "max" (por defecto) = la VM más cargada de
//                            las activas (necesario cuando el GT comparte VM con el primer nivel: esa VM es la que se
//                            calienta primero y las demás solo hacen de RMC); "ultimo" = solo la del último nivel activo.
//   --cubeta N --fuga R      leaky bucket: cuando ya no hay más niveles que activar y el último está al límite, la
//                            cubeta (cola de espera de N solicitudes) se vacía a R solicitudes/s hacia los RMC; lo
//                            que desborda se rechaza de inmediato con SOBRECARGA (el cliente no espera ni reintenta).
//   --linea-tiempo archivo   CSV con una fila por segundo: niveles activos, CPU de cada nivel, reparto, cola, rechazos.
//
// Uso: ./gestor [--bind tcp://*:5555] [--bind-rmc tcp://*:5556] [--cancel tcp://localhost:5557]
//               [--hb-ms 500] [--liveness 4] [--ttl 15] [--stats-csv archivo]
//               [--escalar base,vm4,vm5 --umbral-out 50 --umbral-in 20 --sostenido-out 3 --sostenido-in 10
//                --enfriar 8 --escala-metrica max] [--cubeta 200 --fuga 800] [--linea-tiempo archivo]
//               [--nombre GT] [--silencioso]
//   kill -USR1 <pid>   imprime las estadísticas, las agrega al CSV y reinicia los contadores
#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <unordered_map>

#include "common.hpp"

using namespace srb;

class GestorTransacciones {
public:
    struct Config {
        std::string bindClientes = "tcp://*:5555";
        std::string bindRMC = "tcp://*:5556";
        std::string epCancel = "tcp://localhost:5557";
        int hbMs = 500;        // intervalo de heartbeat
        int liveness = 4;      // heartbeats consecutivos sin señal para dar por caído a un RMC
        double ttl = 15.0;     // segundos que una solicitud puede esperar un RMC libre
        std::string statsCsv;
        std::string nombre = "GT";
        // --- escalado por CPU y leaky bucket
        std::vector<std::string> niveles;  // VMs de RMC en orden de activación (vacío = sin escalado)
        double umbralOut = 50.0;           // % de CPU (de la VM más cargada de las activas) que dispara el scale-out
        std::string metrica = "max";       // "max" = VM más cargada de las activas; "ultimo" = solo el último nivel activo
        double umbralIn = 20.0;            // % de CPU bajo el cual se libera el último nivel (scale-in)
        double sostenidoOut = 3.0;         // s que la CPU debe mantenerse alta antes de escalar
        double sostenidoIn = 10.0;         // s que debe mantenerse baja antes de liberar
        double enfriar = 8.0;              // s mínimos entre dos cambios de nivel
        int cubeta = 0;                    // capacidad de la cubeta (solicitudes en espera); 0 = sin límite
        double fuga = 0.0;                 // solicitudes/s que salen de la cubeta cuando está activa; 0 = sin tope
        std::string lineaTiempo;           // CSV con una fila por segundo
    };

    explicit GestorTransacciones(const Config& c)
        : cfg(c),
          ctx_(1),
          sockClientes(crearSocket(ctx_, zmq::socket_type::router)),
          sockRMC(crearSocket(ctx_, zmq::socket_type::router)),
          sockCancel(crearSocket(ctx_, zmq::socket_type::push)) {
        sockRMC.set(zmq::sockopt::router_handover, 1);  // un RMC que reconecta reemplaza su sesión
        sockClientes.bind(cfg.bindClientes);
        sockRMC.bind(cfg.bindRMC);
        sockCancel.connect(cfg.epCancel);
    }

    void iniciar() {
        logLinea(cfg.nombre, "escuchando clientes en " + cfg.bindClientes + " y RMC en " + cfg.bindRMC);
        zmq_pollitem_t items[2] = {{static_cast<void*>(sockClientes), 0, ZMQ_POLLIN, 0},
                                   {static_cast<void*>(sockRMC), 0, ZMQ_POLLIN, 0}};
        double proximoHB = ahoraSeg() + cfg.hbMs / 1000.0;
        double ultimoResumen = ahoraSeg();
        double proximoSegundo = ahoraSeg() + 1.0;
        tInicio = ahoraSeg();
        if (!cfg.niveles.empty()) {
            std::string s;
            for (const std::string& n : cfg.niveles) s += (s.empty() ? "" : " -> ") + n;
            logLinea(cfg.nombre, "escalado por CPU: niveles " + s + " (out>=" + fmt0(cfg.umbralOut) + "%, in<" +
                                     fmt0(cfg.umbralIn) + "%, métrica " + cfg.metrica + ")" +
                                     (cfg.cubeta > 0 ? "; leaky bucket cubeta=" + std::to_string(cfg.cubeta) +
                                                           " fuga=" + fmt0(cfg.fuga) + "/s"
                                                     : ""));
        }
        long vistoAntes = 0;
        while (!g_parar) {
            if (g_volcar) {
                g_volcar = 0;
                volcarEstadisticas(true);
            }
            int espera = cfg.hbMs / 5 > 0 ? cfg.hbMs / 5 : 1;
            if (cubetaActiva && cfg.fuga > 0 && !cola.empty()) espera = 1;  // el goteo necesita reloj fino
            int rc = zmq_poll(items, 2, espera);
            if (rc > 0) {
                if (items[0].revents & ZMQ_POLLIN) atenderCliente();
                if (items[1].revents & ZMQ_POLLIN) atenderRMC();
            }
            double ahora = ahoraSeg();
            if (ahora >= proximoHB) {
                proximoHB = ahora + cfg.hbMs / 1000.0;
                latir(ahora);
            }
            if (ahora >= proximoSegundo) {
                proximoSegundo = ahora + 1.0;
                cadaSegundo(ahora);
            }
            despachar(ahora);
            if (ahora - ultimoResumen >= 10.0) {
                ultimoResumen = ahora;
                long total = totalContadas();
                if (total != vistoAntes) {
                    vistoAntes = total;
                    volcarEstadisticas(false);
                }
            }
        }
        logLinea(cfg.nombre, "deteniendo");
        volcarEstadisticas(true);
    }

private:
    struct Pendiente {
        std::string identidad;  // cliente que espera la respuesta
        TipoOperacion tipo;
        double t0;              // llegada al GT
        std::string payload;    // solicitud serializada (se conserva para poder reasignarla)
        std::string trabajador; // RMC que la tiene ahora
    };
    struct Trabajador {
        double expira = 0;
        std::string vm;
        bool libre = false;
        unsigned long corr = 0;  // solicitud en curso (0 = ninguna)
        long despachadas = 0;
        int nivel = -1;          // posición de su VM en --escalar (-1 = no administrada: siempre activa)
        double cpu = 0;          // CPU del nodo informada en el último heartbeat (%)
        double cpuT = 0;         // cuándo la informó
    };

    // ---------------------------------------------------------------- clientes --
    void atenderCliente() {
        std::vector<std::string> f;
        if (!recibirMultipart(sockClientes, f) || f.size() < 2) return;
        std::string identidad = f.front();
        std::string payload = f.back();  // el REQ agrega un delimitador vacío en medio

        Mensaje m;
        std::string motivo;
        if (payload.size() > kMaxBytes || !deserializar(payload, m)) {
            Mensaje err;
            err.resultado = Estado::ERROR;
            responderCliente(identidad, err);
            ++invalidas;
            logOp(cfg.nombre, "mensaje mal formado o demasiado grande -> ERROR");
            return;
        }
        if (!validar(m, motivo)) {
            Mensaje err = m;
            err.resultado = Estado::ERROR;
            responderCliente(identidad, err);
            ++invalidas;
            logOp(cfg.nombre, describir(m) + " inválido (" + motivo + ") -> ERROR");
            return;
        }
        if (m.tipo == TipoOperacion::CANCELACION) {  // canal asíncrono
            Mensaje ack = m;
            ack.resultado = Estado::EXITO;  // acuse de recibo inmediato
            responderCliente(identidad, ack);
            if (!enviarMultipart(sockCancel, {serializar(m)}, true))
                logLinea(cfg.nombre, "no se pudo enviar la cancelación al servicio");
            logOp(cfg.nombre, describir(m) + " -> ACK inmediato, enviado a Cancelaciones");
            return;
        }
        // leaky bucket: con la cubeta activa y llena, la solicitud se rechaza ya (no espera ni se reintenta)
        if (cubetaActiva && cfg.cubeta > 0 && static_cast<int>(cola.size()) >= cfg.cubeta) {
            Mensaje r = m;
            r.resultado = Estado::SOBRECARGA;
            responderCliente(identidad, r);
            ++sobrecargas;
            ++sobrecargasSeg;
            return;
        }
        // canal síncrono: se encola hasta que haya un RMC libre
        unsigned long corr = ++secuencia;
        double ahora = ahoraSeg();
        pendientes[corr] = {identidad, m.tipo, ahora, payload, ""};
        cola.push_back(corr);
        if (tPrimera == 0.0) tPrimera = ahora;
    }

    void responderCliente(const std::string& identidad, const Mensaje& m) {
        enviarMultipart(sockClientes, {identidad, "", serializar(m)}, true);
    }

    // ------------------------------------------------------------------- RMC --
    void atenderRMC() {
        std::vector<std::string> f;
        if (!recibirMultipart(sockRMC, f) || f.size() < 2) return;
        const std::string& id = f[0];
        const std::string& cmd = f[1];
        double ahora = ahoraSeg();

        auto it = trabajadores.find(id);
        if (it == trabajadores.end()) {  // RMC nuevo (o uno que había sido dado por caído)
            Trabajador t;
            t.vm = id.substr(0, id.find('/'));
            t.nivel = nivelDe(t.vm);
            it = trabajadores.emplace(id, t).first;
            marcarLibre(id, it->second);
            logLinea(cfg.nombre, "RMC registrado: " + id + " (" + std::to_string(trabajadores.size()) +
                                     " en línea)");
        }
        Trabajador& t = it->second;
        t.expira = ahora + cfg.hbMs * cfg.liveness / 1000.0;
        if ((cmd == CMD_HB || cmd == CMD_READY) && f.size() >= 3) {  // CPU del nodo del RMC
            try {
                t.cpu = std::stod(f[2]);
                t.cpuT = ahora;
            } catch (...) {
            }
        }

        if (cmd == CMD_READY) {
            if (t.corr != 0) {  // el RMC se reinició y perdió lo que tenía en curso
                reasignar(t.corr);
                t.corr = 0;
            }
            marcarLibre(id, t);
        } else if (cmd == CMD_REP && f.size() >= 4) {
            unsigned long corr = 0;
            try {
                corr = std::stoul(f[2]);
            } catch (...) {
                return;
            }
            auto p = pendientes.find(corr);
            if (p != pendientes.end()) {  // la primera respuesta gana; las tardías se descartan
                Mensaje r;
                if (!deserializar(f[3], r)) r.resultado = Estado::ERROR;
                responderCliente(p->second.identidad, r);
                ++respondidasSeg;
                contar(p->second.tipo, r.resultado);
                char ms[32];
                std::snprintf(ms, sizeof ms, "%.1f ms", (ahora - p->second.t0) * 1000.0);
                logOp(cfg.nombre, describir(r) + " -> " + nombre(r.resultado) + " (" + ms +
                                      ") por " + id);
                pendientes.erase(p);
            }
            if (t.corr == corr) {
                t.corr = 0;
                marcarLibre(id, t);
            }
        }
        // "HB": solo renueva expira
    }

    void marcarLibre(const std::string& id, Trabajador& t) {
        if (!t.libre) {
            t.libre = true;
            libres.push_back(id);
        }
    }

    // Reparte las solicitudes en cola entre los RMC libres de los niveles activos (el que lleva más tiempo sin
    // trabajar primero). Con la cubeta activa y --fuga, el reparto sale a ritmo constante (leaky bucket).
    void despachar(double ahora) {
        const bool gotea = cubetaActiva && cfg.fuga > 0;
        fichas = gotea ? std::min(2.0, fichas + (ahora - tFichas) * cfg.fuga) : 1.0;
        tFichas = ahora;
        while (!cola.empty()) {
            if (gotea && fichas < 1.0) break;
            std::string wid;
            bool hallado = false;
            for (auto it = libres.begin(); it != libres.end();) {
                auto w = trabajadores.find(*it);
                if (w == trabajadores.end() || !w->second.libre) {  // ya no está libre o se dio por caído
                    it = libres.erase(it);
                    continue;
                }
                if (elegible(w->second)) {
                    wid = *it;
                    libres.erase(it);
                    hallado = true;
                    break;
                }
                ++it;  // su nivel no está activo: queda en espera
            }
            if (!hallado) break;
            unsigned long corr = cola.front();
            cola.pop_front();
            auto p = pendientes.find(corr);
            if (p == pendientes.end()) {  // ya respondida
                libres.push_front(wid);
                continue;
            }
            if (ahora - p->second.t0 > cfg.ttl) {  // el cliente ya se rindió
                pendientes.erase(p);
                ++expiradas;
                libres.push_front(wid);
                continue;
            }
            Trabajador& t = trabajadores[wid];
            if (!enviarMultipart(sockRMC, {wid, CMD_REQ, std::to_string(corr), p->second.payload}, true)) {
                cola.push_front(corr);  // no se pudo entregar; se intenta de nuevo
                libres.push_front(wid);
                break;
            }
            t.libre = false;
            t.corr = corr;
            ++t.despachadas;
            ++repartoVM[t.vm];
            ++despSeg[t.nivel >= 0 ? static_cast<size_t>(t.nivel) : cfg.niveles.size()];
            if (gotea) fichas -= 1.0;
            p->second.trabajador = wid;
        }
    }

    // Heartbeats a los RMC y detección de los que dejaron de responder.
    void latir(double ahora) {
        std::vector<std::string> muertos;
        for (auto& kv : trabajadores) {
            enviarMultipart(sockRMC, {kv.first, CMD_HB}, true);
            if (ahora > kv.second.expira) muertos.push_back(kv.first);
        }
        for (const std::string& id : muertos) {
            Trabajador& t = trabajadores[id];
            std::string detalle;
            if (t.corr != 0 && pendientes.count(t.corr)) {
                reasignar(t.corr);
                detalle = "; su solicitud en curso se reasigna a otro RMC";
            }
            logLinea(cfg.nombre, "RMC " + id + " sin señales: se da por caído" + detalle);
            trabajadores.erase(id);
        }
        // Solicitudes que llevan demasiado tiempo sin respuesta (el cliente ya se rindió).
        for (auto it = pendientes.begin(); it != pendientes.end();) {
            if (ahora - it->second.t0 > 60.0) it = pendientes.erase(it);
            else ++it;
        }
    }

    // ------------------------------------------------ escalado por CPU y cubeta --
    static std::string fmt0(double v) {
        char b[24];
        std::snprintf(b, sizeof b, "%.0f", v);
        return b;
    }

    int nivelDe(const std::string& vm) const {
        for (size_t i = 0; i < cfg.niveles.size(); ++i)
            if (cfg.niveles[i] == vm) return static_cast<int>(i);
        return -1;
    }

    // ¿Este RMC recibe trabajo ahora? Sí si su VM no está administrada o su nivel está activo.
    bool elegible(const Trabajador& t) const { return t.nivel < 0 || t.nivel < activos; }

    // CPU del nivel n (la mayor informada por sus RMC en los últimos 4 s). hay = tiene RMC registrados.
    double cpuNivel(int n, bool& hay, double ahora) const {
        double c = 0;
        hay = false;
        for (const auto& kv : trabajadores) {
            if (kv.second.nivel != n) continue;
            hay = true;
            if (ahora - kv.second.cpuT <= 4.0 && kv.second.cpu > c) c = kv.second.cpu;
        }
        return c;
    }

    // Se evalúa una vez por segundo. Ver la cabecera para las reglas.
    void evaluarEscala(double ahora) {
        const int K = static_cast<int>(cfg.niveles.size());
        if (K == 0) return;
        int disp = 0;  // niveles consecutivos con RMC registrados
        bool hay = false;
        while (disp < K) {
            cpuNivel(disp, hay, ahora);
            if (!hay) break;
            ++disp;
        }
        // Tolerancia a fallas: si el primer nivel se quedó sin RMC, se activa el primero que sí tenga.
        int primero = 0;
        while (primero < K) {
            cpuNivel(primero, hay, ahora);
            if (hay) break;
            ++primero;
        }
        if (primero < K && activos < primero + 1) activos = primero + 1;
        if (disp > 0 && activos > disp) activos = std::max(1, disp);  // se cayó una VM de las activas
        if (activos < 1) activos = 1;

        const int ultimo = activos - 1;
        double c = cpuNivel(ultimo, hay, ahora);  // CPU del último nivel activo (decide el scale-in)
        double cPrev = 0;                         // CPU de los niveles anteriores (holgura para el scale-in)
        if (ultimo > 0) cPrev = cpuNivel(ultimo - 1, hay, ahora);
        double cOut = c;                          // CPU que decide el scale-out y la cubeta
        int caliente = ultimo;                    // nivel al que pertenece cOut
        if (cfg.metrica == "max") {
            cPrev = 0;
            for (int n = 0; n <= ultimo; ++n) {
                double v = cpuNivel(n, hay, ahora);
                if (v > cOut) {
                    cOut = v;
                    caliente = n;
                }
                if (n < ultimo && v > cPrev) cPrev = v;
            }
        }
        bool enfriado = ahora - tCambio >= cfg.enfriar;
        if (cOut >= cfg.umbralOut) {
            if (tAlto == 0.0) tAlto = ahora;
        } else {
            tAlto = 0.0;
        }
        if (c < cfg.umbralIn) {
            if (tBajo == 0.0) tBajo = ahora;
        } else {
            tBajo = 0.0;
        }
        bool altoSostenido = tAlto > 0.0 && ahora - tAlto >= cfg.sostenidoOut;

        if (activos < disp) {
            if (altoSostenido && enfriado) {  // scale-out
                ++activos;
                tCambio = ahora;
                tAlto = tBajo = 0.0;
                logLinea(cfg.nombre, "ESCALA-OUT: CPU de '" + cfg.niveles[caliente] + "' = " + fmt0(cOut) + "% >= " +
                                         fmt0(cfg.umbralOut) + "% -> se activa '" + cfg.niveles[activos - 1] +
                                         "' (" + std::to_string(activos) + "/" + std::to_string(K) + " niveles)");
            }
        } else if (cfg.cubeta > 0 && !cubetaActiva && altoSostenido) {  // ya no hay más VMs: entra la cubeta
            cubetaActiva = true;
            tCubetaBaja = 0.0;
            fichas = 1.0;
            logLinea(cfg.nombre, "CUBETA ACTIVA: todos los niveles disponibles (" + std::to_string(activos) +
                                     ") están al límite (CPU '" + cfg.niveles[caliente] + "' = " + fmt0(cOut) +
                                     "%); salida " + (cfg.fuga > 0 ? fmt0(cfg.fuga) + "/s" : std::string("libre")) +
                                     ", capacidad " + std::to_string(cfg.cubeta));
        }
        if (cubetaActiva) {  // la cubeta se libera cuando hay holgura (o se activó otro nivel)
            if (cOut < cfg.umbralOut - 10.0 || activos < disp) {
                if (tCubetaBaja == 0.0) tCubetaBaja = ahora;
                if (ahora - tCubetaBaja >= cfg.sostenidoOut) {
                    cubetaActiva = false;
                    tCubetaBaja = 0.0;
                    logLinea(cfg.nombre, "CUBETA LIBERADA: CPU de '" + cfg.niveles[caliente] + "' = " + fmt0(cOut) + "%");
                }
            } else {
                tCubetaBaja = 0.0;
            }
        }
        // scale-in: el último nivel casi no trabaja y el anterior aguantaría la carga con holgura
        if (activos > 1 && !cubetaActiva && tBajo > 0.0 && ahora - tBajo >= cfg.sostenidoIn && enfriado &&
            cPrev < cfg.umbralOut - 10.0) {
            logLinea(cfg.nombre, "ESCALA-IN: CPU de '" + cfg.niveles[ultimo] + "' = " + fmt0(c) + "% < " +
                                     fmt0(cfg.umbralIn) + "% -> se libera (" + std::to_string(activos - 1) + "/" +
                                     std::to_string(K) + " niveles)");
            --activos;
            tCambio = ahora;
            tAlto = tBajo = 0.0;
        }
    }

    // Una vez por segundo: evalúa el escalado y escribe la línea de tiempo.
    void cadaSegundo(double ahora) {
        evaluarEscala(ahora);
        if (cfg.lineaTiempo.empty()) return;
        if (!tl.is_open()) {
            bool existe = std::ifstream(cfg.lineaTiempo).good();
            tl.open(cfg.lineaTiempo, std::ios::app);
            if (!existe) {
                tl << "t_s,hora,niveles_activos,cubeta,cola,respondidas,sobrecarga";
                for (const std::string& n : cfg.niveles) tl << ",cpu_" << n;
                for (const std::string& n : cfg.niveles) tl << ",desp_" << n;
                tl << ",desp_otros\n";
            }
        }
        char hora[16];
        std::time_t tt = std::time(nullptr);
        std::strftime(hora, sizeof hora, "%H:%M:%S", std::localtime(&tt));
        char b[96];
        std::snprintf(b, sizeof b, "%.0f,%s,%d,%d,%zu,%ld,%ld", ahora - tInicio, hora, activos,
                      cubetaActiva ? 1 : 0, cola.size(), respondidasSeg, sobrecargasSeg);
        tl << b;
        bool hay = false;
        for (size_t i = 0; i < cfg.niveles.size(); ++i) {
            std::snprintf(b, sizeof b, ",%.1f", cpuNivel(static_cast<int>(i), hay, ahora));
            tl << b;
        }
        for (size_t i = 0; i <= cfg.niveles.size(); ++i) tl << ',' << despSeg[i];
        tl << '\n';
        tl.flush();
        respondidasSeg = sobrecargasSeg = 0;
        for (long& v : despSeg) v = 0;
    }

    // La solicitud vuelve al frente de la cola. Es seguro repetirla: Persistencia reconoce
    // los duplicados por (idCliente, idMensaje).
    void reasignar(unsigned long corr) {
        auto p = pendientes.find(corr);
        if (p == pendientes.end()) return;
        p->second.trabajador.clear();
        cola.push_front(corr);
        ++reasignadas;
    }

    // ------------------------------------------------------------- validación --
    static bool fechaValida(int f) {
        int a = f / 10000, m = (f / 100) % 100, d = f % 100;
        return a >= 2000 && a <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
    }

    // Validación básica antes de reenviar a otros componentes.
    static bool validar(const Mensaje& m, std::string& motivo) {
        if (m.idCliente < 1 || m.idCliente > 1000000) {
            motivo = "idCliente fuera de rango";
            return false;
        }
        if (m.tipo != TipoOperacion::CONSULTA && m.idMensaje < 1) {
            motivo = "idMensaje inválido";
            return false;
        }
        switch (m.tipo) {
            case TipoOperacion::CONSULTA: {
                int anio = m.mes / 100, mes = m.mes % 100;
                if (anio < 2000 || anio > 2100 || mes < 1 || mes > 12) {
                    motivo = "mes fuera de rango (AAAAMM)";
                    return false;
                }
                return true;
            }
            case TipoOperacion::RESERVA:
                if (m.idEvento < 1 || m.idEvento > 100000 || m.idOcurrencia < 1 ||
                    m.idOcurrencia > 100) {
                    motivo = "evento/ocurrencia fuera de rango";
                    return false;
                }
                if (m.numAsientos < 1 || m.numAsientos > 100) {
                    motivo = "numAsientos fuera de rango";
                    return false;
                }
                return true;
            case TipoOperacion::MODIFICACION:
                if (m.idReserva < 1) {
                    motivo = "idReserva inválido";
                    return false;
                }
                if (m.fechaNueva > 0) {
                    if (!fechaValida(m.fechaNueva)) {
                        motivo = "fechaNueva inválida (AAAAMMDD)";
                        return false;
                    }
                } else if (m.numAsientosNuevo < 1 || m.numAsientosNuevo > 100) {
                    motivo = "numAsientosNuevo fuera de rango";
                    return false;
                }
                return true;
            case TipoOperacion::CANCELACION:
                if (m.idReserva < 1) {
                    motivo = "idReserva inválido";
                    return false;
                }
                return true;
        }
        return false;
    }

    // ----------------------------------------------------------- estadísticas --
    // Se cuenta al responder (transacciones procesadas), separando el resultado.
    void contar(TipoOperacion tipo, Estado resultado) {
        contadores[static_cast<int>(tipo)][static_cast<int>(resultado)]++;
        tUltima = ahoraSeg();
    }

    long totalContadas() const {
        long t = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) t += contadores[i][j];
        return t;
    }

    void volcarEstadisticas(bool reiniciar) {
        double ventana = (tUltima > tPrimera) ? (tUltima - tPrimera) : 0.0;
        double minutos = ventana / 60.0;
        std::ofstream csv;
        if (!cfg.statsCsv.empty()) {
            bool existe = std::ifstream(cfg.statsCsv).good();
            csv.open(cfg.statsCsv, std::ios::app);
            if (!existe) csv << "momento,tipo,total,exito,rechazo,error,tpm,ventana_s\n";
        }
        char momento[32];
        std::time_t t = std::time(nullptr);
        std::strftime(momento, sizeof momento, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        for (int i = 0; i < 4; ++i) {
            long n = contadores[i][0] + contadores[i][1] + contadores[i][2];
            if (n == 0) continue;
            char b[200];
            std::snprintf(b, sizeof b, "%s: total=%ld exito=%ld rechazo=%ld error=%ld  TPM=%.1f",
                          nombre(static_cast<TipoOperacion>(i)), n, contadores[i][0],
                          contadores[i][1], contadores[i][2], minutos > 0 ? n / minutos : 0.0);
            logLinea("GT-STATS", b);
            if (csv.is_open())
                csv << momento << ',' << nombre(static_cast<TipoOperacion>(i)) << ',' << n << ','
                    << contadores[i][0] << ',' << contadores[i][1] << ',' << contadores[i][2] << ','
                    << (minutos > 0 ? n / minutos : 0.0) << ',' << ventana << '\n';
        }
        if (!repartoVM.empty()) {
            std::string s;
            long total = 0;
            for (auto& kv : repartoVM) total += kv.second;
            for (auto& kv : repartoVM) {
                char b[96];
                std::snprintf(b, sizeof b, "%s=%ld (%.1f%%)  ", kv.first.c_str(), kv.second,
                              total ? 100.0 * kv.second / total : 0.0);
                s += b;
                if (csv.is_open())
                    csv << momento << ",REPARTO_" << kv.first << ',' << kv.second << ",,,,,\n";
            }
            logLinea("GT-STATS", "reparto entre VMs: " + s);
        }
        if (invalidas || reasignadas || expiradas || sobrecargas)
            logLinea("GT-STATS", "inválidas=" + std::to_string(invalidas) + " reasignadas=" +
                                     std::to_string(reasignadas) + " expiradas=" +
                                     std::to_string(expiradas) + " sobrecarga=" + std::to_string(sobrecargas));
        if (!cfg.niveles.empty())
            logLinea("GT-STATS", "niveles activos=" + std::to_string(activos) + "/" +
                                     std::to_string(cfg.niveles.size()) + (cubetaActiva ? "  CUBETA ACTIVA" : ""));
        if (reiniciar) {
            for (auto& fila : contadores)
                for (long& v : fila) v = 0;
            repartoVM.clear();
            for (auto& kv : trabajadores) kv.second.despachadas = 0;
            invalidas = reasignadas = expiradas = sobrecargas = 0;
            tPrimera = tUltima = 0.0;
        }
    }

    static constexpr size_t kMaxBytes = 4096;

    Config cfg;
    zmq::context_t ctx_;
    zmq::socket_t sockClientes;  // ROUTER (clientes)
    zmq::socket_t sockRMC;       // ROUTER (réplicas del RMC)
    zmq::socket_t sockCancel;    // PUSH (Cancelaciones)
    std::unordered_map<std::string, Trabajador> trabajadores;
    std::deque<std::string> libres;       // RMC libres, el que lleva más tiempo sin trabajar primero
    std::deque<unsigned long> cola;       // solicitudes esperando un RMC
    std::unordered_map<unsigned long, Pendiente> pendientes;
    std::map<std::string, long> repartoVM;
    unsigned long secuencia = 0;
    long contadores[4][3] = {};
    long invalidas = 0, reasignadas = 0, expiradas = 0, sobrecargas = 0;
    double tPrimera = 0.0, tUltima = 0.0;
    // escalado y cubeta
    int activos = 1;               // niveles activos
    bool cubetaActiva = false;
    double tAlto = 0.0, tBajo = 0.0, tCambio = -1e9, tCubetaBaja = 0.0;
    double fichas = 1.0, tFichas = 0.0;
    double tInicio = 0.0;
    long respondidasSeg = 0, sobrecargasSeg = 0;
    std::vector<long> despSeg = std::vector<long>(64, 0);  // despachos del último segundo por nivel (+ otros)
    std::ofstream tl;
};

int main(int argc, char** argv) {
    instalarSenales();
    g_silencio = argFlag(argc, argv, "--silencioso");
    GestorTransacciones::Config c;
    c.bindClientes = argStr(argc, argv, "--bind", c.bindClientes);
    c.bindRMC = argStr(argc, argv, "--bind-rmc", c.bindRMC);
    c.epCancel = argStr(argc, argv, "--cancel", c.epCancel);
    c.hbMs = argInt(argc, argv, "--hb-ms", c.hbMs);
    c.liveness = argInt(argc, argv, "--liveness", c.liveness);
    c.ttl = argDbl(argc, argv, "--ttl", c.ttl);
    c.statsCsv = argStr(argc, argv, "--stats-csv", "");
    c.nombre = argStr(argc, argv, "--nombre", c.nombre);
    for (const std::string& n : dividir(argStr(argc, argv, "--escalar", ""), ','))
        if (!n.empty()) c.niveles.push_back(n);
    if (c.niveles.size() > 60) c.niveles.resize(60);
    c.umbralOut = argDbl(argc, argv, "--umbral-out", c.umbralOut);
    c.umbralIn = argDbl(argc, argv, "--umbral-in", c.umbralIn);
    c.metrica = argStr(argc, argv, "--escala-metrica", c.metrica) == "ultimo" ? "ultimo" : "max";
    c.sostenidoOut = argDbl(argc, argv, "--sostenido-out", c.sostenidoOut);
    c.sostenidoIn = argDbl(argc, argv, "--sostenido-in", c.sostenidoIn);
    c.enfriar = argDbl(argc, argv, "--enfriar", c.enfriar);
    c.cubeta = argInt(argc, argv, "--cubeta", c.cubeta);
    c.fuga = argDbl(argc, argv, "--fuga", c.fuga);
    c.lineaTiempo = argStr(argc, argv, "--linea-tiempo", "");
    try {
        GestorTransacciones gt(c);
        gt.iniciar();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
