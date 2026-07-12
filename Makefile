.PHONY: test clean

# Compiler flags: C++17, warnings, sanitizer
# -Ithird_party/lua: the vendored Lua 5.4.6 headers (lua_glow_include.h),
# needed by the Lua/Fennel layer (lua_vm.cpp, glow_lua_api.cpp,
# lua_effect.cpp, glow_fennel.cpp) and their tests. Harmless for every
# other target -- just an extra include search path.
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -Ithird_party/lua

# Vendored Lua 5.4.6 (third_party/lua/, LUA_32BITS=1 -- see
# scripts/vendor_lua.sh). Curated to exactly the libraries the sandbox
# opens (base/math/string/table + their aux lib): no io/os/debug/
# package/utf8/coroutine/ltests/onelua/lua.c compiled in at all, so an
# accidental attempt to open one of those would be a link error, not a
# silent sandbox regression.
LUA_CFLAGS = -std=c99 -Wall -Wextra -Werror -O2 -Ithird_party/lua
LUA_C_SOURCES = third_party/lua/lapi.c third_party/lua/lcode.c third_party/lua/lctype.c \
                third_party/lua/ldebug.c third_party/lua/ldo.c third_party/lua/ldump.c \
                third_party/lua/lfunc.c third_party/lua/lgc.c third_party/lua/llex.c \
                third_party/lua/lmem.c third_party/lua/lobject.c third_party/lua/lopcodes.c \
                third_party/lua/lparser.c third_party/lua/lstate.c third_party/lua/lstring.c \
                third_party/lua/ltable.c third_party/lua/ltm.c third_party/lua/lundump.c \
                third_party/lua/lvm.c third_party/lua/lzio.c third_party/lua/lauxlib.c \
                third_party/lua/lbaselib.c third_party/lua/lmathlib.c third_party/lua/lstrlib.c \
                third_party/lua/ltablib.c
LUA_C_OBJECTS = $(LUA_C_SOURCES:.c=.o)

# Modules shared by every Lua/Fennel test target.
GLOW_LUA_SOURCES = lua_vm.cpp glow_lua_api.cpp lua_effect.cpp glow_fennel.cpp eval_queue.cpp \
                   vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                   oscillator.cpp color.cpp effects.cpp show_control.cpp pixel_matrix.cpp \
                   pixel_patterns.cpp

# --- test_aim: aim/vec_math geometry tests ---
AIM_SOURCES = vec_math.cpp aim.cpp test_aim.cpp
AIM_OBJECTS = $(AIM_SOURCES:.cpp=.o)
AIM_TARGET = test_aim

# --- test_fixture_profile: fixture profile binary format tests ---
FP_SOURCES = fixture_profile.cpp profile_encoder.cpp test_fixture_profile.cpp
FP_OBJECTS = $(FP_SOURCES:.cpp=.o)
FP_TARGET  = test_fixture_profile

# --- test_show: render loop + show integration tests ---
# Core + tests only; device sinks (dmx_sink.cpp, artnet_sink.cpp) are
# device-only and excluded from the host build.
SHOW_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp test_show.cpp
SHOW_OBJECTS = $(SHOW_SOURCES:.cpp=.o)
SHOW_TARGET = test_show

# --- test_effects: oscillator/color/effects engine tests ---
EFFECTS_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                   oscillator.cpp color.cpp effects.cpp test_effects.cpp
EFFECTS_OBJECTS = $(EFFECTS_SOURCES:.cpp=.o)
EFFECTS_TARGET  = test_effects

# --- test_show_control: show control layer tests ---
SHOW_CONTROL_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                        show_control.cpp test_show_control.cpp
SHOW_CONTROL_OBJECTS = $(SHOW_CONTROL_SOURCES:.cpp=.o)
SHOW_CONTROL_TARGET  = test_show_control

