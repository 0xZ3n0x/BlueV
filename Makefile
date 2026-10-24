# SPDX-License-Identifier: GPL-3.0-or-later
# BlueV — PS5 payload (ps5-payload-sdk)
#
# The whole program is C++20, built without exceptions and RTTI. libsbc from
# third_party/sbc is C and is built unmodified. The layers:
#
#   src/core            the Bluetooth stack, A2DP and the radio; no PS5 header
#   src/service         the service state machine, the web server, the log
#   src/platform/ps5    the only code that talks to the console
#   src/platform/posix  plain files (bonds, the log file)
#   src/app             main(): builds the real objects
#
# Host tests (gtest/gmock) are built with CMake: tests/run.sh.

HERE      := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
SRC_DIR   := $(HERE)/src
BUILD_DIR := $(HERE)/build
OBJ_DIR   := $(BUILD_DIR)/obj

TOOLCHAIN ?= /opt/ps5-payload-sdk/bin
CC        := $(TOOLCHAIN)/prospero-clang
CXX       := $(TOOLCHAIN)/prospero-clang++
STRIP     := $(TOOLCHAIN)/prospero-strip

VERSION  := $(shell cat $(HERE)/VERSION)

TARGET   := $(BUILD_DIR)/bluev.elf
STRIPPED := $(BUILD_DIR)/bluev.stripped.elf
RELEASE_DIR := $(BUILD_DIR)/release
RELEASE  := $(RELEASE_DIR)/bluev.elf

SBC_DIR   := $(HERE)/third_party/sbc/sbc

INCLUDES := -I$(SRC_DIR) -I$(SRC_DIR)/compat -I$(SBC_DIR) -I$(BUILD_DIR)
COMMON   := -O2 -Wall -Wextra -fno-strict-aliasing -MMD -MP \
            -ffile-prefix-map=$(HERE)=. \
            -DHAVE_CONFIG_H $(INCLUDES)
ifdef DEBUG
COMMON  += -g
endif
CFLAGS   := $(COMMON)
CXXFLAGS := $(COMMON) -std=c++20 -fno-exceptions -fno-rtti -Wthread-safety \
            -DBLUEV_VERSION=\"$(VERSION)\"
LDFLAGS  := -lkernel_sys

CXX_SRC := $(patsubst $(SRC_DIR)/%.cpp,%,$(shell find $(SRC_DIR)/core $(SRC_DIR)/service \
             $(SRC_DIR)/platform/ps5 $(SRC_DIR)/platform/posix $(SRC_DIR)/app -name '*.cpp' | sort))
SBC_SRC := sbc sbc_primitives sbc_primitives_mmx sbc_primitives_sse

OBJS := $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(CXX_SRC))) \
        $(addprefix $(OBJ_DIR)/sbc/,$(addsuffix .o,$(SBC_SRC)))

all: $(TARGET)

# the version is compiled into main.cpp only
$(OBJ_DIR)/app/main.o: $(HERE)/VERSION

$(BUILD_DIR)/web_page.h: $(HERE)/web/index.html $(HERE)/tools/embed.py
	@mkdir -p $(BUILD_DIR)
	python3 $(HERE)/tools/embed.py $< > $@

$(BUILD_DIR)/icon_png.h: $(HERE)/homebrew/bluev/sce_sys/icon0.png $(HERE)/tools/embed.py
	@mkdir -p $(BUILD_DIR)
	python3 $(HERE)/tools/embed.py $< icon_png > $@

# libsbc is built unmodified; compat/config.h is force-included for the
# build settings (SIMD paths, no run-time CPU probe).
$(OBJ_DIR)/sbc/%.o: $(SBC_DIR)/%.c | $(SBC_DIR)/sbc.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -include $(SRC_DIR)/compat/config.h -c $< -o $@

$(SBC_DIR)/sbc.h:
	git submodule update --init third_party/sbc

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)/web_page.h $(BUILD_DIR)/icon_png.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# The launcher entry: copy this folder to /data/homebrew/ on the console.
HB_DIR := $(BUILD_DIR)/homebrew/bluev

homebrew: $(TARGET)
	@mkdir -p $(HB_DIR)/sce_sys
	cp $(TARGET) $(HB_DIR)/eboot.elf
	cp $(HERE)/homebrew/bluev/homebrew.js $(HB_DIR)/
	cp $(HERE)/homebrew/bluev/sce_sys/icon0.png $(HB_DIR)/sce_sys/
	@echo "launcher entry in $(HB_DIR)"

strip: $(TARGET)
	$(STRIP) -o $(STRIPPED) $(TARGET)

# The file to put in a release: optimised (the default), without the symbol
# table and the compiler note, and with the paths of this machine taken out
# (tools/scrub_paths.py fails the build when one is left). Build it from a clean
# tree at the tag: make clean release. The file is build/release/bluev.elf, apart
# from build/bluev.elf, which is not scrubbed and must not be published.
release: $(TARGET)
	@mkdir -p $(RELEASE_DIR)
	$(TOOLCHAIN)/llvm-strip --strip-all -o $(RELEASE) $(TARGET)
	$(TOOLCHAIN)/llvm-objcopy --remove-section=.comment $(RELEASE)
	python3 $(HERE)/tools/scrub_paths.py $(RELEASE)
	cd $(RELEASE_DIR) && sha256sum bluev.elf > bluev.elf.sha256
	@echo "release file: $(RELEASE)"

clean:
	rm -rf $(BUILD_DIR)

-include $(OBJS:.o=.d)

.PHONY: all homebrew strip release clean
