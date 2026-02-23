# Compiler settings
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -g

# Define the output executables
TARGETS = server client

# Default target: builds both server and client
all: $(TARGETS)

# Server build rules
# It recompiles if server.cpp, hashtable.cpp, or hashtable.h changes
server: server.cpp hashtable.cpp hashtable.h
	$(CXX) $(CXXFLAGS) server.cpp hashtable.cpp -o server

# Client build rules
client: client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o client

# Cleanup rule to remove binaries and object files
clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean