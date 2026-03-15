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

## v1.2: TLV Binary Serialization Protocol (Current)
* **Goal:** Implement a robust binary serialization protocol using Tag-Length-Value (TLV) to safely encode strings, integers, arrays, and errors.
* **Architecture Shift:**
    * **Zero-Copy Streaming:** Eliminated the intermediate `Response` struct. The server now streams TLV encoded bytes directly into the `conn->outgoing` buffer using targeted helper functions (`out_str`, `out_int`, etc.), preventing unnecessary memory allocations.
    * **The Placeholder Trick:** Because the protocol requires a total message length header upfront, but the dynamic TLV message size isn't known until generation finishes, the server inserts a 4-byte zero placeholder, serializes the data, calculates the consumed bytes, and retroactively overwrites the placeholder.
    * **Client Recursive Parsing:** Upgraded the client to parse binary streams dynamically. Implemented a recursive `print_response` function that reads the tag byte and conditionally consumes exactly the right amount of bytes, natively supporting nested arrays and preventing buffer underflows.
* **Outcome:** A highly efficient, type-safe binary protocol that prepares the architecture for more complex, nested data types in the future.

## v1.3: Sorted Sets & Polymorphic Database (Current)
* **Goal:** Implement Sorted Sets (ZSETs) to support ranking, fast point lookups, and range queries.
* **Architecture Shift:**
    * **Multi-Type Store:** Transitioned the global hash table from a simple string store to a polymorphic database using an `Entry` struct with type tags (`T_STR`, `T_ZSET`). This enforces strict type safety at the command routing layer.
    * **Dual Intrusive Data Structures:** Built the `ZSet` combining an AVL tree (for sorting) and a Hash table (for point lookups). The `ZNode` payload intrusively embeds both `AVLNode` and `HNode` simultaneously, decoupling the tree and hash logic from the payload itself.
    * **Order Statistic Tree:** Augmented the AVL tree to track subtree sizes (`cnt`), enabling an `avl_offset` function that mathematically skips entire branches to achieve $O(\log N)$ range query offsets instead of standard $O(N)$ linear scans.
    * **Memory Optimization:** Leveraged C-style Flexible Array Members (`char name[0]`) within `ZNode` to allocate the struct and its variable-length string in a single contiguous memory block, drastically reducing heap fragmentation and allocation overhead.
    * **Zero-Allocation Lookups:** Introduced a stack-allocated `LookupKey` with a pre-calculated hash code to search the global dictionary, preventing unnecessary heap allocations during `GET`, `SET`, and `DEL` operations.
* **Outcome:** A type-safe, multi-structure database engine capable of handling complex composite data types with enterprise-grade algorithmic efficiency.

## v2.0: Event-Driven Timers & Cache Expiration (Current)
* **Goal:** Implement robust connection lifecycle management and automatic TTL-based cache eviction without blocking the event loop.
* **Architecture Shift:**
    * **Dynamic Event Loop Timeouts:** Modified the `poll()` syscall to use a dynamically calculated `timeout_ms` instead of sleeping indefinitely (`-1`). The server now calculates the exact delta to the nearest timer, allowing it to wake up autonomously for cleanup sweeps.
    * **O(1) Idle Connection Queue:** Implemented a custom intrusive Doubly Linked List (`DList`) to track connection age. Because all connections have a fixed 5-second timeout, the list naturally acts as a strictly ordered FIFO queue. This allows $O(1)$ identification of expired connections at the head, entirely avoiding $O(N)$ linear scans.
    * **Min-Heap for TTLs:** Engineered an array-encoded Min-Heap to track arbitrary Time-to-Live (TTL) values for database keys. Because TTLs are random, the binary heap structure guarantees $O(1)$ access to the nearest expiring key (the root) and $O(\log N)$ updates, ensuring thousands of simultaneous expirations don't degrade throughput.
    * **Cross-Referenced Intrusive Data:** Embedded a `heap_idx` within the database `Entry` payload, while giving the `HeapItem` a back-reference pointer to that index. When the heap re-sorts itself (`heap_up`/`heap_down`), it uses this pointer to silently update the database on its new array location. This allows $O(\log N)$ synchronization when a user manually deletes a key before its timer naturally expires.
    * **Amortized Cleanup:** Implemented a work limit (`k_max_works`) inside the TTL cleanup routine to prevent latency spikes. If thousands of keys expire on the exact same millisecond, the server evicts them in smaller batches across multiple event loop iterations.
* **Outcome:** The database now autonomously manages resource limits and cache freshness with minimal CPU overhead, evolving into a production-ready TTL caching layer.