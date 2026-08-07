# Standalone dev tests for the pure-C++ dimmer engine. No ESPHome / ESP-IDF.
#
#   make          # build + run every test
#   make engine   # dimmer engine only (C++17 compiler)
#   make bridge   # bridge-package safety only (python3)
#   make clean
#
# Requires only a C++17 compiler and python3. Override with e.g. `make CXX=clang++`.
#
# The bridge test's QEMU layer is opt-in and needs your own (non-redistributable)
# stock firmware files; it skips cleanly without them:
#   SHELLY_FW_DIR=/path/to/stock SHELLY_APP_BIN=/path/to/firmware.bin make bridge

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O0 -I..
PYTHON   ?= python3

ENGINE_HDRS = ../components/shelly_wall_dimmer/dimmer_engine.h \
              ../components/shelly_wall_dimmer/dimmer_protocol.h

.PHONY: test engine bridge clean
test: engine bridge

engine: engine_test
	./engine_test

bridge:
	$(PYTHON) bridge_package_test.py

engine_test: engine_test.cpp $(ENGINE_HDRS)
	$(CXX) $(CXXFLAGS) engine_test.cpp -o engine_test

clean:
	rm -f engine_test
