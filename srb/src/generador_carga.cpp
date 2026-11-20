// generador_carga.cpp  (capa de consumidores)
// GeneradorCarga crea un hilo por Cliente. Cada Cliente envía solicitudes al Gestor de
// Transacciones (REQ, con reintentos hacia el GT de relevo si el principal no responde) y
// escucha notificaciones (SUB). Metricas guarda los tiempos de respuesta medidos con el reloj
// del propio cliente.
//
// Reparto entre VMs: se lanza el MISMO comando en cada VM de clientes, cambiando solo
// --vm-indice (0, 1, ...). Cada proceso atiende los clientes cuyo índice global módulo
// --vm-total coincide con su --vm-indice, así 8 clientes quedan 4 + 4 y el perfil de cada
// cliente (solo consulta / mixto) se conserva.
//
// Uso: ./generador --clientes 8 --solo-consulta 4 --rpm 60 --duracion 120
//        [--gt tcp://IP2:5555[,tcp://IP_RELEVO:5555]] [--notif tcp://IP2:5558]
//        [--vm-indice 0 --vm-total 2]
//        [--pct-consulta 40 --pct-reserva 40 --pct-modificar 20 --pct-cancelar 0]
//        [--mes 202611] [--eventos 50] [--dobles 20] [--max-asientos 4]
//        [--calentamiento 5] [--timeout-ms 4000] [--reintentos 3] [--semilla 1] [--backoff-ms 50]
//        [--arquitectura base|escalable] [--repeticion 1]
//        [--csv resumen.csv] [--muestras muestras.csv] [--en-vivo SEG]
//   --rpm 0  = sin pausa entre solicitudes (máxima carga).
//   --en-vivo SEG  imprime cada SEG segundos una línea "[EN VIVO]" con las solicitudes enviadas, respondidas,
//                  el ritmo por segundo, las que están en curso, las rechazadas por SOBRECARGA y las fallas
//                  (0 = apagado, por defecto). Cuenta TODO lo que ve el generador, incluido el calentamiento.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <thread>

#include "common.hpp"

using namespace srb;

// ---------------------------------------------------------------- Metricas --
struct Muestra {
    int tipo;
    double ms;
    double t;  // segundos desde el inicio de la corrida
    int resultado;
    int intentos;
};

class Metricas {
public:
    // Registra una operación completada (con respuesta) o con timeout.
    void registrar(TipoOperacion tipo, double ms, double t, Estado e, int intentos, int cambios) {
        std::lock_guard<std::mutex> g(mtx);
        int i = idx(tipo);
        if (e == Estado::SOBRECARGA) {  // rechazada de inmediato por la cubeta del GT: no entra en los tiempos
            sobrecargas[i]++;
            muestras.push_back({static_cast<int>(tipo), ms, t, static_cast<int>(e), intentos});
            return;
        }
        tiempos[i].push_back(ms);
        resultados[i][static_cast<int>(e)]++;
        if (intentos > 1) reintentos++;
        failovers += cambios;
        muestras.push_back({static_cast<int>(tipo), ms, t, static_cast<int>(e), intentos});
    }
    void registrarTimeout(TipoOperacion tipo, double t, int intentos, int cambios) {
        std::lock_guard<std::mutex> g(mtx);
        timeouts[idx(tipo)]++;  // aparte, para no distorsionar los promedios
        failovers += cambios;
        muestras.push_back({static_cast<int>(tipo), -1.0, t, -1, intentos});  // resultado -1 = timeout
    }
    void registrarCancelacion() { std::lock_guard<std::mutex> g(mtx); cancelaciones++; }
    void registrarNotificacion() { std::lock_guard<std::mutex> g(mtx); notificaciones++; }
    // Cuenta TODAS las reservas confirmadas y los timeouts de reserva (incluso en el
    // calentamiento): se contrastan con las filas de la tabla reserva de la BD.
    void contarReservaTotal(bool exito, bool timeout) {
        std::lock_guard<std::mutex> g(mtx);
        if (exito) reservasExitoTotal++;
        if (timeout) reservasTimeoutTotal++;
    }

