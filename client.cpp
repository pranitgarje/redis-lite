#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <cassert>

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}
static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}
static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

/*This is the main function. It takes a file descriptor (fd) representing the connection to the server, 
 and a string (text) that we want to send.*/
 const size_t k_max_msg = 4096;    
static int32_t query(int fd, const char *text){
    uint32_t len = (uint32_t)strlen(text);
    if (len > k_max_msg) {
        return -1;
          /*Checks if the message is too big (over 4096 bytes). If it is, the function stops and returns -1 to avoid errors.*/
    }
    char wbuf[4 + k_max_msg];     /*Creates a buffer large enough for the header (4 bytes) and the maximum possible message.*/
    memcpy(wbuf, &len, 4);   /*Copies the length of the message into the first 4 bytes of the buffer. This is the "Header."*/
    memcpy(&wbuf[4], text, len);   /*Copies the actual text message into the buffer, starting right after the header (index 4).*/
    if (int32_t err = write_all(fd, wbuf, 4 + len)) {
        return err;
    }
    char rbuf[4 + k_max_msg];   /*A buffer to hold the incoming reply.*/
    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);     /*Attempts to read exactly 4 bytes from the server*/
    if (err) {
        if (errno == 0) {
            fprintf(stderr, "EOF\n");
        } else {
            fprintf(stderr, "read() error\n");
        }
        return err;
    }
    memcpy(&len, rbuf, 4);  // assume little endian
    if (len > k_max_msg) {
        fprintf(stderr, "too long\n");
        return -1;
    }
    err = read_full(fd, &rbuf[4], len);
    if (err) {
        fprintf(stderr, "read() error\n");
        return err;
    }
    printf("server says: %.*s\n", len, &rbuf[4]);
    return 0;

}

int main(){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

     // multiple requests
    int32_t err = query(fd, "hello1");
    if (err) {
        goto L_DONE;
    }
    err = query(fd, "hello2");
    if (err) {
        goto L_DONE;
    }
    err = query(fd, "hello3");
    if (err) {
        goto L_DONE;
    }

L_DONE:
    close(fd);
    return 0;
}