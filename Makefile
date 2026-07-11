.PHONY: test clean

# Compiler flags: C++17, warnings, sanitizer
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g

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

# Compile object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

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

test: $(AIM_TARGET) $(FP_TARGET) $(SHOW_TARGET) $(EFFECTS_TARGET) $(SHOW_CONTROL_TARGET) $(PIXEL_MATRIX_TARGET) $(PROVISION_TARGET) $(LIVE_CONTROL_TARGET) $(WEB_PROTOCOL_TARGET) $(CONTROL_QUEUE_TARGET) $(PACING_TARGET) $(APPLY_TARGET)
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

# Clean build artifacts
clean:
	rm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \
	      $(EFFECTS_OBJECTS) $(EFFECTS_TARGET) $(SHOW_CONTROL_OBJECTS) $(SHOW_CONTROL_TARGET) \
	      $(PIXEL_MATRIX_OBJECTS) $(PIXEL_MATRIX_TARGET) $(PROVISION_OBJECTS) $(PROVISION_TARGET) \
	      $(LIVE_CONTROL_OBJECTS) $(LIVE_CONTROL_TARGET) \
	      $(WEB_PROTOCOL_OBJECTS) $(WEB_PROTOCOL_TARGET) \
	      $(CONTROL_QUEUE_TARGET)

# Rebuild
rebuild: clean test
