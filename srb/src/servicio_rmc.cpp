// servicio_rmc.cpp  (capa de lógica de negocio)
// Servicio de Reservas, Modificaciones y Consultas. Se lanza uno por VM con varios hilos
// (--hilos); cada hilo es una réplica del RMC:
//   - socket DEALER hacia cada Gestor de Transacciones (titular y de relevo): se registra con
//     READY, atiende una solicitud a la vez y envía heartbeats (patrón Paranoid Pirate).
//     Si un GT deja de dar señales, reabre el socket y se vuelve a registrar.
//   - socket REQ hacia Persistencia (con timeout).
//
// Uso: ./rmc --gt tcp://IP_GT:5556[,tcp://IP_GT_RELEVO:5556] --persistencia tcp://IP_BD:5559
//            [--hilos 2] [--nombre vm4] [--hb-ms 500] [--liveness 4] [--timeout-ms 3000]
//            [--cpu-simulada archivo] [--carga-cpu-ms N] [--silencioso]
// Cada READY/heartbeat lleva la CPU del nodo (% de todos sus núcleos): el GT la usa para decidir cuándo
// activar la siguiente VM (scale-out) y cuándo liberarla (scale-in). --cpu-simulada lee ese valor de un
// archivo (para ensayar el escalado sin generar carga real).
// --carga-cpu-ms N: cada solicitud consume además N ms de CPU (un bucle de cálculo, no una espera) antes de ejecutarse.
// Es una CARGA SINTÉTICA para pruebas de estrés: simula una lógica de negocio costosa (validaciones, precios, reglas)
// y permite que la capa lógica (VM3/VM4/VM5) se sature antes que la BD, que es lo que hace visible el escalado. Por
// defecto vale 0 (sin carga extra) y el comportamiento es el de siempre.
#include <ctime>
#include <thread>

#include "common.hpp"

using namespace srb;

// Consume `ms` milisegundos de CPU del hilo actual (tiempo de CPU, no de reloj: si la VM está saturada y el hilo
// espera turno, el trabajo sigue costando `ms` de CPU y la solicitud tarda más, como en una lógica real).
static void quemarCpu(double ms) {
    if (ms <= 0) return;
    timespec t0;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
    volatile unsigned long long x = 88172645463325252ULL;
    for (;;) {
        for (int i = 0; i < 2000; ++i) {
            unsigned long long v = x;
            v ^= v << 13;
            v ^= v >> 7;
            v ^= v << 17;
            x = v;
        }
        timespec t;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000.0 + (t.tv_nsec - t0.tv_nsec) / 1e6 >= ms) break;
    }
}

struct ConfigRMC {
    std::vector<std::string> gts;
    std::string persistencia = "tcp://localhost:5559";
    int hilos = 2;
    std::string nombre;
    int hbMs = 500;
    int liveness = 4;
    int timeoutMs = 3000;
    std::string cpuSimulada;
    double cargaCpuMs = 0.0;  // CPU sintética por solicitud (ms); 0 = ninguna
};

class ServicioRMC {
public:
    ServicioRMC(zmq::context_t& ctx, const ConfigRMC& cfg, int idx, MedidorCPU& cpu)
        : ctx_(ctx),
          cfg_(cfg),
          cpu_(cpu),
          persistencia(ctx, cfg.persistencia, cfg.timeoutMs),
          idRMC(cfg.nombre + "/h" + std::to_string(idx) + "#" + std::to_string(getpid())),
          tag("RMC[" + cfg.nombre + "/h" + std::to_string(idx) + "]") {}

