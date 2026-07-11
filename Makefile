.PHONY: test clean

# Compiler flags: C++17, warnings, sanitizer
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -fsanitize=undefined -g

# Source files
SOURCES = vec_math.cpp aim.cpp test_aim.cpp
OBJECTS = $(SOURCES:.cpp=.o)
TARGET = test_aim

# Build the test executable
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $(TARGET) -lm

# Compile object files
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Run tests
test: $(TARGET)
	./$(TARGET)

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)

# Rebuild
rebuild: clean test
