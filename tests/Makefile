# Standalone dev tests for the pure-C++ dimmer engine. No ESPHome / ESP-IDF.
#
#   make          # build + run the tests
#   make clean
#
# Requires only a C++17 compiler. Override with e.g. `make CXX=clang++`.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -I..

ENGINE_HDRS = ../components/shelly_wall_dimmer/dimmer_engine.h \
              ../components/shelly_wall_dimmer/dimmer_protocol.h

.PHONY: test clean
test: engine_test
	./engine_test

engine_test: engine_test.cpp $(ENGINE_HDRS)
	$(CXX) $(CXXFLAGS) engine_test.cpp -o engine_test

clean:
	rm -f engine_test
