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
static int32_t read_full(int fd, char *buf, size_t n){
    while(n>0){
        ssize_t rv=read(fd, buf, n);   //Stores the return value, which is the actual number of bytes read.
        if(rv<=0){
            return -1;   
              //If rv is 0, it means "End of File" (the connection was closed).
              // If rv is negative (-1), an error occurred.      
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;           //Subtracts the number of bytes just read from the total remaining count.
        buf += rv;                 //Moves the buffer pointer forward. If we read 10 bytes, the next read should start at index 10, not 0.
    }
    return 0;   //Returns 0 to indicate success (all bytes were read).

}
static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n); // System call to write data
        if (rv <= 0) {
            return -1;  // Returns -1 on failure
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;  // Decrease remaining count
        buf += rv;        // Advance buffer pointer
    }
    return 0; // Success
}

const size_t k_max_msg = 4096;    //It allocates 4 bytes for the header (length) and 4096 bytes for the maximum allowed message body.
static int32_t one_request(int connfd){
    char rbuf[4+k_max_msg];          //A buffer is allocated to hold the incoming data.
    errno=0;
    int32_t err=read_full(connfd,rbuf,4);    //It attempts to read exactly the first 4 bytes. These bytes represent the length of the upcoming message.
    if (err) {
        if (errno == 0) {
            fprintf(stderr, "EOF\n");
        } else {
            fprintf(stderr, "read() error\n");
        }
        return err;
    }
    uint32_t len=0;
    memcpy(&len, rbuf, 4);   //Copies the 4 bytes from the buffer rbuf into the integer variable len and this converts the raw bytes into a number
    if (len > k_max_msg) {
        fprintf(stderr, "too long\n");
        return -1;
        /*Validation: It checks if the client is trying to send a message larger than the buffer can hold (4096 bytes). If so, it rejects the request to prevent buffer overflow.*/
    }
    err = read_full(connfd, &rbuf[4], len);
    if (err) {
        fprintf(stderr, "read() error\n");
        return err;
    }
    /*read_full (Body): Now that the code knows the message is len bytes long, it reads exactly that many bytes.
    &rbuf[4]: It stores this data starting at index 4 of the buffer (immediately after the header*/

    printf("client says: %.*s\n", len, &rbuf[4]);

    // Sending the reply
    const char reply[] = "world";       //A new buffer is created to hold the outgoing message.
    char wbuf[4 + sizeof(reply)];       //This is 6 bytes (5 letters for "world" + 1 null terminator).
    len = (uint32_t)strlen(reply);     /*Calculates the length of the string "world" (5 bytes). It excludes the null terminator because the length prefix handles the boundary.*/
    memcpy(wbuf, &len, 4);             //Copies the length (5) into the first 4 bytes of wbuf
    memcpy(&wbuf[4], reply, len);      //Copies the actual string data ("world") into wbuf starting at offset 4.
    return write_all(connfd, wbuf, 4 + len);  // Sends the entire package (4 bytes header + 5 bytes body = 9 bytes total) to the client.
}
static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);
    if (n <= 0) {
        fprintf(stderr, "read() error or client closed\n");
        return;
    }
    rbuf[n] = '\0';
    fprintf(stderr, "client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
}

int main(){
    int fd=socket(AF_INET, SOCK_STREAM, 0);
    if(fd<0){
        die("socket()");
    }
    int val=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    struct sockaddr_in addr={};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(12345);
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    int rv=bind(fd,(const struct sockaddr*)&addr, sizeof(addr));
    if(rv){
        die("bind()");
    }
    rv=listen(fd,SOMAXCONN);
    if(rv){
        die("listen()");
    }
    while(true){
        struct sockaddr_in client_addr={};
        socklen_t addrlen=sizeof(client_addr);
        int connfd=accept(fd,(struct sockaddr*)&client_addr, &addrlen);
        if(connfd<0){
            continue;
        }
        while (true) {
            // here the server only serves one client connection at once
            int32_t err = one_request(connfd);
            if (err) {
                break;
            }
        }
        close(connfd);
    }
    
    close(fd);
    return 0;
}