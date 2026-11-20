// test_mensaje.cpp: pruebas unitarias de la serialización de mensajes y del log de operaciones.
// Compilar y correr: make bin/test_mensaje && bin/test_mensaje
#include <cstdio>

#include "common.hpp"

using namespace srb;

static int fallos = 0, total = 0;

static void verificar(bool ok, const char* nombreCaso) {
    ++total;
    if (!ok) ++fallos;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", nombreCaso);
}

int main() {
    // ida y vuelta de un mensaje completo
    Mensaje m;
    m.idMensaje = 9999999999123L;  // 64 bits
    m.tipo = TipoOperacion::MODIFICACION;
    m.idCliente = 7;
    m.endpointCliente = "sub:tcp://10.0.0.1:5558";
    m.idEvento = 12;
    m.idOcurrencia = 2;
    m.mes = 202611;
    m.numAsientos = 3;
    m.idReserva = 55;
    m.fechaNueva = 20261116;
    m.numAsientosNuevo = 0;
    m.tsEnvio = 1234.5;
    m.resultado = Estado::RECHAZO;
    m.eventos = {{1, 1, 2, 20261102, 95, 5}, {1, 2, 2, 20261116, 100, 0}};
    Mensaje r;
    bool ok = deserializar(serializar(m), r);
    verificar(ok, "deserializar acepta un mensaje serializado");
    verificar(r.idMensaje == m.idMensaje && r.tipo == m.tipo && r.idCliente == 7 && r.idEvento == 12 &&
                  r.idOcurrencia == 2 && r.mes == 202611 && r.numAsientos == 3 && r.idReserva == 55 &&
                  r.fechaNueva == 20261116 && r.resultado == Estado::RECHAZO,
              "ida y vuelta conserva todos los campos (idMensaje de 64 bits incluido)");
    verificar(r.eventos.size() == 2 && r.eventos[0].asientosLibres == 95 &&
                  r.eventos[1].fecha == 20261116,
              "ida y vuelta conserva la lista de eventos");
    verificar(r.endpointCliente == "sub:tcp://10.0.0.1:5558", "ida y vuelta conserva el endpoint");

    // un mensaje sin eventos
    Mensaje v;
    Mensaje rv;
    verificar(deserializar(serializar(v), rv) && rv.eventos.empty(), "mensaje por defecto sin eventos");

    // los separadores dentro del endpoint no rompen el formato
    Mensaje s;
    s.endpointCliente = "a|b;c,d\ne";
    Mensaje rs;
    verificar(deserializar(serializar(s), rs) && rs.endpointCliente == "a_b_c_d_e",
              "los separadores del endpoint se sustituyen");

    // mensajes mal formados: se rechazan sin lanzar excepciones
    Mensaje x;
    verificar(!deserializar("", x), "rechaza texto vacío");
    verificar(!deserializar("basura sin formato", x), "rechaza texto sin separadores");
    verificar(!deserializar("1|0|1||0|0|0|0|0|0|0|0.0|0", x), "rechaza 13 campos");
    verificar(!deserializar("1|0|1||0|0|0|0|0|0|0|0.0|0||", x), "rechaza 15 campos");
    verificar(!deserializar("x|0|1||0|0|0|0|0|0|0|0.0|0|", x), "rechaza un campo numérico no numérico");
    verificar(!deserializar("1|9|1||0|0|0|0|0|0|0|0.0|0|", x), "rechaza un tipo de operación inexistente");
    verificar(!deserializar("1|0|1||0|0|0|0|0|0|0|0.0|7|", x), "rechaza un resultado inexistente");
    verificar(!deserializar("1|0|1||0|0|0|0|0|0|0|0.0|0|1,2,3", x), "rechaza un evento con campos de menos");
    verificar(!deserializar("99999999999999999999999|0|1||0|0|0|0|0|0|0|0.0|0|", x),
              "rechaza un número fuera de rango");
    verificar(deserializar("1|0|1||0|0|202611|0|0|0|0|0.0|0|", x) && x.mes == 202611,
              "acepta un mensaje válido escrito a mano");

    // descripciones para el log
    Mensaje d;
    d.tipo = TipoOperacion::RESERVA;
    d.idCliente = 3;
    d.idMensaje = 4;
    d.idEvento = 5;
    d.idOcurrencia = 1;
    d.numAsientos = 2;
    verificar(describir(d) == "RESERVA cli=3 msg=4 E5/occ1 asientos=2", "describir una reserva");
    d.tipo = TipoOperacion::MODIFICACION;
    d.idReserva = 9;
    d.fechaNueva = 20261116;
    verificar(describir(d) == "MODIFICACION cli=3 msg=4 reserva=9 fecha->20261116", "describir una modificación");

    std::printf("\n%d pruebas, %d fallos\n", total, fallos);
    return fallos == 0 ? 0 : 1;
}