    size_t n(TipoOperacion t) const { std::lock_guard<std::mutex> g(mtx); return tiempos[idx(t)].size(); }
    double promedio(TipoOperacion t) const {
        std::lock_guard<std::mutex> g(mtx);
        const auto& v = tiempos[idx(t)];
        if (v.empty()) return 0.0;
        double s = 0;
        for (double x : v) s += x;
        return s / v.size();
    }
    // Desviación estándar muestral (n-1).
    double desviacionEstandar(TipoOperacion t) const {
        double p = promedio(t);
        std::lock_guard<std::mutex> g(mtx);
        const auto& v = tiempos[idx(t)];
        if (v.size() < 2) return 0.0;
        double s = 0;
        for (double x : v) s += (x - p) * (x - p);
        return std::sqrt(s / (v.size() - 1));
    }
    double percentil95(TipoOperacion t) const {
        std::lock_guard<std::mutex> g(mtx);
        std::vector<double> v = tiempos[idx(t)];
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        size_t i = static_cast<size_t>(std::ceil(0.95 * v.size())) - 1;
        return v[std::min(i, v.size() - 1)];
    }
    // Transacciones por minuto vistas por el generador en la ventana medida.
    double tpm(TipoOperacion t, double ventanaSeg) const {
        return ventanaSeg > 0 ? n(t) / (ventanaSeg / 60.0) : 0.0;
    }
    long getResultado(TipoOperacion t, Estado e) const {
        std::lock_guard<std::mutex> g(mtx);
        return resultados[idx(t)][static_cast<int>(e)];
    }
    long getTimeouts(TipoOperacion t) const { std::lock_guard<std::mutex> g(mtx); return timeouts[idx(t)]; }
    long getSobrecargas() const {
        std::lock_guard<std::mutex> g(mtx);
        return sobrecargas[0] + sobrecargas[1] + sobrecargas[2];
    }
    long getReintentos() const { std::lock_guard<std::mutex> g(mtx); return reintentos; }
    long getFailovers() const { std::lock_guard<std::mutex> g(mtx); return failovers; }
    long getCancelaciones() const { std::lock_guard<std::mutex> g(mtx); return cancelaciones; }
    long getNotificaciones() const { std::lock_guard<std::mutex> g(mtx); return notificaciones; }
    long getReservasExitoTotal() const { std::lock_guard<std::mutex> g(mtx); return reservasExitoTotal; }
    long getReservasTimeoutTotal() const { std::lock_guard<std::mutex> g(mtx); return reservasTimeoutTotal; }
    std::vector<Muestra> getMuestras() const { std::lock_guard<std::mutex> g(mtx); return muestras; }

private:
    static int idx(TipoOperacion t) {
        return t == TipoOperacion::CONSULTA ? 0 : (t == TipoOperacion::RESERVA ? 1 : 2);
    }
    mutable std::mutex mtx;
    std::vector<double> tiempos[3];  // consulta, reserva, modificación
    long resultados[3][4] = {};
    long timeouts[3] = {0, 0, 0};
    long sobrecargas[3] = {0, 0, 0};
    long reintentos = 0, failovers = 0, cancelaciones = 0, notificaciones = 0;
    long reservasExitoTotal = 0, reservasTimeoutTotal = 0;
    std::vector<Muestra> muestras;
};

// ----------------------------------------------------------------- Cliente --
class Cliente {
public:
    Cliente(zmq::context_t& ctx, int id, const std::vector<std::string>& gts,
            const std::string& epNotif, int timeoutMs, int reintentos)
        : idCliente(id),
          sockReq(ctx, gts, timeoutMs, reintentos),
          sockSub(crearSocket(ctx, zmq::socket_type::sub)) {
        if (!epNotif.empty()) {
            sockSub.connect(epNotif);
            std::string topico = "c" + std::to_string(id) + "|";
            sockSub.set(zmq::sockopt::subscribe, topico);
        }
    }

    // false = no llegó respuesta tras todos los intentos.
    bool enviarSolicitud(const Mensaje& m, Mensaje& respuesta, int& intentos, int& cambios) {
        return sockReq.pedir(m, respuesta, intentos, cambios);
    }

    // Espera hasta esperaMs por una notificación de cancelación efectiva.
    bool recibirNotificacion(Mensaje& out, int esperaMs) {
        if (!hayEntrada(sockSub, esperaMs)) return false;
        std::vector<std::string> t;
        if (!recibirMultipart(sockSub, t) || t.size() < 2) return false;
        return deserializar(t.back(), out);
    }