    void iniciar() {
        enlaces.resize(cfg_.gts.size());
        for (size_t i = 0; i < enlaces.size(); ++i) abrir(i, true);
        std::vector<zmq_pollitem_t> items(enlaces.size());
        const double silencioMax = cfg_.hbMs * cfg_.liveness / 1000.0;
        std::vector<std::string> f;
        while (!g_parar) {
            for (size_t i = 0; i < enlaces.size(); ++i)
                items[i] = {static_cast<void*>(enlaces[i].sock), 0, ZMQ_POLLIN, 0};
            zmq_poll(items.data(), static_cast<int>(items.size()), cfg_.hbMs / 2 > 0 ? cfg_.hbMs / 2 : 1);
            double ahora = ahoraSeg();
            for (size_t i = 0; i < enlaces.size(); ++i) {
                if (items[i].revents & ZMQ_POLLIN) {
                    if (!recibirMultipart(enlaces[i].sock, f) || f.empty()) continue;
                    enlaces[i].ultimoRx = ahora;
                    if (enlaces[i].caido) {
                        enlaces[i].caido = false;
                        logLinea(tag, "GT " + cfg_.gts[i] + " responde de nuevo");
                    }
                    if (f[0] == CMD_REQ && f.size() >= 3) atender(i, f[1], f[2]);
                    ahora = ahoraSeg();
                }
                if (ahora >= enlaces[i].proximoHB) {
                    enlaces[i].proximoHB = ahora + cfg_.hbMs / 1000.0;
                    enviarMultipart(enlaces[i].sock, {CMD_HB, cpuTexto()}, true);
                }
                if (ahora - enlaces[i].ultimoRx > silencioMax) {
                    if (!enlaces[i].caido) {
                        enlaces[i].caido = true;
                        logLinea(tag, "GT " + cfg_.gts[i] + " sin señales; se intenta reconectar");
                    }
                    abrir(i, false);  // socket nuevo y READY otra vez
                }
            }
        }
        logOp(tag, "deteniendo");
    }

    // Consulta de los eventos de un mes: se reenvía a Persistencia y se devuelve su respuesta.
    Mensaje atenderConsulta(const Mensaje& m) {
        Mensaje r;
        if (!persistencia.pedir(m, r)) return fallo(m, "consulta");
        return r;
    }

    // Reserva en dos pasos: (1) disponibilidad, (2) confirmación. El paso 2 vuelve a verificar
    // dentro de la transacción de la BD, así que no hay sobreventa aunque otro cliente se cuele.
    Mensaje atenderReserva(const Mensaje& m) {
        Mensaje disp = m;
        disp.tipo = TipoOperacion::CONSULTA;  // CONSULTA con mes == 0 -> disponibilidad
        disp.mes = 0;
        disp.eventos.clear();
        Mensaje rd;
        if (!persistencia.pedir(disp, rd)) return fallo(m, "disponibilidad");
        // Si Persistencia contesta con otro tipo, esta misma solicitud ya se había ejecutado (reasignada o
        // reintentada): su resultado guardado es la respuesta; no se decide nada con la disponibilidad actual.
        if (rd.tipo != TipoOperacion::CONSULTA) return rd;
        if (rd.resultado == Estado::ERROR) return fallo(m, "disponibilidad");
        if (rd.resultado == Estado::RECHAZO || rd.eventos.empty() ||
            rd.eventos.front().asientosLibres < m.numAsientos) {
            Mensaje r = m;
            r.resultado = Estado::RECHAZO;  // evento inexistente o sin asientos suficientes
            return r;
        }
        Mensaje r;
        if (!persistencia.pedir(m, r)) return fallo(m, "confirmación de la reserva");
        return r;
    }

    // Modificación: (1) se consulta la disponibilidad (en la otra ocurrencia o de más asientos
    // en la misma); (2) si hay, se aplica el cambio en Persistencia; si no, se rechaza y la
    // reserva queda como estaba.
    Mensaje atenderModificacion(const Mensaje& m) {
        int necesarios = 0;
        Mensaje disp = m;
        disp.tipo = TipoOperacion::CONSULTA;
        disp.mes = 0;
        disp.eventos.clear();
        bool consultar = false;
        if (m.fechaNueva > 0) {  // otra ocurrencia: hacen falta los mismos asientos
            consultar = true;
            necesarios = m.numAsientos;
        } else if (m.numAsientos > 0 && m.numAsientosNuevo > m.numAsientos) {  // más asientos
            consultar = true;
            necesarios = m.numAsientosNuevo - m.numAsientos;
            disp.fechaNueva = 0;
        }
        if (consultar) {
            Mensaje rd;
            if (!persistencia.pedir(disp, rd)) return fallo(m, "disponibilidad");
            if (rd.tipo != TipoOperacion::CONSULTA) return rd;  // ya ejecutada antes: resultado guardado
            if (rd.resultado == Estado::ERROR) return fallo(m, "disponibilidad");
            if (rd.resultado == Estado::RECHAZO || rd.eventos.empty() ||
                (necesarios > 0 && rd.eventos.front().asientosLibres < necesarios)) {
                Mensaje r = m;
                r.resultado = Estado::RECHAZO;
                return r;
            }
        }
        Mensaje r;
        if (!persistencia.pedir(m, r)) return fallo(m, "modificación");
        return r;
    }

private:
    struct Enlace {
        zmq::socket_t sock;
        double ultimoRx = 0;
        double proximoHB = 0;
        bool caido = false;
    };

