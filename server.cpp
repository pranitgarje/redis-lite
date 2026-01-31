#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
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
        do_something(connfd);
        close(connfd);
    }
    
    close(fd);
    return 0;
}