# Stage 1: Build the C++ server
FROM gcc:12 AS builder
WORKDIR /app

# Copy your source code into the container
COPY . .

# Compile the static library and the server
RUN make clean && make server

# Stage 2: Create the lightweight production image
FROM debian:bookworm-slim
WORKDIR /app

# Copy ONLY the compiled server from the builder stage
COPY --from=builder /app/server .

# Expose the port Aphelo runs on
EXPOSE 1234

# Start the database!
CMD ["./server"]