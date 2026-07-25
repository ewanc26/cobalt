.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitPro")
endif

include $(DEVKITPRO)/wut/share/wut_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files to embed
# ROMFS is the directory containing assets bundled into the RPX/WUHB
#---------------------------------------------------------------------------------
TARGET		:=	cobalt
BUILD		:=	build
SOURCES		:=	src
DATA		:=
ROMFS		:=	romfs

WIIU_PORTLIBS	:=	$(PORTLIBS_PATH)/wiiu

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS	= -g -O2 -Wall $(MACHDEP) $(INCLUDE) \
	-I$(WUT_ROOT)/include \
	-I$(WIIU_PORTLIBS)/include \
	-I$(WIIU_PORTLIBS)/include/SDL2

CXXFLAGS	=	$(CFLAGS)

LDFLAGS	= $(RPXSPECS) -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
# Order matters: dependees first, dependers last
#---------------------------------------------------------------------------------
LIBS	:=	-lwut \
	-lSDL2_ttf -lSDL2 \
	-lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
	-lfreetype -lpng -lz \
	-lm

#---------------------------------------------------------------------------------
# list of directories containing libraries
#---------------------------------------------------------------------------------
LIBDIRS	:=	$(WIIU_PORTLIBS) $(PORTLIBS_PATH)/ppc $(WUT_ROOT)

#---------------------------------------------------------------------------------
# WUHB packaging metadata
#---------------------------------------------------------------------------------
APP_NAME	:=	Cobalt
APP_SHORTNAME	:=	Cobalt
APP_AUTHOR	:=	Ewan C
APP_ICON	:=	assets/icon.png
APP_TV_SPLASH	:=	assets/tv_splash.png
APP_DRC_SPLASH	:=	assets/drc_splash.png

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifneq ($(strip $(CPPFILES)),)
export LD	:=	$(CXX)
else
export LD	:=	$(CC)
endif

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD) \
			-I$(LIBOGC_INC)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
			-L$(LIBOGC_LIB)

export ROMFS_PATH	:=	$(CURDIR)/$(ROMFS)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

.PHONY: clean run bundle

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).rpx $(OUTPUT).wuhb

run: $(OUTPUT).wuhb
	@echo "Copy $(OUTPUT).wuhb to SD card or use Aroma FTP"

bundle: $(OUTPUT).wuhb
	@mkdir -p dist/apps/cobalt
	@cp $(OUTPUT).wuhb dist/apps/cobalt/
	@echo bundle: ready at dist/apps/cobalt

$(OUTPUT).wuhb: $(OUTPUT).rpx
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	$(bin2o)

DEPENDS	:=	$(OFILES:.o=.d)
-include $(DEPENDS)

#---------------------------------------------------------------------------------
else

$(OUTPUT).wuhb: $(OUTPUT).rpx
$(OUTPUT).rpx: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES_SOURCES) : $(HFILES)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	$(bin2o)

DEPENDS	:=	$(OFILES:.o=.d)
-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
