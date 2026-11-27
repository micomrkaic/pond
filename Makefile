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

SRC  := src/main.c src/param.c src/config.c src/script.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c src/record.c src/attract.c
HDR  := src/app.h src/param.h src/config.h src/script.h src/wave.h src/dct.h src/hos.h src/render.h src/view3d.h src/text.h src/gl.h src/font8x8.h src/audio.h src/dsp.h src/record.h src/attract.h src/version.h
BIN  := pond
# where the tests are built; `make sanitize` uses build/asan, so it leaves these alone
B    := build
VERSION := $(shell sed -n 's/^\#define POND_VERSION "\(.*\)"/\1/p' src/version.h)

EMCC       ?= emcc
# Fixed heap, no growth: growable memory gives the runtime a resizable
# ArrayBuffer, which current Chrome refuses in TextDecoder (and elsewhere).
# 160 MB covers a 512^2 rectangle or a 512 x 256 disk basis; the browser build clamps the grid there.
EMFLAGS    ?= -std=c17 -O3 -msimd128 -sUSE_SDL=2 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
              -sALLOW_MEMORY_GROWTH=0 -sINITIAL_MEMORY=160MB -sMINIFY_HTML=0 \
              -sEXPORTED_FUNCTIONS=_main,_pond_key -sEXPORTED_RUNTIME_METHODS=ccall \
              --preload-file demos
WEB_DIR    := build/web

.PHONY: all web test bench clean sanitize fuzz dist bindist

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SRC) $(SDL_LIBS) $(LDLIBS)

test: $(B)/test_dct $(B)/test_wave $(B)/test_disk $(B)/test_hos $(B)/test_paddle $(B)/test_break $(B)/test_audio $(B)/test_script $(BIN)
	./$(B)/test_dct
	./$(B)/test_wave
	./$(B)/test_disk
	./$(B)/test_hos
	./$(B)/test_paddle
	./$(B)/test_break
	./$(B)/test_audio
	./$(B)/test_script
	sh tests/test_man.sh ./$(BIN) pond.1

$(B)/test_script: tests/test_script.c src/config.c src/param.c src/script.c src/record.c src/attract.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c src/app.h src/param.h src/config.h src/script.h src/record.h src/attract.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ tests/test_script.c src/config.c src/param.c src/script.c src/record.c src/attract.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c $(SDL_LIBS) $(LDLIBS)

$(B)/test_audio: tests/test_audio.c src/audio.c src/dsp.c src/audio.h src/dsp.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ tests/test_audio.c src/audio.c src/dsp.c $(SDL_LIBS) $(LDLIBS)

$(B)/test_break: tests/test_break.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_break.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

$(B)/test_paddle: tests/test_paddle.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_paddle.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

$(B)/test_hos: tests/test_hos.c src/hos.c src/wave.c src/disk.c src/dct.c src/hos.h src/wave.h src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_hos.c src/hos.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

$(B)/test_disk: tests/test_disk.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_disk.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

$(B)/test_dct: tests/test_dct.c src/dct.c src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_dct.c src/dct.c $(LDLIBS)

$(B)/test_wave: tests/test_wave.c src/wave.c src/disk.c src/dct.c src/wave.h src/dct.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ tests/test_wave.c src/wave.c src/disk.c src/dct.c $(LDLIBS)

# the tests again, built with AddressSanitizer and UndefinedBehaviorSanitizer
# (leaks included on Linux), into build/asan; any finding stops the run.  CI runs this.
SAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
sanitize:
	UBSAN_OPTIONS=print_stacktrace=1 $(MAKE) test B=build/asan BIN=build/asan/pond \
	  CFLAGS="-std=c17 -O1 -g -Wall -Wextra -Wpedantic $(SAN_FLAGS)" LDLIBS="-lm $(SAN_FLAGS)"

# the script and config parsers under libFuzzer (needs clang): shows are files people
# swap, so a malformed one should be an error message, never a crash.  The demos seed
# the corpus; new inputs that reach new code collect in build/fuzz-corpus.
FUZZCC    ?= clang
FUZZ_TIME ?= 60
fuzz: $(B)/fuzz_parse
	@mkdir -p $(B)/fuzz-corpus
	./$(B)/fuzz_parse -max_total_time=$(FUZZ_TIME) -max_len=8192 -timeout=10 $(B)/fuzz-corpus demos

