# Compiler settings
CXX = g++
CXXFLAGS = -Wall -Wextra -O2 -g

# Define the output executables and the new library
TARGETS = server client test_heap benchmark
LIBRARY = libavl.a

# Default target: builds both server, client, and the test
all: $(TARGETS)

# ---------------------------------------------------------
# 1. BUILD THE AVL STATIC LIBRARY
# ---------------------------------------------------------
# Step A: Compile avl.cpp into an object file (avl.o)
avl.o: avl.cpp avl.h
	$(CXX) $(CXXFLAGS) -c avl.cpp -o avl.o

# Step B: Archive the object file into a static library (libavl.a)
$(LIBRARY): avl.o
	ar rcs $(LIBRARY) avl.o


# 2. SERVER & CLIENT BUILD RULES
# ---------------------------------------------------------
# Server now depends on the static library, the zset logic, AND the heap
server: server.cpp hashtable.cpp zset.cpp heap.cpp hashtable.h zset.h heap.h $(LIBRARY)
	$(CXX) $(CXXFLAGS) server.cpp hashtable.cpp zset.cpp heap.cpp -L. -lavl -o server

client: client.cpp $(LIBRARY)
	$(CXX) $(CXXFLAGS) client.cpp -o client

# 3. HEAP TEST BUILD RULE
# ---------------------------------------------------------
test_heap: test_heap.cpp heap.h
	$(CXX) $(CXXFLAGS) test_heap.cpp -o test_heap

benchmark: benchmark.cpp
	$(CXX) $(CXXFLAGS) benchmark.cpp -o benchmark

# Cleanup rule to remove binaries, object files, and libraries
clean:
	rm -f $(TARGETS) *.o *.a

.PHONY: all clean