    int idCliente;

private:
    EnlaceGT sockReq;
    zmq::socket_t sockSub;
};

// ----------------------------------------------------------- GeneradorCarga --
struct Parametros {
    std::vector<std::string> gts;
    std::string notif, arquitectura, csv, muestras;
    int clientes, soloConsulta, rpm, duracion, calentamiento, timeoutMs, reintentos, semilla;
    int pctConsulta, pctReserva, pctModificar, pctCancelar;
    int backoffMs = 50;  // espera tras un rechazo por SOBRECARGA
    int enVivo = 0;      // cada cuántos segundos se imprime el contador en vivo (0 = apagado)
    int mes, eventos, dobles, maxAsientos, vmIndice, vmTotal, repeticion;
};

struct ReservaPropia {
    int idReserva, idEvento, idOcurrencia, numAsientos;
};

// Contadores en vivo: atómicos y sin mutex, para que contar no frene a los clientes.
struct Vivo {
    std::atomic<long> enviadas{0}, respondidas{0}, rechazadas{0}, timeouts{0}, errores{0};
    std::atomic<long> porTipo[3];  // respondidas: consulta, reserva, modificación
    Vivo() { for (auto& a : porTipo) a.store(0); }
};

class GeneradorCarga {
public:
    explicit GeneradorCarga(const Parametros& p) : p_(p), ctx_(1) {}

    void iniciar() {
        std::vector<int> mios;
        for (int i = 0; i < p_.clientes; ++i)
            if (i % p_.vmTotal == p_.vmIndice) mios.push_back(i);
        int soloMios = 0;
        for (int i : mios) soloMios += (i < p_.soloConsulta);
        logLinea("CARGA", "VM " + std::to_string(p_.vmIndice) + "/" + std::to_string(p_.vmTotal) +
                              ": " + std::to_string(mios.size()) + " de " + std::to_string(p_.clientes) +
                              " clientes (solo consulta: " + std::to_string(soloMios) + ") rpm=" +
                              std::to_string(p_.rpm) + " duracion=" + std::to_string(p_.duracion) +
                              "s calentamiento=" + std::to_string(p_.calentamiento) + "s");
        // Los idMensaje son únicos por corrida: así una corrida nueva nunca se confunde con
        // un reintento de una anterior que sigue guardada en la BD.
        base_ = static_cast<long>(std::time(nullptr) % 10000000L) * 1000000L;
        t0_ = ahoraSeg();
        std::vector<std::thread> hilos;
        std::atomic<bool> terminado{false};
        std::thread vivo;
        if (p_.enVivo > 0) vivo = std::thread(&GeneradorCarga::imprimirVivo, this, static_cast<int>(mios.size()), &terminado);
        for (int i : mios) hilos.emplace_back(&GeneradorCarga::cicloCliente, this, i);
        for (auto& h : hilos) h.join();
        terminado = true;
        if (vivo.joinable()) vivo.join();
        informar(static_cast<int>(mios.size()));
    }

private:
    // Cuenta en vivo el resultado de una solicitud ya enviada.
    void contarVivo(TipoOperacion tipo, bool ok, Estado e) {
        if (!ok) { vivo_.timeouts++; return; }
        if (e == Estado::SOBRECARGA) { vivo_.rechazadas++; return; }
        vivo_.respondidas++;
        if (e == Estado::ERROR) vivo_.errores++;
        int i = tipo == TipoOperacion::CONSULTA ? 0 : (tipo == TipoOperacion::RESERVA ? 1 : 2);
        vivo_.porTipo[i]++;
    }