$(B)/fuzz_parse: tests/fuzz_parse.c src/config.c src/param.c src/script.c src/record.c src/attract.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c $(HDR)
	@mkdir -p $(B)
	$(FUZZCC) -std=c17 -O1 -g -fsanitize=fuzzer,address,undefined $(SDL_CFLAGS) -o $@ tests/fuzz_parse.c src/config.c src/param.c src/script.c src/record.c src/attract.c src/wave.c src/disk.c src/hos.c src/dct.c src/render.c src/view3d.c src/text.c src/audio.c src/dsp.c $(SDL_LIBS) -lm

# a source tarball of what is committed, and a binary one of this machine's build
# (the program, its manual, the demos), named for the version and the platform;
# the release workflow attaches both to a GitHub release when a tag v$(VERSION) is pushed
PLATFORM := $(shell uname -s | tr A-Z a-z)-$(shell uname -m)
dist:
	@mkdir -p build
	git archive --format=tar.gz --prefix=pond-$(VERSION)/ -o build/pond-$(VERSION).tar.gz HEAD
	@echo build/pond-$(VERSION).tar.gz

bindist: $(BIN)
	rm -rf build/pond-$(VERSION)-$(PLATFORM)
	mkdir -p build/pond-$(VERSION)-$(PLATFORM)/demos
	cp $(BIN) pond.1 README.md LICENSE build/pond-$(VERSION)-$(PLATFORM)/
	cp demos/*.pond build/pond-$(VERSION)-$(PLATFORM)/demos/
	tar -C build -czf build/pond-$(VERSION)-$(PLATFORM).tar.gz pond-$(VERSION)-$(PLATFORM)
	@echo build/pond-$(VERSION)-$(PLATFORM).tar.gz

# headless timing; needs no display
bench: $(BIN)
	./$(BIN) --bench 300 --preset 3

# browser build: needs emsdk on PATH; serve the output directory with any static
# server (python3 -m http.server).  For GitHub Pages: make web WEB_DIR=docs,
# commit docs/, and point Pages at /docs.
# A distribution's Emscripten (Debian's, under /usr/share) keeps a frozen cache where a
# user cannot write, and SDL2 has to be fetched and built into it on first use.  Then
# the cache is copied once to ~/.cache/emscripten and used from there, unfrozen
# (unless EM_CACHE is set already).
web: $(SRC) $(HDR) web/shell.html
	@mkdir -p $(WEB_DIR)
	@c=$$(em-config CACHE 2>/dev/null); f=$$(em-config FROZEN_CACHE 2>/dev/null); \
	if [ -z "$$EM_CACHE" ] && [ -n "$$c" ] && { [ ! -w "$$c" ] || [ "$$f" = True ]; }; then \
	  EM_CACHE="$$HOME/.cache/emscripten"; EM_FROZEN_CACHE=; export EM_CACHE EM_FROZEN_CACHE; \
	  if [ ! -d "$$EM_CACHE" ]; then mkdir -p "$$EM_CACHE" && cp -R "$$c"/. "$$EM_CACHE"/ && rm -f "$$EM_CACHE"/cache.lock; fi; \
	  echo "Emscripten's cache $$c is read-only (a distribution's): using $$EM_CACHE"; fi; \
	echo "$(EMCC) $(EMFLAGS) --shell-file web/shell.html -o $(WEB_DIR)/index.html $(SRC)"; \
	$(EMCC) $(EMFLAGS) --shell-file web/shell.html -o $(WEB_DIR)/index.html $(SRC)

clean:
	rm -rf $(BIN) build pond-*.bmp

# Install / uninstall, portable to GNU make 3.81 (macOS) and BSD/GNU install(1).
#
#   make install                         -> ~/.local/bin/pond
#   make install PREFIX=/usr/local       -> system-wide (with sudo)
#
# Command-line PREFIX overrides the ?= default. DESTDIR is for packagers.

PROG    ?= pond
PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1
DATADIR ?= $(PREFIX)/share/pond
INSTALL ?= install

.PHONY: install uninstall

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(PROG) $(DESTDIR)$(BINDIR)/
	@if [ -f $(PROG).1 ]; then \
		$(INSTALL) -d $(DESTDIR)$(MANDIR) && \
		$(INSTALL) -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)/; \
	fi
	# the demos, where --attract looks for them: next to bin/, in share/pond/demos
	$(INSTALL) -d $(DESTDIR)$(DATADIR)/demos
	$(INSTALL) -m 644 demos/*.pond $(DESTDIR)$(DATADIR)/demos/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(MANDIR)/$(PROG).1
	rm -rf $(DESTDIR)$(DATADIR)
