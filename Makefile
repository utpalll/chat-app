# =============================================================================
#  Makefile for the C++ Chat Application
# =============================================================================
#
#  Targets:
#    make            -> build both `server` and `client`
#    make server     -> build only the server
#    make client     -> build only the client
#    make clean      -> remove the compiled binaries
#
# =============================================================================

CXX       := g++
CXXFLAGS  := -std=c++17 -Wall -Wextra -O2 -pthread
LDFLAGS   := -pthread

# Shared header that both binaries depend on.
COMMON    := common.h

# Build everything by default.
all: server client

server: server.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) server.cpp -o server $(LDFLAGS)

client: client.cpp $(COMMON)
	$(CXX) $(CXXFLAGS) client.cpp -o client $(LDFLAGS)

clean:
	rm -f server client

.PHONY: all clean