    // Una línea cada p_.enVivo segundos con lo que está pasando AHORA (hilo aparte, solo lee los contadores).
    void imprimirVivo(int clientesAqui, std::atomic<bool>* terminado) {
        const double paso = p_.enVivo;
        double proximo = t0_ + paso, anterior = t0_;
        long hechasAntes = 0;
        bool ultima = false;
        while (!ultima) {
            while (!*terminado && ahoraSeg() < proximo)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ultima = *terminado;
            double ahora = ahoraSeg();
            long env = vivo_.enviadas, res = vivo_.respondidas, rech = vivo_.rechazadas, to = vivo_.timeouts;
            long hechas = res + rech + to;
            double dt = std::max(0.001, ahora - anterior);
            // en la línea final se muestra el ritmo medio de toda la corrida (el último intervalo es casi vacío)
            double ritmo = ultima ? hechas / std::max(0.001, ahora - t0_) : (hechas - hechasAntes) / dt;
            char b[320];
            std::snprintf(b, sizeof b,
                          "%st=%3.0fs clientes=%d | enviadas=%ld respondidas=%ld %s=%.0f/s en_curso=%ld | "
                          "consulta=%ld reserva=%ld modif=%ld | rechazadas=%ld fallas=%ld",
                          ultima ? "FIN " : "", ahora - t0_, clientesAqui, env, res,
                          ultima ? "media" : "ahora", ritmo, env - hechas, vivo_.porTipo[0].load(),
                          vivo_.porTipo[1].load(), vivo_.porTipo[2].load(), rech, to + vivo_.errores.load());
            logLinea("EN VIVO", b);
            hechasAntes = hechas;
            anterior = ahora;
            proximo += paso;
            if (proximo < ahora) proximo = ahora + paso;
        }
    }

    // Perfil de un cliente: qué operación pide a continuación.
    TipoOperacion elegirTipo(bool soloConsulta, std::mt19937& rng, bool hayReservas) {
        if (soloConsulta) return TipoOperacion::CONSULTA;
        int dado = static_cast<int>(rng() % 100);
        TipoOperacion t;
        if (dado < p_.pctConsulta) t = TipoOperacion::CONSULTA;
        else if (dado < p_.pctConsulta + p_.pctReserva) t = TipoOperacion::RESERVA;
        else if (dado < p_.pctConsulta + p_.pctReserva + p_.pctModificar) t = TipoOperacion::MODIFICACION;
        else t = TipoOperacion::CANCELACION;
        if ((t == TipoOperacion::MODIFICACION || t == TipoOperacion::CANCELACION) && !hayReservas)
            t = TipoOperacion::RESERVA;  // no hay nada propio que modificar o cancelar
        return t;
    }

    Mensaje generarSolicitud(int idCliente, long idMensaje, TipoOperacion t, std::mt19937& rng,
                             std::vector<ReservaPropia>& misReservas,
                             const std::map<int, std::vector<std::pair<int, int>>>& calendario) {
        Mensaje m;
        m.idMensaje = idMensaje;
        m.idCliente = idCliente;
        m.tipo = t;
        m.endpointCliente = "sub:" + p_.notif;
        m.tsEnvio = relojPared();
        if (t == TipoOperacion::CONSULTA) {
            m.mes = p_.mes;
        } else if (t == TipoOperacion::RESERVA) {
            m.idEvento = 1 + static_cast<int>(rng() % p_.eventos);
            m.idOcurrencia = (m.idEvento <= p_.dobles) ? 1 + static_cast<int>(rng() % 2) : 1;
            m.numAsientos = 1 + static_cast<int>(rng() % p_.maxAsientos);
        } else if (t == TipoOperacion::MODIFICACION) {
            const ReservaPropia& r = misReservas[rng() % misReservas.size()];
            m.idReserva = r.idReserva;
            m.idEvento = r.idEvento;
            m.idOcurrencia = r.idOcurrencia;
            m.numAsientos = r.numAsientos;
            // Cambio de fecha solo si el evento tiene otra ocurrencia en el mes.
            int fecha = 0;
            auto it = calendario.find(r.idEvento);
            if (it != calendario.end() && rng() % 2 == 0)
                for (const auto& oc : it->second)
                    if (oc.first != r.idOcurrencia) fecha = oc.second;
            if (fecha > 0) m.fechaNueva = fecha;
            else m.numAsientosNuevo = 1 + static_cast<int>(rng() % p_.maxAsientos);
        } else {  // CANCELACION de una reserva propia
            size_t i = rng() % misReservas.size();
            m.idReserva = misReservas[i].idReserva;
            misReservas.erase(misReservas.begin() + i);
        }
        return m;
    }

