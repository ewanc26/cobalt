#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

TOPDIR ?= $(CURDIR)

include $(DEVKITPRO)/wut/share/wut_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files to embed
# INCLUDES is a list of directories containing extra header files
# CONTENT is the directory bundled into the WUHB as /vol/content
#---------------------------------------------------------------------------------
TARGET		:=	cobalt
BUILD		:=	build
SOURCES		:=	src src/app src/ui src/input src/net src/atproto src/cache src/util
DATA		:=
INCLUDES	:=	src
CONTENT		:=	romfs

#---------------------------------------------------------------------------------
# WUHB packaging metadata
#---------------------------------------------------------------------------------
APP_NAME	:=	Cobalt
APP_SHORT_NAME	:=	Cobalt
APP_AUTHOR	:=	Ewan C
ICON		:=	assets/icon.png
TV_SPLASH	:=	assets/tv_splash.png
DRC_SPLASH	:=	assets/drc_splash.png

WIIU_PORTLIBS	:=	$(PORTLIBS_PATH)/wiiu

#---------------------------------------------------------------------------------
# Wolfram — Ewan's C AT Protocol SDK, built for Wii U as a sibling checkout.
#
# AGENTS.md §8: use Wolfram rather than growing a second ATProto implementation
# inside Cobalt. It is optional at build time so the app still compiles without
# a Wolfram checkout; COBALT_HAS_WOLFRAM gates the code that uses it.
#
# Build the dependency first with:
#   cd ../wolfram && DEVKITPRO=/opt/devkitpro cmake -S . -B build-wiiu \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/.devdeps/wiiu.cmake -DWOLFRAM_BUILD_WIIU=ON \
#       -DWOLFRAM_BUILD_TESTS=OFF -DWOLFRAM_BUILD_EXAMPLES=OFF
#   cmake --build build-wiiu -j8 --target wolfram
#---------------------------------------------------------------------------------
# $(TOPDIR), not $(CURDIR): this file is re-parsed by the sub-make running
# inside $(BUILD), where CURDIR is the build directory and the sibling path
# would silently miss — leaving Wolfram quietly un-linked.
WOLFRAM_ROOT	?=	$(TOPDIR)/../wolfram
WOLFRAM_BUILD	?=	$(WOLFRAM_ROOT)/build-wiiu
WOLFRAM_LIB	:=	$(WOLFRAM_BUILD)/libwolfram.a

ifneq ($(wildcard $(WOLFRAM_LIB)),)
	WOLFRAM_CFLAGS	:=	-DCOBALT_HAS_WOLFRAM=1 \
				-I$(WOLFRAM_ROOT)/include \
				-I$(WOLFRAM_BUILD)/_deps/cjson-src
	WOLFRAM_LIBS	:=	$(WOLFRAM_LIB) \
				$(WOLFRAM_BUILD)/_deps/cjson-build/libcjson.a \
				$(WOLFRAM_BUILD)/_deps/libcbor-build/src/libcbor.a
else
	WOLFRAM_CFLAGS	:=
	WOLFRAM_LIBS	:=
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS	=	-g -Wall -Wextra -O2 -ffunction-sections -fdata-sections \
			$(MACHDEP) \
			$(INCLUDE) \
			-D__WIIU__ -DESPRESSO -DCURL_STATICLIB \
			-I$(WIIU_PORTLIBS)/include/SDL2 \
			$(WOLFRAM_CFLAGS)

CXXFLAGS	=	$(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti

ASFLAGS	:=	-g $(MACHDEP)
LDFLAGS	=	$(RPXSPECS) -g $(MACHDEP) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
# Order matters: dependers first, dependees last
#---------------------------------------------------------------------------------
LIBS	:=	-lSDL2_ttf -lSDL2 \
			$(WOLFRAM_LIBS) \
			-lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
			-lharfbuzz -lfreetype -lpng16 -lbz2 \
			-lbrotlidec -lbrotlicommon -lz \
			-lwut -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(WIIU_PORTLIBS) $(PORTLIBS_PATH)/ppc $(WUT_ROOT)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES_BIN		:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES			:=	$(OFILES_BIN) $(OFILES_SOURCES)

export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

#---------------------------------------------------------------------------------
# WUHB metadata is consumed by wut_rules inside the build directory, so the paths
# it references have to be absolute and exported into that sub-make's environment.
#---------------------------------------------------------------------------------
ifneq ($(strip $(CONTENT)),)
	export APP_CONTENT := $(TOPDIR)/$(CONTENT)
endif

ifneq ($(strip $(ICON)),)
	export APP_ICON := $(TOPDIR)/$(ICON)
else ifneq ($(wildcard $(TOPDIR)/$(TARGET).png),)
	export APP_ICON := $(TOPDIR)/$(TARGET).png
endif

ifneq ($(strip $(TV_SPLASH)),)
	export APP_TV_SPLASH := $(TOPDIR)/$(TV_SPLASH)
endif

ifneq ($(strip $(DRC_SPLASH)),)
	export APP_DRC_SPLASH := $(TOPDIR)/$(DRC_SPLASH)
endif

export APP_NAME
export APP_SHORTNAME := $(APP_SHORT_NAME)
export APP_AUTHOR

.PHONY: $(BUILD) all clean run bundle

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) dist $(OUTPUT).elf $(OUTPUT).rpx $(OUTPUT).wuhb

#---------------------------------------------------------------------------------
run: all
	@echo "Copy $(OUTPUT).wuhb to sd:/wiiu/apps/ or push it over Aroma's FTP server"

#---------------------------------------------------------------------------------
# The bundle carries a per-installation entropy seed alongside the WUHB. It is
# generated here, never committed, and must not be shared between consoles:
# Wolfram's DRBG is deterministic, so a common seed would give every install
# identical key material. Cobalt reads it from sd:/wiiu/apps/cobalt/entropy.bin
# (see src/util/entropy.h) and rotates it on every boot.
bundle: all
	@mkdir -p dist/wiiu/apps/$(TARGET)
	@cp $(OUTPUT).wuhb dist/wiiu/apps/
	@if [ ! -f "dist/wiiu/apps/$(TARGET)/entropy.bin" ]; then \
		openssl rand 64 > "dist/wiiu/apps/$(TARGET)/entropy.bin" && \
		echo bundle ... generated a fresh per-installation entropy seed; \
	fi
	@echo bundle ... dist/wiiu/apps/$(TARGET).wuhb

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all		:	$(OUTPUT).wuhb

$(OUTPUT).wuhb	:	$(OUTPUT).rpx
$(OUTPUT).rpx	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SOURCES) : $(HFILES_BIN)

#---------------------------------------------------------------------------------
# rule for embedding raw binary data into the executable
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
