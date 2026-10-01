#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO. Example: export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/devkitARM/3ds_rules

TARGET      := capture2cloud-old3ds
BUILD       := build
SOURCES     := source
DATA        :=
INCLUDES    := include ..
FFMPEG_PREFIX := $(TOPDIR)/build-tools/ffmpeg
FFMPEG_LIB := $(FFMPEG_PREFIX)/lib/libavcodec.a

APP_TITLE       := Capture2Cloud Old3DS
APP_DESCRIPTION := Low-latency Capture2Cloud client
APP_AUTHOR      := wozt
APP_ICON        := $(DEVKITPRO)/libctru/default_icon.png

ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS  := -g -Wall -Wextra -Werror -O3 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := -g $(ARCH)
LDFLAGS := -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS    := -lavcodec -lswscale -lavutil -lopus -ljpeg -lctru -lm
LIBDIRS := $(FFMPEG_PREFIX) $(CTRULIB) $(PORTLIBS)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
export OFILES := $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export LD := $(CC)

.PHONY: all clean cia $(BUILD)

all: $(FFMPEG_LIB) $(BUILD)

$(FFMPEG_LIB): $(TOPDIR)/scripts/build-ffmpeg-old3ds.sh
	DEVKITPRO=$(DEVKITPRO) $(TOPDIR)/scripts/build-ffmpeg-old3ds.sh

$(BUILD): $(FFMPEG_LIB)
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

cia: all
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).smdh \
		$(TARGET).cia $(TARGET).map $(TARGET).lst

else

DEPENDS := $(OFILES:.o=.d)

.PHONY: all cia
all: $(OUTPUT).3dsx $(OUTPUT).cia

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES) $(FFMPEG_LIB)

# makerom and bannertool are open-source host tools. scripts/build-3ds-tools.sh
# installs reproducible local copies when the distribution does not package them.
cia: $(OUTPUT).cia

$(OUTPUT).cia: $(OUTPUT).elf $(OUTPUT).smdh $(TOPDIR)/cia.rsf $(TOPDIR)/build-tools/makerom
	$(TOPDIR)/build-tools/makerom -f cia -o $@ -elf $(OUTPUT).elf \
		-rsf $(TOPDIR)/cia.rsf -icon $(OUTPUT).smdh -exefslogo

$(TOPDIR)/build-tools/makerom:
	$(TOPDIR)/scripts/build-3ds-tools.sh

-include $(DEPENDS)

endif
