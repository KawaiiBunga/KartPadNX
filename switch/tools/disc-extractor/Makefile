# Standalone Switch NRO for on-console extraction of a user-supplied Wii dump.
DEVKITPRO ?= /opt/devkitpro
LIBNX     := $(DEVKITPRO)/libnx
PREFIX    := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
CXX       := $(PREFIX)g++
NACPTOOL  := $(DEVKITPRO)/tools/bin/nacptool
ELF2NRO   := $(DEVKITPRO)/tools/bin/elf2nro

ROOT      := ../../..
OUT       := $(ROOT)/build/switch-extractor
TARGET    := kartpad-disc-extractor
ARCH      := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CXXFLAGS  := $(ARCH) -O2 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections \
             -std=gnu++20 -D__SWITCH__ -I$(LIBNX)/include -I$(ROOT)/switch/src/runtime
LDFLAGS   := -specs=$(LIBNX)/switch.specs $(ARCH) -Wl,--gc-sections -L$(LIBNX)/lib
LIBS      := -lnx
SOURCES   := source/main.cpp $(ROOT)/switch/src/runtime/disc_extractor.cpp \
             $(ROOT)/switch/src/runtime/disc_reader.cpp $(ROOT)/switch/src/runtime/wii_cluster.cpp \
             $(ROOT)/switch/src/runtime/wii_crypto.cpp $(ROOT)/switch/src/runtime/wii_crypto_switch.cpp \
             $(ROOT)/switch/src/runtime/wii_common_key.cpp \
             $(ROOT)/switch/src/runtime/wii_fst.cpp
ELF       := $(OUT)/$(TARGET).elf
NACP      := $(OUT)/$(TARGET).nacp
NRO       := $(OUT)/$(TARGET).nro

all: $(NRO)

$(OUT):
	@mkdir -p $@

$(ELF): $(SOURCES) $(wildcard $(ROOT)/switch/src/runtime/*.h) Makefile | $(OUT)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) $(LIBS) -o $@

$(NACP): | $(OUT)
	$(NACPTOOL) --create "KartPad Disc Extractor" "KartPad-NX" "0.1.0" $@

$(NRO): $(ELF) $(NACP)
	$(ELF2NRO) $(ELF) $@ --nacp=$(NACP) --icon=$(LIBNX)/default_icon.jpg

clean:
	rm -f $(ELF) $(NACP) $(NRO)

.PHONY: all clean
