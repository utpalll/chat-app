# =============================================================================
#  Dockerfile - builds and runs the C++ chat server
# =============================================================================
#
#  Stage 1 (builder): compile the static server binary with g++.
#  Stage 2 (runtime): copy just the binary into a tiny image and run it.
#
#  Railway (and most container hosts) inject a PORT environment variable;
#  the server reads it automatically, so no extra configuration is needed.
# =============================================================================

# ---- Stage 1: build ----
FROM gcc:13 AS builder

WORKDIR /build

# Copy sources needed to compile the server.
COPY common.h server.cpp ./

# Build a statically linked binary so the runtime image needs no extra libs.
RUN g++ -std=c++17 -O2 -pthread -static -o chat-server server.cpp

# ---- Stage 2: runtime ----
FROM debian:bookworm-slim

WORKDIR /app

# Copy only the compiled binary from the builder stage.
COPY --from=builder /build/chat-server /app/chat-server

# Document the default port (Railway overrides this via the PORT env var).
EXPOSE 5555

# Launch the server. It binds to 0.0.0.0 and uses $PORT when present.
CMD ["/app/chat-server"]
