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

.PHONY: test engine bridge boot-matrix stock-fw clean
test: engine bridge boot-matrix

engine: engine_test
	./engine_test

# If `make stock-fw` has been run, hand the bridge test the fetched images
# automatically so its QEMU layer stops silently skipping. An explicit
# SHELLY_FW_DIR in the environment still wins.
STOCK_2_0_0 := $(wildcard stock_fw/2.0.0)
ifneq ($(STOCK_2_0_0),)
export SHELLY_FW_DIR ?= $(abspath stock_fw/2.0.0)
endif

bridge:
	$(PYTHON) bridge_package_test.py

# SH0S boot-record semantics against the real Shelly bootloader under QEMU.
# Skips cleanly without QEMU or the stock images; `make stock-fw` fetches those.
boot-matrix:
	$(PYTHON) boot_matrix_test.py

# One-time: retrieve the stock firmware packages the QEMU layers need. Nothing
# downloaded is committed; see fetch_stock_fw.py for the integrity model.
stock-fw:
	$(PYTHON) fetch_stock_fw.py

engine_test: engine_test.cpp $(ENGINE_HDRS)
	$(CXX) $(CXXFLAGS) engine_test.cpp -o engine_test

clean:
	rm -f engine_test
