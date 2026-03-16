#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>

// Helper to append raw bytes
static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

// Pack a single command into the buffer using your custom protocol
static void pack_command(std::vector<uint8_t> &buf, const std::vector<std::string> &args) {
    uint32_t body_len = 4; // 4 bytes for the 'nstr' field
    for (const std::string &s : args) {
        body_len += 4 + s.size(); // 4 bytes for string length + actual string size
    }

    // 1. Total Message Length
    buf_append(buf, (const uint8_t *)&body_len, 4);
    
    // 2. Number of Strings
    uint32_t nstr = args.size();
    buf_append(buf, (const uint8_t *)&nstr, 4);
    
    // 3. The Strings themselves
    for (const std::string &s : args) {
        uint32_t slen = s.size();
        buf_append(buf, (const uint8_t *)&slen, 4);
        buf_append(buf, (const uint8_t *)s.data(), s.size());
    }
}

int main() {
    // 1. Connect to the server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK); // 127.0.0.1
    
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        std::cerr << "Failed to connect to server!\n";
        return 1;
    }

    int num_requests = 100000;
    std::vector<uint8_t> write_buf;

    std::cout << "Packing " << num_requests << " SET commands...\n";
    for (int i = 0; i < num_requests; i++) {
        std::string key = "key_" + std::to_string(i);
        pack_command(write_buf, {"set", key, "bench_value"});
    }

    std::cout << "Starting Benchmark...\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    // 2. Blast all 100,000 requests into the socket (Pipelining!)
    size_t written = 0;
    while (written < write_buf.size()) {
        ssize_t rv = write(fd, write_buf.data() + written, write_buf.size() - written);
        if (rv <= 0) {
            std::cerr << "Write error\n";
            return 1;
        }
        written += rv;
    }

    // 3. Read the responses (We just wait until we read enough bytes to account for all responses)
    // Every "SET" response in your protocol is an INT tag (NIL tag = 5 bytes total: 4 length + 1 TAG_NIL)
    size_t expected_response_bytes = num_requests * 5; 
    size_t total_read = 0;
    char read_buf[64 * 1024];

    while (total_read < expected_response_bytes) {
        ssize_t rv = read(fd, read_buf, sizeof(read_buf));
        if (rv <= 0) {
            std::cerr << "Read error\n";
            break;
        }
        total_read += rv;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    // 4. Calculate Math
    double rps = num_requests / elapsed.count();
    
    std::cout << "--------------------------------------\n";
    std::cout << "Completed " << num_requests << " requests in " << elapsed.count() << " seconds.\n";
    std::cout << "Throughput: " << rps << " Requests Per Second (RPS)\n";
    std::cout << "--------------------------------------\n";

    close(fd);
    return 0;
}