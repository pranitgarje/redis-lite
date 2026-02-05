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
* **Goal:** handle multiple clients simultaneously without using threads.
* **Challenge:** The v0.2 nested loop architecture "captured" the thread. If Client A was connected, Client B couldn't even handshake until Client A disconnected.
* **Solution:** Implemented IO Multiplexing using `poll()`.
    * **Non-Blocking:** Switched sockets to `O_NONBLOCK` using `fcntl`.
    * **State Management:** Created a `Conn` struct to buffer data for each client independently.
    * **Result:** The server can now interleave requests from thousands of clients on a single thread.