# --- test_pixel_matrix: per-pixel matrix engine tests ---
PIXEL_MATRIX_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                       color.cpp pixel_matrix.cpp pixel_patterns.cpp test_pixel_matrix.cpp
PIXEL_MATRIX_OBJECTS = $(PIXEL_MATRIX_SOURCES:.cpp=.o)
PIXEL_MATRIX_TARGET  = test_pixel_matrix

# --- test_provision: provisioning compiler/loader tests ---
PROVISION_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp \
                    color.cpp pixel_matrix.cpp provision.cpp show_bundle.cpp test_provision.cpp
PROVISION_OBJECTS = $(PROVISION_SOURCES:.cpp=.o)
PROVISION_TARGET  = test_provision

# --- test_live_control: live control layer (MIDI/OSC/web → cues) tests ---
LIVE_CONTROL_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                       show_control.cpp live_control.cpp test_live_control.cpp
LIVE_CONTROL_OBJECTS = $(LIVE_CONTROL_SOURCES:.cpp=.o)
LIVE_CONTROL_TARGET  = test_live_control

# --- test_web_protocol: web console JSON protocol testable core tests ---
WEB_PROTOCOL_SOURCES = web_protocol.cpp test_web_protocol.cpp
WEB_PROTOCOL_OBJECTS = $(WEB_PROTOCOL_SOURCES:.cpp=.o)
WEB_PROTOCOL_TARGET  = test_web_protocol

# --- test_control_queue: control-event queue tests (TSan build) ---
# TSan and ASan cannot be combined in one binary, so this target gets
# its own compile rule with -fsanitize=thread instead of the default
# -fsanitize=address,undefined. Sources are compiled in a single
# command (no .o files) to avoid flag conflicts with the ASan objects.
CONTROL_QUEUE_CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -fsanitize=thread -g
CONTROL_QUEUE_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp profile_encoder.cpp show.cpp \
                        show_control.cpp live_control.cpp control_queue.cpp test_control_queue.cpp
CONTROL_QUEUE_TARGET  = test_control_queue

# --- test_lua_vm: LuaVM unit tests (allocator cap, sandbox, GC pacing,
# instruction budget). Fennel-free by design -- see lua_vm.cpp's header.
LUA_VM_SOURCES = lua_vm.cpp test_lua_vm.cpp
LUA_VM_OBJECTS = $(LUA_VM_SOURCES:.cpp=.o)
LUA_VM_TARGET  = test_lua_vm

# --- test_lua_effect: LuaEffect + GlowLuaApi + Fennel integration --
# the emit pattern, the error-disable contract, zero allocation.
LUA_EFFECT_SOURCES = $(GLOW_LUA_SOURCES) test_lua_effect.cpp
LUA_EFFECT_OBJECTS = $(LUA_EFFECT_SOURCES:.cpp=.o)
LUA_EFFECT_TARGET  = test_lua_effect

# --- test_glow_lua_api: the glow.* API surface (cue/scene state, glow.fx.*
# handles, glow.matrix.* via a fake registry).
GLOW_LUA_API_SOURCES = $(GLOW_LUA_SOURCES) test_glow_lua_api.cpp
GLOW_LUA_API_OBJECTS = $(GLOW_LUA_API_SOURCES:.cpp=.o)
GLOW_LUA_API_TARGET  = test_glow_lua_api

# --- test_glow_fennel: the process-wide VM singleton, glow_lua_eval_fennel,
# the eval submission queue drain -- the syntax-error/runtime-error/
# infinite-loop/OOM "rig keeps rendering" guarantees.
GLOW_FENNEL_SOURCES = $(GLOW_LUA_SOURCES) test_glow_fennel.cpp
GLOW_FENNEL_OBJECTS = $(GLOW_FENNEL_SOURCES:.cpp=.o)
GLOW_FENNEL_TARGET  = test_glow_fennel

# --- test_scripts_storage: scriptNameIsValid (the host-testable core of
# the "scripts" LittleFS partition; mount/read/save need real hardware).
SCRIPTS_STORAGE_SOURCES = scripts_storage.cpp test_scripts_storage.cpp
SCRIPTS_STORAGE_OBJECTS = $(SCRIPTS_STORAGE_SOURCES:.cpp=.o)
SCRIPTS_STORAGE_TARGET  = test_scripts_storage

