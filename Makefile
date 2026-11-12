# RedBaboon: builds the game, the test titles, the host tests and the content preview. Everything a build writes
# goes under build/; `make clean` removes it.
#
#   make                 the game: build/RedBaboon/default.xbe
#   make iso             build/RedBaboon.iso, an XISO of the game for xemu
#   make demos           the renderer and audio test titles: build/RenderDemo/, build/AudioDemo/
#   make test            build and run the host tests (flight model, mission simulation, seed parser)
#   make audio_render    render every sound effect and music track to WAV files in build/audio/
#   make audio_game      run the game's audio through a whole mission on the host: build/audio/game_path.wav
#   make preview         render the procedural meshes and textures to PNG files in build/preview/
#   make xemu            run the game in xemu (tools/xemu, docs/xemu.md)
#   make hardware        install and run the game on a real Xbox (tools/hardware, docs/hardware.md); ARGS=... adds
#                        options, e.g. make hardware ARGS="--seconds 60 --record captures/run.mp4"
#   make clean           remove build/
#
# The Xbox builds need nxdk (https://github.com/XboxDev/nxdk) with its tools built, and clang and lld on PATH.
# Set NXDK_DIR (and OXOS_DIR for the xemu and hardware rigs) in the environment or in local.mk, which is not in git:
#   NXDK_DIR = /path/to/nxdk
#   OXOS_DIR = /path/to/oxos
#   export PATH := /usr/lib/llvm-18/bin:$(PATH)

ROOT := $(realpath $(CURDIR))
BUILD := $(ROOT)/build
-include $(ROOT)/local.mk
export NXDK_DIR OXOS_DIR
export PATH := $(NXDK_DIR)/bin:$(PATH)

GAME_SRCS = $(sort $(wildcard $(ROOT)/src/*.cpp $(ROOT)/src/*/*.cpp))
GAME_HDRS = $(wildcard $(ROOT)/src/*/*.h)
XBOX_MAKE = $(MAKE) --no-print-directory -f $(ROOT)/xbox.mk ROOT=$(ROOT)

.PHONY: all game iso demos render_demo audio_demo test audio_render audio_game preview xemu hardware clean \
        check_nxdk
all: game

# --- Xbox titles

check_nxdk:
	@test -f "$(NXDK_DIR)/Makefile" || { echo "NXDK_DIR is not set to an nxdk checkout (see the top of Makefile)"; \
	    exit 1; }

game: check_nxdk
	@mkdir -p $(BUILD)/obj/game
	+$(XBOX_MAKE) -C $(BUILD)/obj/game XBE_TITLE=RedBaboon OUTPUT_DIR=$(BUILD)/RedBaboon SDL3=y \
	    APP_SRCS="$(GAME_SRCS)"

iso: game
	+$(XBOX_MAKE) -C $(BUILD)/obj/game XBE_TITLE=RedBaboon OUTPUT_DIR=$(BUILD)/RedBaboon SDL3=y \
	    APP_SRCS="$(GAME_SRCS)" GEN_XISO=$(BUILD)/RedBaboon.iso

demos: render_demo audio_demo

render_demo: check_nxdk
	@mkdir -p $(BUILD)/obj/render_demo
	+$(XBOX_MAKE) -C $(BUILD)/obj/render_demo XBE_TITLE=RenderDemo OUTPUT_DIR=$(BUILD)/RenderDemo \
	    APP_SRCS="$(ROOT)/src/render/render_xbox.cpp $(ROOT)/tests/render_demo/main.cpp"

audio_demo: check_nxdk
	@mkdir -p $(BUILD)/obj/audio_demo
	+$(XBOX_MAKE) -C $(BUILD)/obj/audio_demo XBE_TITLE=AudioDemo OUTPUT_DIR=$(BUILD)/AudioDemo \
	    APP_SRCS="$(wildcard $(ROOT)/src/audio/*.cpp) $(ROOT)/tests/audio_demo/main.cpp"

# --- Host programs (g++ or clang on the PC)

HOST_CXX ?= g++
HOST_CXXFLAGS = -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src
HOST_GAME_SRCS = $(sort $(wildcard $(ROOT)/src/game/*.cpp $(ROOT)/src/content/*.cpp))
HOST_AUDIO_SRCS = $(addprefix $(ROOT)/src/audio/,synth.cpp sfx.cpp music.cpp mixer.cpp audio.cpp)
HOST_STUBS = $(addprefix $(ROOT)/tests/host/,render_null.cpp audio_null.cpp input_null.cpp)
HOST_TESTS = $(BUILD)/tests/flight_test $(BUILD)/tests/sim_test $(BUILD)/tests/seed_test

test: $(HOST_TESTS)
	$(BUILD)/tests/flight_test
	$(BUILD)/tests/sim_test
	$(BUILD)/tests/seed_test

$(BUILD)/tests/flight_test $(BUILD)/tests/sim_test: $(BUILD)/tests/%: $(ROOT)/tests/host/%.cpp $(HOST_GAME_SRCS) \
        $(HOST_STUBS) $(GAME_HDRS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -o $@ $< $(HOST_GAME_SRCS) $(HOST_STUBS)

$(BUILD)/tests/seed_test: $(ROOT)/tests/host/seed_test.cpp $(ROOT)/src/game/seed.cpp $(GAME_HDRS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_CXXFLAGS) -fno-exceptions -fno-rtti -o $@ $< $(ROOT)/src/game/seed.cpp

audio_render: $(BUILD)/tests/audio_render
	@mkdir -p $(BUILD)/audio
	$< $(BUILD)/audio

$(BUILD)/tests/audio_render: $(ROOT)/tests/host/audio_render/main.cpp $(HOST_AUDIO_SRCS) $(GAME_HDRS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_AUDIO_SRCS)

audio_game: $(BUILD)/tests/audio_game
	@mkdir -p $(BUILD)/audio
	$< $(BUILD)/audio/game_path.wav

$(BUILD)/tests/audio_game: $(ROOT)/tests/host/audio_game/main.cpp $(HOST_GAME_SRCS) $(HOST_AUDIO_SRCS) \
        $(GAME_HDRS)
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_GAME_SRCS) $(HOST_AUDIO_SRCS) \
	    $(ROOT)/tests/host/render_null.cpp $(ROOT)/tests/host/input_null.cpp

PREVIEW_SRCS = $(wildcard $(ROOT)/src/content/*.cpp) $(ROOT)/tools/preview/preview.cpp $(ROOT)/tools/preview/raster.cpp

preview: $(BUILD)/tools/preview
	$< $(BUILD)/preview

$(BUILD)/tools/preview: $(PREVIEW_SRCS) $(GAME_HDRS) $(ROOT)/tools/preview/raster.h
	@mkdir -p $(@D)
	$(HOST_CXX) $(HOST_CXXFLAGS) -I$(ROOT)/tools/preview -o $@ $(PREVIEW_SRCS)

# --- Rigs

xemu: game
	$(ROOT)/tools/xemu/run_game.sh $(BUILD)/RedBaboon $(ARGS)

hardware: game
	$(ROOT)/tools/hardware/run_game.sh $(BUILD)/RedBaboon $(ARGS)

clean:
	rm -rf $(BUILD)
