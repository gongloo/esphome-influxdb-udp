CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../../components

# Include Homebrew path on macOS if present
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
	CXXFLAGS += -I$(BREW_PREFIX)/include
endif

SRCS = test_main.cpp test_line_protocol.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = test_runner

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all run clean

