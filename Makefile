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

$(AIM_TARGET): $(AIM_OBJECTS)
	$(CXX) $(CXXFLAGS) $(AIM_OBJECTS) -o $(AIM_TARGET) -lm

$(FP_TARGET): $(FP_OBJECTS)
	$(CXX) $(CXXFLAGS) $(FP_OBJECTS) -o $(FP_TARGET) -lm

$(SHOW_TARGET): $(SHOW_OBJECTS)
	$(CXX) $(CXXFLAGS) $(SHOW_OBJECTS) -o $(SHOW_TARGET) -lm

$(EFFECTS_TARGET): $(EFFECTS_OBJECTS)
	$(CXX) $(CXXFLAGS) $(EFFECTS_OBJECTS) -o $(EFFECTS_TARGET) -lm

# Compile object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Run tests
test: $(AIM_TARGET) $(FP_TARGET) $(SHOW_TARGET) $(EFFECTS_TARGET)
	./$(AIM_TARGET)
	./$(FP_TARGET)
	./$(SHOW_TARGET)
	./$(EFFECTS_TARGET)

# Clean build artifacts
clean:
	rm -f $(AIM_OBJECTS) $(AIM_TARGET) $(FP_OBJECTS) $(FP_TARGET) $(SHOW_OBJECTS) $(SHOW_TARGET) \
	      $(EFFECTS_OBJECTS) $(EFFECTS_TARGET)

# Rebuild
rebuild: clean test