    void cicloCliente(int idx) {
        const int idCliente = idx + 1;  // único en todo el sistema, aunque haya varias VMs
        const bool soloConsulta = idx < p_.soloConsulta;
        Cliente cliente(ctx_, idCliente, p_.gts, p_.notif, p_.timeoutMs, p_.reintentos);
        std::mt19937 rng(static_cast<unsigned>(p_.semilla) * 7919u + idCliente);
        std::vector<ReservaPropia> misReservas;
        std::map<int, std::vector<std::pair<int, int>>> calendario;  // evento -> (ocurrencia, fecha)
        std::this_thread::sleep_for(std::chrono::milliseconds(300));  // que el SUB se conecte

        long seq = 0;
        auto actualizarCalendario = [&](const Mensaje& r) {
            if (r.eventos.empty()) return;
            calendario.clear();
            for (const Evento& e : r.eventos) calendario[e.idEvento].push_back({e.idOcurrencia, e.fecha});
        };
        {   // consulta inicial (no se mide): para saber qué fechas tiene cada evento
            Mensaje m = generarSolicitud(idCliente, base_ + (++seq),
                                         TipoOperacion::CONSULTA, rng, misReservas, calendario), r;
            int intentos, cambios;
            vivo_.enviadas++;
            bool ok0 = cliente.enviarSolicitud(m, r, intentos, cambios);
            contarVivo(TipoOperacion::CONSULTA, ok0, r.resultado);
            if (ok0) actualizarCalendario(r);
        }

        const double intervalo = p_.rpm > 0 ? 60.0 / p_.rpm : 0.0;
        const double fin = t0_ + p_.duracion;
        while (ahoraSeg() < fin && !g_parar) {
            double inicioIter = ahoraSeg();
            TipoOperacion tipo = elegirTipo(soloConsulta, rng, !misReservas.empty());
            Mensaje m = generarSolicitud(idCliente, base_ + (++seq),
                                         tipo, rng, misReservas, calendario);
            Mensaje r;
            int intentos = 0, cambios = 0;
            double ts = ahoraSeg();
            vivo_.enviadas++;
            bool ok = cliente.enviarSolicitud(m, r, intentos, cambios);
            double ms = (ahoraSeg() - ts) * 1000.0;
            contarVivo(tipo, ok, r.resultado);
            double t = ts - t0_;
            bool medir = t >= p_.calentamiento;  // se descartan los primeros segundos

            if (tipo == TipoOperacion::RESERVA) metricas.contarReservaTotal(ok && r.resultado == Estado::EXITO, !ok);
            if (tipo == TipoOperacion::CANCELACION) {
                if (ok && medir) metricas.registrarCancelacion();
            } else if (medir) {
                if (!ok) metricas.registrarTimeout(tipo, t, intentos, cambios);
                else metricas.registrar(tipo, ms, t, r.resultado, intentos, cambios);
            }
            if (ok && tipo == TipoOperacion::CONSULTA) actualizarCalendario(r);
            if (ok && tipo == TipoOperacion::RESERVA && r.resultado == Estado::EXITO)
                misReservas.push_back({r.idReserva, r.idEvento, r.idOcurrencia, r.numAsientos});
            if (ok && tipo == TipoOperacion::MODIFICACION && r.resultado == Estado::EXITO)
                for (auto& rp : misReservas)
                    if (rp.idReserva == r.idReserva) {
                        rp.idOcurrencia = r.idOcurrencia;
                        rp.numAsientos = r.numAsientos;
                    }

            Mensaje nota;
            while (cliente.recibirNotificacion(nota, 0)) metricas.registrarNotificacion();
            // el GT rechazó por sobrecarga: un cliente educado espera un poco antes de volver a pedir
            if (ok && r.resultado == Estado::SOBRECARGA && p_.backoffMs > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(p_.backoffMs));

            double restante = intervalo - (ahoraSeg() - inicioIter);
            if (restante > 0) std::this_thread::sleep_for(std::chrono::duration<double>(restante));
        }
        // Espera breve por notificaciones que aún estén en camino.
        Mensaje nota;
        double limite = ahoraSeg() + 2.0;
        while (ahoraSeg() < limite) {
            if (cliente.recibirNotificacion(nota, 200)) metricas.registrarNotificacion();
            else if (p_.pctCancelar == 0) break;
        }
    }

