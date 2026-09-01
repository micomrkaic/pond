# pond — a numerically honest wave tank as screen candy
# Copyright (C) 2026 Mico <https://github.com/micomrkaic>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# pond — build for Linux / macOS (SDL2) and the browser (Emscripten)

CC      ?= cc
CFLAGS  ?= -std=c17 -O3 -Wall -Wextra -Wpedantic
# CFLAGS += -march=native      # a few percent on the shading; not for shipping binaries
LDLIBS  ?= -lm

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

SRC  := src/main.c src/config.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c
HDR  := src/config.h src/wave.h src/dct.h src/hos.h src/render.h src/view3d.h src/text.h src/gl.h src/font8x8.h src/audio.h src/dsp.h
BIN  := pond

EMCC       ?= emcc
# Fixed heap, no growth: growable memory gives the runtime a resizable
# ArrayBuffer, which current Chrome refuses in TextDecoder (and elsewhere).
# 160 MB covers a 512^2 rectangle or a 512 x 256 disk basis; the browser build clamps the grid there.
EMFLAGS    ?= -std=c17 -O3 -msimd128 -sUSE_SDL=2 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
              -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=160MB -sMINIFY_HTML=0
WEB_DIR    := build/web

.PHONY: all web test bench clean

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SRC) $(SDL_LIBS) $(LDLIBS)

test: build/test_dct build/test_wave build/test_disk build/test_hos build/test_paddle
	./build/test_dct
	./build/test_wave
	./build/test_disk
	./build/test_hos
	./build/test_paddle

build/test_paddle: tests/test_paddle.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_paddle.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

build/test_hos: tests/test_hos.c src/hos.c src/wave.c src/disk.c src/dct.c src/hos.h src/wave.h src/dct.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_hos.c src/hos.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

build/test_disk: tests/test_disk.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_disk.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

build/test_dct: tests/test_dct.c src/dct.c src/dct.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_dct.c src/dct.c $(LDLIBS)

build/test_wave: tests/test_wave.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/test_wave.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

# headless timing; needs no display
bench: $(BIN)
	./$(BIN) --bench 300 --preset 3

# browser build: needs emsdk on PATH; serve the output directory with any static
# server (python3 -m http.server).  For GitHub Pages: make web WEB_DIR=docs,
# commit docs/, and point Pages at /docs.
web: $(SRC) $(HDR) web/shell.html
	@mkdir -p $(WEB_DIR)
	$(EMCC) $(EMFLAGS) --shell-file web/shell.html -o $(WEB_DIR)/index.html $(SRC)

clean:
	rm -rf $(BIN) build pond-*.bmp
