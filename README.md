# redis-lite
A high-performance, in-memory key-value store built from scratch in C++. Implements a custom TCP server using raw sockets to replicate core Redis functionality without external dependencies.
**Redis-Lite** is a project to build a lightweight, high-performance in-memory key-value store from scratch. The goal is to deconstruct the internal architecture of databases like Redis to understand the complexities of network programming, memory management, and asynchronous I/O at the system level.

## 1. Engineering Decisions

### Why C++?
This project is built in C++ specifically to prioritize **control** and **performance**:
* **Zero-Overhead Abstraction:** Unlike Python or Java, C++ compiles directly to machine code without a Garbage Collector (GC), ensuring predictable latency—critical for a database.
* **System-Level Access:** The project requires direct manipulation of OS primitives (sockets, file descriptors, memory pages). C++ provides the raw pointer access needed to optimize how bytes are aligned and stored.
* **Memory Management:** Building a database requires fine-grained control over memory allocation to prevent fragmentation and leaks, which C++ enables manually.

### Why Custom Protocols?
Instead of using HTTP/REST frameworks (like Flask or Express), this project uses raw TCP sockets. This is to minimize the packet overhead and handle the raw byte streams directly, mimicking how production-grade databases handle low-level communication.

---
## 1. Architecture: v0.3 (Event-Driven & Non-Blocking)
*Current Version: v0.3 - Event Loop with `poll()`*

In previous versions, the server blocked on `accept()` or `read()`, meaning it could only serve one client at a time (or one per thread). v0.3 introduces a **Single-Threaded Event Loop** architecture, similar to the real Redis or Node.js.

### Core Concepts
1.  **Non-Blocking I/O:** All sockets are set to `O_NONBLOCK` mode. If a read/write operation can't complete immediately, the kernel returns `EAGAIN` instead of putting the process to sleep.
2.  **IO Multiplexing (`poll`):** Instead of checking sockets one by one, we use the `poll()` syscall to monitor all active connections simultaneously. The kernel wakes us up only when a socket is ready to be read from or written to.
3.  **State Machine:** Each connection is managed via a `Conn` struct that tracks its state (`want_read`, `want_write`) and maintains separate input/output buffers.

### System Design
The server now employs a "Connection Loop" separate from the "Request Loop":
1.  **Accept Phase:** The outer loop waits for a TCP handshake.
2.  **Request Phase:** Once connected, an inner `while(true)` loop continuously reads the 4-byte header, determines the message size, and processes the command.
3.  **Teardown:** The inner loop breaks only when the client disconnects or a protocol violation occurs.

**Sequence Diagram:**
![Sequence Diagram](assets/Architecture_v0.2.drawio.png) 

### Key Technical Implementation
* **The `Conn` Struct:** ```cpp
    struct Conn {
        int fd;
        bool want_read;  // State: Waiting for request?
        bool want_write; // State: Response ready to send?
        std::vector<uint8_t> incoming; // Read Buffer
        std::vector<uint8_t> outgoing; // Write Buffer
    };
    ```
* **Dynamic Buffering:** Unlike fixed `char` arrays, I implemented `std::vector<uint8_t>` buffers. This handles **TCP Streaming** issues (like partial reads) by appending data until a full frame (header + body) is available.
* **Pipelining Support:** The `handle_read` loop processes *all* available requests in the input buffer before returning to `poll()`. This allows a client to send 3 commands in one packet and get 3 responses, significantly increasing throughput.

