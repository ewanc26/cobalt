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

# SDL2_image is how ui/imagecache.c decodes. Without it the module still builds
# and is still swept and tested — decoding compiles out and every load reports
# failure — so a checkout that lacks the package loses coverage of one call
# rather than of the threading and eviction logic around it.
HAVE_SDL_IMAGE := $(shell pkg-config --exists SDL2_image 2>/dev/null && echo yes)
ifeq ($(HAVE_SDL_IMAGE),yes)
   IMAGE_CFLAGS := $(shell pkg-config --cflags SDL2_image) -DCOBALT_FORCE_SDL_IMAGE=1
   IMAGE_LIBS   := $(shell pkg-config --libs SDL2_image)
else
   IMAGE_CFLAGS :=
   IMAGE_LIBS   :=
endif

# Matches the main Makefile's convention: Wolfram is a sibling checkout and is
# optional. Present, it lets the sweep check Cobalt's calls against the real SDK
# signatures instead of only the no-Wolfram configuration.
WOLFRAM_ROOT ?= $(ROOT)/../wolfram
HAVE_WOLFRAM := $(wildcard $(WOLFRAM_ROOT)/include/wolfram/session.h)

# A *host* build of Wolfram, if one exists, additionally enables the link check
# below. Build it with:
#   cmake -S ../wolfram -B ../wolfram/build-host -DWOLFRAM_BUILD_EXAMPLES=OFF
#   cmake --build ../wolfram/build-host -j8 --target wolfram
WOLFRAM_HOST_BUILD ?= $(WOLFRAM_ROOT)/build-host
# .dylib alongside .so/.a: CMake names the shared library after the host's
# convention, and a macOS checkout building Wolfram for this same link check
# gets a .dylib rather than a .so.
HAVE_WOLFRAM_LIB := $(firstword $(wildcard $(WOLFRAM_HOST_BUILD)/libwolfram.so \
                                           $(WOLFRAM_HOST_BUILD)/libwolfram.dylib \
                                           $(WOLFRAM_HOST_BUILD)/libwolfram.a))
# Wolfram pulls cJSON in through FetchContent, so the host build carries its own
# copy rather than using a system one.
WOLFRAM_CJSON := $(firstword $(wildcard \
   $(WOLFRAM_HOST_BUILD)/_deps/cjson-build/libcjson.so \
   $(WOLFRAM_HOST_BUILD)/_deps/cjson-build/libcjson.dylib \
   $(WOLFRAM_HOST_BUILD)/_deps/cjson-build/libcjson.a))

CFLAGS  := -std=gnu11 -g -O1 -Wall -Wextra -I$(SRC) $(SDL_CFLAGS) $(CURL_CFLAGS) \
           $(IMAGE_CFLAGS) -include $(CURDIR)/host_sdl_compat.h
LDLIBS  := $(SDL_LIBS) $(IMAGE_LIBS) $(CURL_LIBS) -lmbedcrypto -lm -lpthread

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
	$(SRC)/net/http.c \
	$(SRC)/ui/imagecache.c \
	$(SRC)/ui/keyboard.c \
	$(SRC)/util/rng.c \
	$(SRC)/util/timefmt.c \
	$(SRC)/atproto/feed.c \
	$(SRC)/app/timeline.c \
	$(SRC)/app/thread.c \
	$(SRC)/app/compose.c \
	$(SRC)/app/notify.c \
	$(SRC)/atproto/notifications.c \
	$(SRC)/atproto/profile.c \
	$(SRC)/app/profile.c \
	$(SRC)/ui/postcard.c \
	$(SRC)/ui/render.c \
	$(SRC)/ui/theme.c \
	$(SRC)/input/input.c

TEST_BIN := $(OUT)/cobalt-tests

#---------------------------------------------------------------------------------
# Compile sweep.
#
# Everything except the translation units that reach for devkitPro headers:
#   net/net.c     - <nn/ac.h>
#   util/log.c    - <coreinit/debug.h>, Aroma's logging module
#   util/paths.c  - <whb/sdcard.h>
# Those are covered by the hardware pass, which is the only place they can be.
#
# main.c is swept, and matters: it owns the startup and shutdown ordering, which
# is exactly the code a hardware pass is slowest to tell you about. The Wii U
# window flags it needs are stubbed in host_sdl_compat.h. It is kept out of the
# link check below only because linkcheck_main.c supplies that binary's main().
#---------------------------------------------------------------------------------
SWEEP_SOURCES := $(filter-out \
	$(SRC)/net/net.c $(SRC)/util/log.c $(SRC)/util/paths.c, \
	$(wildcard $(SRC)/*.c $(SRC)/*/*.c))

LINK_SOURCES := $(filter-out $(SRC)/main.c, $(SWEEP_SOURCES))

.PHONY: all check sweep linkcheck clean

all: sweep linkcheck check

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
# Link check.
#
# The sweep above is -fsyntax-only and therefore never resolves a symbol: a
# function deleted while callers remained compiles clean and fails only at link
# time. The unit-test binary catches that, but only for the without-Wolfram
# configuration — so the halves of feed.c, notifications.c and session.c behind
# COBALT_HAS_WOLFRAM had no link coverage at all. This gives them some.
#
# Needs a host build of Wolfram and is skipped without one, so a checkout with
# only the headers still gets the sweep and the unit tests.
#---------------------------------------------------------------------------------
LINK_BIN := $(OUT)/cobalt-linkcheck

linkcheck:
ifeq ($(HAVE_WOLFRAM_LIB),)
	@echo "linkcheck ... skipped (no host Wolfram build at $(WOLFRAM_HOST_BUILD))"
else
	@echo "linkcheck ... linking the Wolfram configuration"
	@mkdir -p $(OUT)
	@$(CC) $(CFLAGS) -DCOBALT_HAS_WOLFRAM=1 -I$(WOLFRAM_ROOT)/include \
		$(CJSON_CFLAGS) -o $(LINK_BIN) \
		linkcheck_main.c linkcheck_stubs.c host_stubs.c $(LINK_SOURCES) \
		$(HAVE_WOLFRAM_LIB) $(WOLFRAM_CJSON) $(LDLIBS) \
		-Wl,-rpath,$(abspath $(WOLFRAM_HOST_BUILD)) \
		-Wl,-rpath,$(abspath $(dir $(WOLFRAM_CJSON)))
	@echo "linkcheck ... ok"
endif

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
