
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

## v1.0: The Reactive Database (Current)
* **Goal:** Feature-complete Key-Value store with stable architecture.
* **Architecture Shift:** Fully decoupled the "Network Layer" from the "Application Logic."
    * Implemented the **Reactor Pattern**: The main loop only cares about file descriptors; the logic is handled via callbacks (`handle_read`, `handle_write`).
* **State Management:**
    * Introduced explicit `want_read` and `want_write` flags in the `Conn` struct. This prevents busy-waiting and ensures we only ask the OS to poll for events we actually care about.
* **Data Layer:**
    * Integrated `std::map` as the in-memory backing store.
    * Implemented the Command Dispatcher to route `SET`, `GET`, and `DEL` commands to the store.
* **Outcome:** A functioning, non-blocking Redis clone capable of pipelining requests and maintaining persistent state.