# Makefile del SRB (arquitectura escalable y tolerante a fallas)
# Requiere: g++ (C++17), libzmq3-dev, cppzmq (zmq.hpp), libpqxx-dev, libpq-dev.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -Isrc
BIN       = bin

all: servicios pruebas

servicios: $(BIN)/persistencia $(BIN)/rmc $(BIN)/cancelaciones $(BIN)/gestor $(BIN)/generador $(BIN)/cliente

pruebas: $(BIN)/test_mensaje $(BIN)/prueba_funcional

$(BIN):
	mkdir -p $(BIN)

$(BIN)/persistencia: src/servicio_persistencia.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq -lpqxx -lpq

$(BIN)/rmc: src/servicio_rmc.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/cancelaciones: src/servicio_cancelaciones.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/gestor: src/gestor_transacciones.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/generador: src/generador_carga.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/cliente: src/cliente_archivo.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/test_mensaje: tests/test_mensaje.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

$(BIN)/prueba_funcional: tests/prueba_funcional.cpp src/common.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ -lzmq

clean:
	rm -rf $(BIN) logs resultados

.PHONY: all servicios pruebas clean
