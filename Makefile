# HexaMesh Dynamic Task Mapping Simulator
# Makefile

CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
TARGET = simulator
SRC_DIR = src
OUTPUT_DIR = output

# Source files
SOURCES = $(SRC_DIR)/main.cpp
HEADERS = $(wildcard include/*.hpp) $(wildcard benchmarks/*.hpp)

# Build target
all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@echo "Building HexaMesh Mapper..."
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SOURCES)
	@echo "✓ Build complete: ./$(TARGET)"

# Run all benchmarks
run: $(TARGET)
	@echo "Running all benchmarks..."
	./$(TARGET)

# Clean
clean:
	@echo "Cleaning..."
	rm -f $(TARGET)
	@echo "✓ Clean complete"

# Clean outputs
clean-output:
	@echo "Cleaning output directory..."
	rm -rf $(OUTPUT_DIR)/*
	@echo "✓ Output cleaned"

# Full clean
clean-all: clean clean-output

# Help
help:
	@echo "HexaMesh Makefile Commands:"
	@echo "  make          - Build"
	@echo "  make run      - Build and run"
	@echo "  make clean    - Remove executable"
	@echo "  make clean-all- Remove everything"
	@echo ""
	@echo "Visualization:"
	@echo "  python visualize.py --all"

.PHONY: all run clean clean-output clean-all help