    void informar(int clientesAqui) {
        const double ventana = std::max(1.0, static_cast<double>(p_.duracion - p_.calentamiento));
        const TipoOperacion C = TipoOperacion::CONSULTA, R = TipoOperacion::RESERVA,
                            M = TipoOperacion::MODIFICACION;
        char b[320];
        std::snprintf(b, sizeof b, "CONSULTA: n=%zu prom=%.2f ms std=%.2f p95=%.2f TPM=%.1f timeouts=%ld",
                      metricas.n(C), metricas.promedio(C), metricas.desviacionEstandar(C),
                      metricas.percentil95(C), metricas.tpm(C, ventana), metricas.getTimeouts(C));
        logLinea("CARGA", b);
        std::snprintf(b, sizeof b,
                      "RESERVA:  n=%zu prom=%.2f ms std=%.2f p95=%.2f TPM=%.1f rechazos=%ld timeouts=%ld",
                      metricas.n(R), metricas.promedio(R), metricas.desviacionEstandar(R),
                      metricas.percentil95(R), metricas.tpm(R, ventana),
                      metricas.getResultado(R, Estado::RECHAZO), metricas.getTimeouts(R));
        logLinea("CARGA", b);
        if (p_.pctModificar > 0) {
            std::snprintf(b, sizeof b,
                          "MODIFIC.: n=%zu prom=%.2f ms std=%.2f p95=%.2f TPM=%.1f rechazos=%ld timeouts=%ld",
                          metricas.n(M), metricas.promedio(M), metricas.desviacionEstandar(M),
                          metricas.percentil95(M), metricas.tpm(M, ventana),
                          metricas.getResultado(M, Estado::RECHAZO), metricas.getTimeouts(M));
            logLinea("CARGA", b);
        }
        long errores = metricas.getResultado(C, Estado::ERROR) + metricas.getResultado(R, Estado::ERROR) +
                       metricas.getResultado(M, Estado::ERROR);
        logLinea("CARGA", "errores=" + std::to_string(errores) + " reintentos=" +
                              std::to_string(metricas.getReintentos()) + " cambios_de_GT=" +
                              std::to_string(metricas.getFailovers()) + " reservas_confirmadas=" +
                              std::to_string(metricas.getReservasExitoTotal()) + " reservas_sin_respuesta=" +
                              std::to_string(metricas.getReservasTimeoutTotal()) + " sobrecarga=" +
                              std::to_string(metricas.getSobrecargas()));
        if (p_.pctCancelar > 0)
            logLinea("CARGA", "cancelaciones enviadas=" + std::to_string(metricas.getCancelaciones()) +
                                  " notificaciones recibidas=" +
                                  std::to_string(metricas.getNotificaciones()));

        if (!p_.muestras.empty()) {
            bool existe = std::ifstream(p_.muestras).good();
            std::ofstream f(p_.muestras, std::ios::app);
            if (!existe)
                f << "arquitectura,repeticion,clientes,vm_indice,tipo,ms,t_s,resultado,intentos,ventana_s\n";
            for (const Muestra& s : metricas.getMuestras()) {
                f << p_.arquitectura << ',' << p_.repeticion << ',' << p_.clientes << ',' << p_.vmIndice
                  << ',' << nombre(static_cast<TipoOperacion>(s.tipo)) << ',' << s.ms << ',' << s.t << ','
                  << (s.resultado < 0 ? "TIMEOUT" : nombre(static_cast<Estado>(s.resultado))) << ','
                  << s.intentos << ',' << ventana << '\n';
            }
            logLinea("CARGA", "muestras agregadas a " + p_.muestras);
        }
        if (p_.csv.empty()) return;
        bool existe = std::ifstream(p_.csv).good();
        std::ofstream f(p_.csv, std::ios::app);
        if (!existe)
            f << "arquitectura,repeticion,clientes,solo_consulta,vm_indice,vm_total,clientes_en_vm,"
                 "duracion_s,calentamiento_s,consultas_n,consulta_prom_ms,consulta_std_ms,"
                 "consulta_p95_ms,consulta_tpm,consulta_timeouts,reservas_n,reserva_prom_ms,"
                 "reserva_std_ms,reserva_p95_ms,reserva_tpm,reserva_rechazos,reserva_timeouts,"
                 "modif_n,modif_prom_ms,errores,reintentos,cambios_de_gt,reservas_confirmadas,"
                 "reservas_sin_respuesta,cancelaciones,notificaciones\n";
        char fila[900];
        std::snprintf(fila, sizeof fila,
                      "%s,%d,%d,%d,%d,%d,%d,%d,%d,%zu,%.3f,%.3f,%.3f,%.2f,%ld,%zu,%.3f,%.3f,%.3f,%.2f,"
                      "%ld,%ld,%zu,%.3f,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                      p_.arquitectura.c_str(), p_.repeticion, p_.clientes, p_.soloConsulta, p_.vmIndice,
                      p_.vmTotal, clientesAqui, p_.duracion, p_.calentamiento, metricas.n(C),
                      metricas.promedio(C), metricas.desviacionEstandar(C), metricas.percentil95(C),
                      metricas.tpm(C, ventana), metricas.getTimeouts(C), metricas.n(R), metricas.promedio(R),
                      metricas.desviacionEstandar(R), metricas.percentil95(R), metricas.tpm(R, ventana),
                      metricas.getResultado(R, Estado::RECHAZO), metricas.getTimeouts(R), metricas.n(M),
                      metricas.promedio(M), errores, metricas.getReintentos(), metricas.getFailovers(),
                      metricas.getReservasExitoTotal(), metricas.getReservasTimeoutTotal(),
                      metricas.getCancelaciones(), metricas.getNotificaciones());
        f << fila;
        logLinea("CARGA", "fila agregada a " + p_.csv);
    }

