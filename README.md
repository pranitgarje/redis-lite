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

## 2. Architecture: v0.1 (Iterative Server)
*Current Version: v0.1 - Basic Client-Server Handshake*

The current implementation establishes the foundational network layer using the **Iterative Server Model**.

### Design Pattern
At this stage, the server handles requests synchronously (blocking I/O).
1.  **Socket Initialization:** The server requests a socket descriptor from the OS kernel.
2.  **Binding:** The server binds to `0.0.0.0` (INADDR_ANY) to accept connections from any interface.
3.  **Blocking Accept:** The main thread blocks at `accept()` until a client initiates a TCP handshake (SYN/ACK).
4.  **Sequential Processing:** Once a connection is established, the server reads the request, processes it, sends a response, and strictly closes the socket.
   

