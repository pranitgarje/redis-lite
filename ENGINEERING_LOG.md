# Engineering Log

## v0.1: The MVP (Jan 2026)
* **Goal:** Establish a TCP connection using C++ syscalls.
* **Design:** Used a simple iterative server (blocking).
* **Why:** To understand the basic `socket`, `bind`, `listen`, `accept` flow without the complexity of protocols.
* **Limitation:** The server could only handle one request and then immediately closed the socket.

## v0.2: Protocol & Persistence (Feb 2026)
* **Goal:** Keep the connection open for multiple commands.
* **Challenge:** TCP fragmentation. The server didn't know where one message stopped and the next began.
* **Solution:** Implemented a length-prefixed framing protocol.
    * Added a 4-byte header to every message.
    * Wrote a nested loop in `server.cpp` to keep reading until the client hangs up.

## v0.3: Concurrency & Event Loop (Feb 2026)
* **Goal:** Handle multiple clients simultaneously without using threads.
* **Challenge:** The v0.2 nested loop architecture "captured" the thread. If Client A was connected, Client B couldn't even handshake.
* **Solution:** Implemented IO Multiplexing using `poll()`.
    * **Non-Blocking:** Switched sockets to `O_NONBLOCK` using `fcntl`.
    * **Result:** The server can now interleave requests from multiple clients on a single thread.

## v1.0: The Reactive Database 
* **Goal:** Feature-complete Key-Value store with stable architecture.
* **Architecture Shift:** Fully decoupled the "Network Layer" from the "Application Logic."
    * Implemented the **Reactor Pattern**: The main loop only cares about file descriptors; the logic is handled via callbacks (`handle_read`, `handle_write`).
* **State Management:**
    * Introduced explicit `want_read` and `want_write` flags in the `Conn` struct. This prevents busy-waiting and ensures we only ask the OS to poll for events we actually care about.
* **Data Layer:**
    * Integrated `std::map` (O(log n) Red-Black Tree) as the temporary in-memory backing store.
* **Outcome:** A functioning, non-blocking Redis clone capable of pipelining requests and maintaining persistent state.

## v1.1: The Custom Hash Table & Intrusive Data Structures (Current)
* **Goal:** Replace the standard library `std::map` with a custom O(1) hash table to maximize throughput and achieve predictable, flat latency.
* **Challenge:** Resizing a massive hash table in a single-threaded event loop blocks the server, causing massive latency spikes (the "stop-the-world" problem). 
* **Architecture Shift:** * **Intrusive Data Structures:** Decoupled the hash table mechanics from the application data. The hash table only manages linking `HNode` pointers. The application embeds this `HNode` into its `Entry` payload and uses the `container_of` macro (pointer arithmetic) to resolve the full struct.
    * **Progressive Rehashing:** Engineered an `HMap` manager holding two tables (`newer` and `older`). When the load factor is exceeded, a resize triggers, but the work is distributed. Subsequent `GET`/`SET`/`DEL` operations do a small chunk of work (`hm_help_rehashing`), gradually moving buckets to prevent blocking the event loop.
    * **Hashing Algorithm:** Integrated the FNV-1a algorithm for fast, uniform string hashing.
* **Outcome:** Transformed the data layer into a highly specialized, latency-optimized, and fully custom in-memory store.