    Parametros p_;
    zmq::context_t ctx_;
    Metricas metricas;
    Vivo vivo_;
    double t0_ = 0.0;
    long base_ = 0;
};

int main(int argc, char** argv) {
    instalarSenales();
    Parametros p;
    p.gts = dividir(argStr(argc, argv, "--gt", "tcp://localhost:5555"), ',');
    p.notif = argStr(argc, argv, "--notif", "");
    p.arquitectura = argStr(argc, argv, "--arquitectura", "base");
    p.csv = argStr(argc, argv, "--csv", "");
    p.muestras = argStr(argc, argv, "--muestras", "");
    p.clientes = argInt(argc, argv, "--clientes", 3);
    p.soloConsulta = argInt(argc, argv, "--solo-consulta", 2);
    p.rpm = argInt(argc, argv, "--rpm", 60);
    p.duracion = argInt(argc, argv, "--duracion", 120);
    p.calentamiento = argInt(argc, argv, "--calentamiento", 5);
    p.timeoutMs = argInt(argc, argv, "--timeout-ms", 4000);
    p.reintentos = argInt(argc, argv, "--reintentos", 3);
    p.semilla = argInt(argc, argv, "--semilla", 1);
    p.pctConsulta = argInt(argc, argv, "--pct-consulta", 40);
    p.pctReserva = argInt(argc, argv, "--pct-reserva", 40);
    p.pctModificar = argInt(argc, argv, "--pct-modificar", 20);
    p.pctCancelar = argInt(argc, argv, "--pct-cancelar", 0);
    p.backoffMs = argInt(argc, argv, "--backoff-ms", p.backoffMs);
    p.enVivo = std::max(0, argInt(argc, argv, "--en-vivo", 0));
    p.mes = argInt(argc, argv, "--mes", 202611);
    p.eventos = std::max(1, argInt(argc, argv, "--eventos", 50));
    p.dobles = argInt(argc, argv, "--dobles", 20);
    p.maxAsientos = std::max(1, argInt(argc, argv, "--max-asientos", 4));
    p.vmIndice = argInt(argc, argv, "--vm-indice", 0);
    p.vmTotal = std::max(1, argInt(argc, argv, "--vm-total", 1));
    p.repeticion = argInt(argc, argv, "--repeticion", 1);
    if (p.pctConsulta + p.pctReserva + p.pctModificar + p.pctCancelar != 100) {
        std::fprintf(stderr, "Los porcentajes (consulta+reserva+modificar+cancelar) deben sumar 100\n");
        return 1;
    }
    if (p.clientes < 1 || p.soloConsulta > p.clientes || p.vmIndice < 0 || p.vmIndice >= p.vmTotal) {
        std::fprintf(stderr, "Revise --clientes, --solo-consulta, --vm-indice y --vm-total\n");
        return 1;
    }
    GeneradorCarga g(p);
    g.iniciar();
    return 0;
}