    std::string cpuTexto() {
        char b[16];
        std::snprintf(b, sizeof b, "%.1f", cpu_.porcentaje());
        return b;
    }

    Mensaje fallo(const Mensaje& m, const char* donde) {
        Mensaje r = m;
        r.resultado = Estado::ERROR;
        logLinea(tag, std::string("sin respuesta de Persistencia (") + donde + ")");
        return r;
    }

    // Crea (o recrea) el socket hacia el GT i y se registra con READY.
    void abrir(size_t i, bool primera) {
        Enlace& e = enlaces[i];
        e.sock = crearSocket(ctx_, zmq::socket_type::dealer);
        fijarIdentidad(e.sock, idRMC);
        e.sock.connect(cfg_.gts[i]);
        double ahora = ahoraSeg();
        e.ultimoRx = ahora;
        e.proximoHB = ahora + cfg_.hbMs / 1000.0;
        enviarMultipart(e.sock, {CMD_READY, cpuTexto()}, true);
        if (primera) logLinea(tag, "registrado en el GT " + cfg_.gts[i]);
    }

    void atender(size_t i, const std::string& corr, const std::string& payload) {
        Mensaje m, r;
        if (!deserializar(payload, m)) {
            r.resultado = Estado::ERROR;
            logLinea(tag, "mensaje mal formado -> ERROR");
        } else {
            quemarCpu(cfg_.cargaCpuMs);
            switch (m.tipo) {
                case TipoOperacion::CONSULTA: r = atenderConsulta(m); break;
                case TipoOperacion::RESERVA: r = atenderReserva(m); break;
                case TipoOperacion::MODIFICACION: r = atenderModificacion(m); break;
                default:
                    r = m;
                    r.resultado = Estado::ERROR;  // las cancelaciones no pasan por el RMC
            }
            logOp(tag, describir(m) + " -> " + nombre(r.resultado));
        }
        enviarMultipart(enlaces[i].sock, {CMD_REP, corr, serializar(r)}, true);
    }

    zmq::context_t& ctx_;
    ConfigRMC cfg_;
    MedidorCPU& cpu_;
    EnlaceREQ persistencia;
    std::string idRMC;
    std::string tag;
    std::vector<Enlace> enlaces;
};

int main(int argc, char** argv) {
    instalarSenales();
    g_silencio = argFlag(argc, argv, "--silencioso");
    ConfigRMC c;
    c.gts = dividir(argStr(argc, argv, "--gt", "tcp://localhost:5556"), ',');
    c.persistencia = argStr(argc, argv, "--persistencia", c.persistencia);
    c.hilos = argInt(argc, argv, "--hilos", c.hilos);
    if (c.hilos < 1) c.hilos = 1;
    c.nombre = argStr(argc, argv, "--nombre", nombreMaquina());
    c.hbMs = argInt(argc, argv, "--hb-ms", c.hbMs);
    c.liveness = argInt(argc, argv, "--liveness", c.liveness);
    c.timeoutMs = argInt(argc, argv, "--timeout-ms", c.timeoutMs);
    c.cpuSimulada = argStr(argc, argv, "--cpu-simulada", "");
    c.cargaCpuMs = argDbl(argc, argv, "--carga-cpu-ms", 0.0);
    try {
        zmq::context_t ctx(1);
        MedidorCPU cpu(c.cpuSimulada);
        std::vector<std::thread> hilos;
        for (int i = 0; i < c.hilos; ++i)
            hilos.emplace_back([&ctx, &c, &cpu, i]() {
                try {
                    ServicioRMC rmc(ctx, c, i, cpu);
                    rmc.iniciar();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "Hilo RMC %d terminó: %s\n", i, e.what());
                }
            });
        logLinea("RMC", c.nombre + ": " + std::to_string(c.hilos) + " réplicas en marcha");
        for (auto& h : hilos) h.join();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