$(AIM_TARGET): $(AIM_OBJECTS)
	$(CXX) $(CXXFLAGS) $(AIM_OBJECTS) -o $(AIM_TARGET) -lm

$(FP_TARGET): $(FP_OBJECTS)
	$(CXX) $(CXXFLAGS) $(FP_OBJECTS) -o $(FP_TARGET) -lm

$(SHOW_TARGET): $(SHOW_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SHOW_OBJECTS) -o $(SHOW_TARGET) -lm

$(EFFECTS_TARGET): $(EFFECTS_OBJECTS)
	$(CXX) $(CXXFLAGS) $(EFFECTS_OBJECTS) -o $(EFFECTS_TARGET) -lm

$(SHOW_CONTROL_TARGET): $(SHOW_CONTROL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SHOW_CONTROL_OBJECTS) -o $(SHOW_CONTROL_TARGET) -lm

$(PIXEL_MATRIX_TARGET): $(PIXEL_MATRIX_OBJECTS)
	$(CXX) $(CXXFLAGS) $(PIXEL_MATRIX_OBJECTS) -o $(PIXEL_MATRIX_TARGET) -lm

$(PROVISION_TARGET): $(PROVISION_OBJECTS)
	$(CXX) $(CXXFLAGS) $(PROVISION_OBJECTS) -o $(PROVISION_TARGET) -lm

