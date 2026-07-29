#---------------------------------------------------------------------------------
# Host tests and compile sweep. See README.md in this directory.
#
# This builds with the *host* compiler, not devkitPPC. It produces nothing that
# runs on a Wii U — it exists so that a typo or a changed Wolfram signature is
# caught in a second rather than after a card swap and a boot (AGENTS.md §10).
#---------------------------------------------------------------------------------
CC      ?= cc
ROOT    := ..
SRC     := $(ROOT)/src
OUT     := build
SCRATCH := $(OUT)/scratch

PKGCONFIG := $(shell command -v pkg-config 2>/dev/null)

ifeq ($(PKGCONFIG),)
   SDL_CFLAGS   := -I/usr/include/SDL2
   SDL_LIBS     := -lSDL2 -lSDL2_ttf
   CJSON_CFLAGS := -I/usr/include/cjson
   CURL_CFLAGS  :=
   CURL_LIBS    := -lcurl
else
   SDL_CFLAGS   := $(shell pkg-config --cflags sdl2 SDL2_ttf)
   SDL_LIBS     := $(shell pkg-config --libs sdl2 SDL2_ttf)
   CJSON_CFLAGS := $(shell pkg-config --cflags libcjson 2>/dev/null || echo -I/usr/include/cjson)
   CURL_CFLAGS  := $(shell pkg-config --cflags libcurl 2>/dev/null)
   CURL_LIBS    := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
endif

# Matches the main Makefile's convention: Wolfram is a sibling checkout and is
# optional. Present, it lets the sweep check Cobalt's calls against the real SDK
# signatures instead of only the no-Wolfram configuration.
WOLFRAM_ROOT ?= $(ROOT)/../wolfram
HAVE_WOLFRAM := $(wildcard $(WOLFRAM_ROOT)/include/wolfram/session.h)

CFLAGS  := -std=gnu11 -g -O1 -Wall -Wextra -I$(SRC) $(SDL_CFLAGS) $(CURL_CFLAGS) \
           -include $(CURDIR)/host_sdl_compat.h
LDLIBS  := $(SDL_LIBS) $(CURL_LIBS) -lmbedcrypto -lm -lpthread

#---------------------------------------------------------------------------------
# Unit tests.
#
# Only modules that are platform-independent by construction are linked; the two
# that are not (util/paths.c, util/log.c) are replaced by host_stubs.c.
#---------------------------------------------------------------------------------
UNIT_SOURCES := \
	test_main.c \
	host_stubs.c \
	$(SRC)/cache/session_store.c \
	$(SRC)/atproto/session.c \
	$(SRC)/app/signin.c \
	$(SRC)/ui/keyboard.c \
	$(SRC)/util/rng.c \
	$(SRC)/ui/render.c \
	$(SRC)/ui/theme.c \
	$(SRC)/input/input.c

TEST_BIN := $(OUT)/cobalt-tests

#---------------------------------------------------------------------------------
# Compile sweep.
#
# Everything except the translation units that need devkitPro headers:
#   main.c        - <whb/proc.h> by way of the SDL Wii U port's expectations
#   net/net.c     - <nn/ac.h>
#   util/log.c    - <coreinit/debug.h>, Aroma's logging module
#   util/paths.c  - <whb/sdcard.h>
# Those are covered by the hardware pass, which is the only place they can be.
#---------------------------------------------------------------------------------
SWEEP_SOURCES := $(filter-out \
	$(SRC)/main.c $(SRC)/net/net.c $(SRC)/util/log.c $(SRC)/util/paths.c, \
	$(wildcard $(SRC)/*.c $(SRC)/*/*.c))

.PHONY: all check sweep clean

all: sweep check

#---------------------------------------------------------------------------------
sweep:
	@echo "sweep ... $(words $(SWEEP_SOURCES)) files (no Wolfram)"
	@for f in $(SWEEP_SOURCES); do \
		$(CC) -fsyntax-only $(CFLAGS) -Werror $$f || exit 1; \
	done
ifeq ($(HAVE_WOLFRAM),)
	@echo "sweep ... skipping the Wolfram configuration ($(WOLFRAM_ROOT) not found)"
else
	@echo "sweep ... same files against $(WOLFRAM_ROOT)"
	@for f in $(SWEEP_SOURCES); do \
		$(CC) -fsyntax-only $(CFLAGS) -Werror -DCOBALT_HAS_WOLFRAM=1 \
			-I$(WOLFRAM_ROOT)/include $(CJSON_CFLAGS) $$f || exit 1; \
	done
endif
	@echo "sweep ... ok"

#---------------------------------------------------------------------------------
check: $(TEST_BIN)
	@mkdir -p $(SCRATCH)
	@./$(TEST_BIN) $(SCRATCH)

$(TEST_BIN): $(UNIT_SOURCES)
	@mkdir -p $(OUT)
	@$(CC) $(CFLAGS) -o $@ $(UNIT_SOURCES) $(LDLIBS)

#---------------------------------------------------------------------------------
clean:
	@rm -rf $(OUT)
