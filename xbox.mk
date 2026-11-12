# Builds one nxdk title. The top-level Makefile runs it from the title's work folder under build/ (make -C
# build/obj/NAME -f xbox.mk ...), so every object, generated shader, main.exe and libSDL3.lib lands there and the
# source tree stays clean. nxdk's own Makefile would put each object next to its source.
#
# Set by the caller:
#   ROOT        the repository
#   XBE_TITLE   the title name in the XBE header
#   OUTPUT_DIR  the folder that receives default.xbe
#   APP_SRCS    the title's .cpp files (absolute paths under ROOT)
#   SDL3        y to link SDL3 from third_party/nxdk-sdl3

NXDK_CXX = y
SRCS := $(APP_SRCS)
OBJ_DIR := $(CURDIR)/obj
GEN_DIR := $(CURDIR)/gen
SHADER_DIR := $(ROOT)/src/render/shaders

# nxdk's Makefile sets OBJS and SHADER_OBJS itself (next to the sources); override keeps these instead.
override OBJS = $(patsubst $(ROOT)/%,$(OBJ_DIR)/%.obj,$(basename $(SRCS)))
override SHADER_OBJS = $(patsubst $(SHADER_DIR)/%.vs.cg,$(GEN_DIR)/shaders/%.inl,$(wildcard $(SHADER_DIR)/*.vs.cg)) \
                       $(patsubst $(SHADER_DIR)/%.ps.cg,$(GEN_DIR)/shaders/%.inl,$(wildcard $(SHADER_DIR)/*.ps.cg))

CXXFLAGS += -std=c++20 -I$(ROOT)/src -I$(GEN_DIR) -O2 -Wall -Wno-unused-function
# The main thread's stack: nxdk's default 64 KB had about 1 KB to spare (a 1 KB buffer in main overflowed it into
# an exception in the title scene, 2026-11-11), so 256 KB.
LDFLAGS += -stack:262144

include $(NXDK_DIR)/Makefile

# SDL3 for the controller and timers only (a submodule: git submodule update --init --recursive). nxdk's compiler
# adds no -O, so its objects get -O2 here.
ifeq ($(SDL3),y)
override SDL3_OBJS = $(patsubst $(ROOT)/%,$(OBJ_DIR)/%.obj,$(basename $(SDL3_SRCS)))
include $(ROOT)/third_party/nxdk-sdl3/config_sdl.make
CFLAGS += $(SDL3_FLAGS)
CXXFLAGS += $(SDL3_FLAGS)
$(SDL3_OBJS): CFLAGS += -O2
main.exe: libSDL3.lib
endif

$(OBJ_DIR)/%.obj: $(ROOT)/%.cpp
	@mkdir -p $(@D)
	@echo "[ CXX      ] $*.cpp"
	$(VE) $(CXX) $(NXDK_CXXFLAGS) $(CXXFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.d)' -c -o '$@' '$<'

$(OBJ_DIR)/%.obj: $(ROOT)/%.c
	@mkdir -p $(@D)
	@echo "[ CC       ] $*.c"
	$(VE) $(CC) $(NXDK_CFLAGS) $(CFLAGS) -MD -MP -MT '$@' -MF '$(@:.obj=.d)' -c -o '$@' '$<'

$(GEN_DIR)/shaders/%.inl: $(SHADER_DIR)/%.vs.cg $(VP20COMPILER)
	@mkdir -p $(@D)
	@echo "[ CG       ] $*.vs.cg"
	$(VE) $(CGC) -profile vp20 -o $@.tmp $< $(QUIET) && $(VP20COMPILER) $@.tmp > $@ && rm -f $@.tmp

$(GEN_DIR)/shaders/%.inl: $(SHADER_DIR)/%.ps.cg $(FP20COMPILER)
	@mkdir -p $(@D)
	@echo "[ CG       ] $*.ps.cg"
	$(VE) $(CGC) -profile fp20 -o $@.tmp $< $(QUIET) && $(FP20COMPILER) $@.tmp > $@ && rm -f $@.tmp

-include $(OBJS:.obj=.d) $(SDL3_OBJS:.obj=.d)
