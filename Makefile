# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make core        compile the vendored emulation core on its own. No SDL, no
#                    frontend -- this is the code the libretro core drives.
#
#   make libretro    build the libretro core (ti99_libretro.so): the upstream
#                    objects plus src/libretro.cpp.
#
#   make bootcheck   build tools/bootcheck, which runs the core's own entry
#                    points for a few frames. Point TI99_SYS at a directory
#                    with 994aROM.bin and 994aGROM.bin to actually boot.
#
CORE_DIR := upstream
CORE     := ti99_libretro
CXX      ?= g++
CXXFLAGS ?= -O2 -fPIC -fno-strict-aliasing -fexceptions -w

# compat/ supplies a <endian.h> shim for macOS so upstream/include stays
# unmodified. Everywhere else the system header is used directly, exactly as
# before.
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Darwin)
INCLUDES := -Iinclude -Icompat -I$(CORE_DIR)/include
else
INCLUDES := -Iinclude -I$(CORE_DIR)/include
endif

CORE_CPP := $(wildcard $(CORE_DIR)/src/core/*.cpp)
CORE_OBJ := $(patsubst %.cpp,%.o,$(CORE_CPP))
OBJECTS  := $(CORE_OBJ) src/libretro.o

all: libretro

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

# Compiling the core on its own is the first thing CI checks; it is also what
# catches a patch to upstream/src/core that no longer builds.
core: $(CORE_OBJ)
	@echo "compiled $(words $(CORE_OBJ)) core objects"

$(CORE).so: $(OBJECTS)
	$(CXX) -shared -o $@ $(OBJECTS) -lm
	@echo "built $(CORE).so"

libretro: $(CORE).so

tools/bootcheck: tools/bootcheck.cpp src/libretro.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ tools/bootcheck.cpp src/libretro.o $(CORE_OBJ) -lm

bootcheck: tools/bootcheck

clean:
	rm -f $(CORE_OBJ) src/*.o tools/bootcheck $(CORE).so

.PHONY: all core libretro bootcheck clean