$(LIVE_CONTROL_TARGET): $(LIVE_CONTROL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(LIVE_CONTROL_OBJECTS) -o $(LIVE_CONTROL_TARGET) -lm

$(WEB_PROTOCOL_TARGET): $(WEB_PROTOCOL_OBJECTS)
	$(CXX) $(CXXFLAGS) $(WEB_PROTOCOL_OBJECTS) -o $(WEB_PROTOCOL_TARGET) -lm

$(CONTROL_QUEUE_TARGET): $(CONTROL_QUEUE_SOURCES)
	$(CXX) $(CONTROL_QUEUE_CXXFLAGS) $(CONTROL_QUEUE_SOURCES) -o $(CONTROL_QUEUE_TARGET) -lm -pthread

$(LUA_VM_TARGET): $(LUA_VM_OBJECTS) $(LUA_C_OBJECTS)
	$(CXX) $(CXXFLAGS) $(LUA_VM_OBJECTS) $(LUA_C_OBJECTS) -o $(LUA_VM_TARGET) -lm

$(LUA_EFFECT_TARGET): $(LUA_EFFECT_OBJECTS) $(LUA_C_OBJECTS)
	$(CXX) $(CXXFLAGS) $(LUA_EFFECT_OBJECTS) $(LUA_C_OBJECTS) -o $(LUA_EFFECT_TARGET) -lm

$(GLOW_LUA_API_TARGET): $(GLOW_LUA_API_OBJECTS) $(LUA_C_OBJECTS)
	$(CXX) $(CXXFLAGS) $(GLOW_LUA_API_OBJECTS) $(LUA_C_OBJECTS) -o $(GLOW_LUA_API_TARGET) -lm

$(GLOW_FENNEL_TARGET): $(GLOW_FENNEL_OBJECTS) $(LUA_C_OBJECTS)
	$(CXX) $(CXXFLAGS) $(GLOW_FENNEL_OBJECTS) $(LUA_C_OBJECTS) -o $(GLOW_FENNEL_TARGET) -lm

$(SCRIPTS_STORAGE_TARGET): $(SCRIPTS_STORAGE_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SCRIPTS_STORAGE_OBJECTS) -o $(SCRIPTS_STORAGE_TARGET) -lm

# Compile object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Vendored Lua (third_party/lua/*.c) is C, compiled with its own flags --
# see LUA_CFLAGS above.
third_party/lua/%.o: third_party/lua/%.c
	$(CC) $(LUA_CFLAGS) -c $< -o $@

# Run tests
# --- test_render_pacing: firmware render-loop pacing math (host-tested) ---
PACING_SOURCES = render_pacing.cpp test_render_pacing.cpp
PACING_OBJECTS = $(PACING_SOURCES:.cpp=.o)
PACING_TARGET  = test_render_pacing
$(PACING_TARGET): $(PACING_OBJECTS)
	$(CXX) $(CXXFLAGS) $(PACING_OBJECTS) -o $(PACING_TARGET) -lm

# --- test_apply_loaded_show: F3 patch-routing glue (host-tested) ---
APPLY_SOURCES = vec_math.cpp aim.cpp fixture_profile.cpp show.cpp show_bundle.cpp \
                pixel_matrix.cpp apply_loaded_show.cpp test_apply_loaded_show.cpp
APPLY_OBJECTS = $(APPLY_SOURCES:.cpp=.o)
APPLY_TARGET  = test_apply_loaded_show
$(APPLY_TARGET): $(APPLY_OBJECTS)
	$(CXX) $(CXXFLAGS) $(APPLY_OBJECTS) -o $(APPLY_TARGET) -lm

test: $(AIM_TARGET) $(FP_TARGET) $(SHOW_TARGET) $(EFFECTS_TARGET) $(SHOW_CONTROL_TARGET) $(PIXEL_MATRIX_TARGET) $(PROVISION_TARGET) $(LIVE_CONTROL_TARGET) $(WEB_PROTOCOL_TARGET) $(CONTROL_QUEUE_TARGET) $(PACING_TARGET) $(APPLY_TARGET) $(LUA_VM_TARGET) $(LUA_EFFECT_TARGET) $(GLOW_LUA_API_TARGET) $(GLOW_FENNEL_TARGET) $(SCRIPTS_STORAGE_TARGET)
	./$(AIM_TARGET)
	./$(FP_TARGET)
	./$(SHOW_TARGET)
	./$(EFFECTS_TARGET)
	./$(SHOW_CONTROL_TARGET)
	./$(PIXEL_MATRIX_TARGET)
	./$(PROVISION_TARGET)
	./$(LIVE_CONTROL_TARGET)
	./$(WEB_PROTOCOL_TARGET)
	./$(CONTROL_QUEUE_TARGET)
	./$(PACING_TARGET)
	./$(APPLY_TARGET)
	./$(LUA_VM_TARGET)
	./$(LUA_EFFECT_TARGET)
	./$(GLOW_LUA_API_TARGET)
	./$(GLOW_FENNEL_TARGET)
	./$(SCRIPTS_STORAGE_TARGET)

# Clean build artifacts
clean:
	rm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \
	      $(EFFECTS_OBJECTS) $(EFFECTS_TARGET) $(SHOW_CONTROL_OBJECTS) $(SHOW_CONTROL_TARGET) \
	      $(PIXEL_MATRIX_OBJECTS) $(PIXEL_MATRIX_TARGET) $(PROVISION_OBJECTS) $(PROVISION_TARGET) \
	      $(LIVE_CONTROL_OBJECTS) $(LIVE_CONTROL_TARGET) \
	      $(WEB_PROTOCOL_OBJECTS) $(WEB_PROTOCOL_TARGET) \
	      $(CONTROL_QUEUE_TARGET) \
	      $(LUA_VM_OBJECTS) $(LUA_VM_TARGET) \
	      $(LUA_EFFECT_OBJECTS) $(LUA_EFFECT_TARGET) \
	      $(GLOW_LUA_API_OBJECTS) $(GLOW_LUA_API_TARGET) \
	      $(GLOW_FENNEL_OBJECTS) $(GLOW_FENNEL_TARGET) \
	      $(SCRIPTS_STORAGE_OBJECTS) $(SCRIPTS_STORAGE_TARGET) \
	      $(LUA_C_OBJECTS)

# Rebuild
rebuild: clean test
