#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string.h>

static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void pack_command(std::vector<uint8_t> &buf, const std::vector<std::string> &args) {
    uint32_t body_len = 4;
    for (const std::string &s : args) {
        body_len += 4 + s.size(); 
    }
    buf_append(buf, (const uint8_t *)&body_len, 4);
    uint32_t nstr = args.size();
    buf_append(buf, (const uint8_t *)&nstr, 4);
    for (const std::string &s : args) {
        uint32_t slen = s.size();
        buf_append(buf, (const uint8_t *)&slen, 4);
        buf_append(buf, (const uint8_t *)s.data(), s.size());
    }
}

void run_benchmark(int fd, const std::string& test_name, const std::vector<uint8_t>& write_buf, size_t expected_response_bytes, int num_requests) {
    std::cout << "Starting " << test_name << " Benchmark...\n";
    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. Blast all requests
    size_t written = 0;
    while (written < write_buf.size()) {
        ssize_t rv = write(fd, write_buf.data() + written, write_buf.size() - written);
        if (rv <= 0) {
            std::cerr << "Write error\n";
            return;
        }
        written += rv;
    }

    // 2. Read all responses
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
    double rps = num_requests / elapsed.count();
    
    std::cout << "  -> " << num_requests << " requests in " << elapsed.count() << " seconds.\n";
    std::cout << "  -> Throughput: " << rps << " RPS\n\n";
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK); 
    
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        std::cerr << "Failed to connect to server!\n";
        return 1;
    }

    int num_requests = 100000;

    // --- 1. SET BENCHMARK ---
    std::vector<uint8_t> set_buf;
    for (int i = 0; i < num_requests; i++) {
        pack_command(set_buf, {"set", "key_" + std::to_string(i), "bench_value"});
    }
    // SET returns NIL (TAG_NIL). 4 byte header + 1 byte tag = 5 bytes per response.
    run_benchmark(fd, "SET (Write)", set_buf, num_requests * 5, num_requests);


    // --- 2. GET BENCHMARK ---
    std::vector<uint8_t> get_buf;
    for (int i = 0; i < num_requests; i++) {
        pack_command(get_buf, {"get", "key_" + std::to_string(i)});
    }
    // GET returns STR. 4 byte header + 1 byte tag + 4 byte str_len + 11 byte string ("bench_value") = 20 bytes.
    run_benchmark(fd, "GET (Read)", get_buf, num_requests * 20, num_requests);


    // --- 3. ZADD BENCHMARK ---
    std::vector<uint8_t> zadd_buf;
    for (int i = 0; i < num_requests; i++) {
        // zadd scores 1.0 player_1, 2.0 player_2, etc.
        pack_command(zadd_buf, {"zadd", "leaderboard", std::to_string(i) + ".0", "player_" + std::to_string(i)});
    }
    // ZADD returns INT. 4 byte header + 1 byte tag + 8 byte int = 13 bytes.
    run_benchmark(fd, "ZADD (Sorted Set)", zadd_buf, num_requests * 13, num_requests);

    close(fd);
    return 0;
}