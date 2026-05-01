CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS ?=
LDFLAGS ?= -pthread
LDLIBS ?= -lCAENVME

# How does redax makefile even find this
CAEN_PREFIX ?= /daq_common3/caen
CPPFLAGS += -I$(CAEN_PREFIX)/include
LDFLAGS += -L$(CAEN_PREFIX)/lib

TARGET := board_stress_test
SRC := main.cc

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)
