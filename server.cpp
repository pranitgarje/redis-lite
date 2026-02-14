
// stdlib
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
// C++
#include <string>
#include <vector>
#include <map>

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno:%d] %s\n", errno, msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);  //Gets the current status flags of the file descriptor fd
    if (errno) {
        die("fcntl error");
        return;
    }

    flags |= O_NONBLOCK;  /*tells the OS that read() and write() on this socket should never "block" (sleep) waiting for data. If data isn't ready, they return immediately with an error (EAGAIN).*/

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags); // Applies the modified flags back to the file descriptor
    if (errno) {
        die("fcntl error");
    }
}

const size_t k_max_msg = 32 << 20;  // likely larger than the kernel buffer
struct Conn {
    int fd = -1;
    bool want_read = false;   // Do we want to read from the socket?
    bool want_write = false;  // Do we have data waiting to be written?
    bool want_close = false;  // Should we close this connection?
    std::vector<uint8_t> incoming;  // Buffer for data received but not yet parsed
    std::vector<uint8_t> outgoing;  // Buffer for data waiting to be sent
};
/*Buffering: Because TCP is a stream (not packets), one read() might give us half a message, or 2.5 messages.
 We append everything to incoming until we have a full message. Similarly, outgoing stores data if the socket isn't ready to send it all at once*/

 static void buf_append(std::vector<uint8_t> &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);    //Adds new raw bytes to the end of the vector.
}
static void buf_consume(std::vector<uint8_t> &buf, size_t n) {
    buf.erase(buf.begin(), buf.begin() + n);           //Removes bytes from the front of the vector after they have been processed or sent.
}

static Conn *handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);  /*Accepts a pending connection from the listening socket.*/
    if (connfd < 0) {                                     /*The new file descriptor specifically for this client.*/
        msg_errno("accept() error");
        return NULL;
    }
       uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );
    fd_set_nb(connfd);   /*Crucial. The new connection must also be non-blocking*/
    Conn *conn = new Conn();    
    conn->fd = connfd;
    conn->want_read = true;            //initializes the state. We default to wanting to read requests from the client.
    return conn;
}
const size_t k_max_args = 200 * 1000;
/*This function grabs 4 bytes from our data and turns them into a 32-bit integer (like turning 04 00 00 00 into the number 4).*/
static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) {
        return false;
    }
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}
/*Same as read_32 but for reading string*/
static bool read_str(const uint8_t *&cur, const uint8_t *end, uint32_t n,std::string &out) {
    if(cur+n>end){
        return false;
    }
    out.assign((const char *)cur, n);  //uses out.assign(...) instead of memcpy
    cur+=n;
    return true;
}
static int32_t parse_req(const uint8_t *data,size_t size,std::vector<std::string> &out){
    const uint8_t *end=data+size;;  //Calculates where the buffer stops so we don't crash.
    uint32_t nstr=0;
    // 1. Read the number of strings
    if(!read_u32(data,end,nstr)){
        return -1;
    }
    // 2. Safety check: Ensure nstr is not unreasonably large to prevent potential DoS attacks or memory issues.
    if (nstr > k_max_args) {
        return -1;
    }
    // 3. Loop to read each string
    while(out.size()<nstr){
        uint32_t len=0;
        // 3a. Read length of next string
        if(!read_u32(data,end,len)){
            return -1;
        }
         // 3b. Read the actual string
         out.push_back(std::string());    
        if(!read_str(data,end,len,out.back())){
            return -1;
        }
       
    }
    // 4. Check for garbage
     if(data!=end){
            return -1;
        }
        return 0;
}
// Response::status
enum {
    RES_OK = 0,
    RES_ERR = 1,    // error
    RES_NX = 2,     // key not found
};

// 1. The Output formot
struct Response{
    uint32_t status=0;  //A simple integer code (0 = OK, 1 = Error, etc.).
    std::vector<uint8_t> data;  //The actual payload we want to send back to the client (like the value of a key).
};
// 2. The Actual Data Store (The "Database")
static std::map<std::string, std::string> g_data;
/*std::map: It's a binary search tree (usually Red-Black Tree) under the hood. It maps a string (Key) to a string (Value).*/

static void do_request(std::vector<std::string> &cmd,Response &out){
    // COMMAND: GET
    if(cmd.size()==2 && cmd[0]=="get"){  /*Validation: A get command must look like ["get", "key"]. If it has 3 parts or 1 part, it's invalid.*/
           auto it=g_data.find(cmd[1]);  //This searches the map for the key (cmd[1]). It returns an iterator (pointer-like object) 
           if(it==g_data.end()){
            out.status=RES_NX; //NX = Non-Existent
            return ;
           }
           const std::string &val = it->second;
           out.data.assign(val.begin(),val.end());
         
    }
    // COMMAND: SET
    /*Crucial detail: If the key cmd[1] doesn't exist, this operator creates it automatically with an empty value!*/
    else if (cmd.size() == 3 && cmd[0] == "set") {
        g_data[cmd[1]].swap(cmd[2]);
     /*.swap(cmd[2]): This is a performance trick.Instead of copying the massive string from cmd[2] into the map, we just swap the internal pointers.cmd[2] (the string in our input vector) becomes empty/garbage, and the map gets the data. This is extremely fast (O(1)).*/
    } 
    // COMMAND: DEL
    /*: Removes the key from the map. If the key didn't exist, it does nothing (safe).*/
    else if (cmd.size() == 2 && cmd[0] == "del") {
        g_data.erase(cmd[1]);
    } 
    // UNKNOWN
    else {
        out.status = RES_ERR; // Error
    }
}
/*Structure: The response protocol is simple: [Total Length] [Status Code] [Data Payload].
resp_len: It calculates 4 (for the status code) + size of data.
buf_append: This is just a helper (likely using std::vector::insert) to push raw bytes onto the output buffer*/
static void make_response(const Response &resp, std::vector<uint8_t> &out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    // 1. Append Total Length (4 bytes)
    buf_append(out, (const uint8_t *)&resp_len, 4);
    // 2. Append Status Code (4 bytes)
    buf_append(out, (const uint8_t *)&resp.status, 4);
    // 3. Append the Actual Data
    buf_append(out, resp.data.data(), resp.data.size());
}

/*static int32_t read_full(int fd, char *buf, size_t n){
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

} */
/*static int32_t write_all(int fd, const char *buf, size_t n) {
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
}  */

// const size_t k_max_msg = 4096;    //It allocates 4 bytes for the header (length) and 4096 bytes for the maximum allowed message body.
// process 1 request if there is enough data
static bool try_one_request(Conn *conn) {
    // try to parse the protocol: message header
    if (conn->incoming.size() < 4) {      //Checks if we have at least 4 bytes (the header). If not, returns false (need more data).
        return false;   // want read
    }
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;   // want close
    }
    // message body
    if (4 + len > conn->incoming.size()) {     
        return false;   // want read
        /*Checks if the full message (Header + Body) is in the buffer. If incoming has 100 bytes but the message says it's 200 bytes long, we return false and wait for more data.*/
    }
    const uint8_t *request = &conn->incoming[4];
    // got one request, do some application logic
    std::vector<std::string> cmd;
    //// 1. Try to parse the accumulated buffer
    if (parse_req(request, len, cmd) < 0) {
        msg("bad request");
        conn->want_close = true;
        return false;   // want close
    }
    // 2. Process the command
     Response resp;
    do_request(cmd, resp);
    //// 3. Send the response back
    make_response(resp, conn->outgoing);
    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
    // Q: Why not just empty the buffer? See the explanation of "pipelining".
    return true;        // success
 
    
}
// application callback when the socket is writable
static void handle_write(Conn *conn) {
    assert(conn->outgoing.size() > 0);
    ssize_t rv = write(conn->fd, &conn->outgoing[0], conn->outgoing.size());  /*Attempts to write everything in the outgoing buffer to the socket.*/
    if (rv < 0 && errno == EAGAIN) {   /*This is the expected behavior for non-blocking I/O. It means the kernel's write buffer is full. We simply return and try again later.*/
        return; // actually not ready
    }
    if (rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;    // error handling
        return;
    }

    // remove written data from `outgoing`
    buf_consume(conn->outgoing, (size_t)rv);      /*Removes the bytes that were successfully written.*/

    // update the readiness intention
    if (conn->outgoing.size() == 0) {   // all data written
        conn->want_read = true;
        conn->want_write = false;  /*State Update: If the buffer is empty (all data sent), we switch flags to want_read (wait for next request) and stop want_write.*/
    } // else: want write
}
// application callback when the socket is readable
static void handle_read(Conn *conn) {
    // read some data
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));    /*Reads as much data as available (up to 64KB) into a temporary stack buffer.*/
    if (rv < 0 && errno == EAGAIN) {
        return; // actually not ready
    }
    // handle IO error
    if (rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return; // want close
    }
    // handle EOF
    if (rv == 0) {
        if (conn->incoming.size() == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return; // want close
    }
    // got some new data
    buf_append(conn->incoming, buf, (size_t)rv);    /*Moves the data from the temporary buffer into the connection's incoming queue.*/

    // parse requests and generate responses
    while (try_one_request(conn)) {}
    // Q: Why calling this in a loop? See the explanation of "pipelining".
    /*Pipelining Loop: This is a key optimization. A client might send 3 requests back-to-back in one packet. 
    This loop processes all available complete requests in the buffer before returning.*/

    // update the readiness intention
    if (conn->outgoing.size() > 0) {    // has a response
        conn->want_read = false;
        conn->want_write = true;
        // The socket is likely ready to write in a request-response protocol,
        // try to write it without waiting for the next iteration.
        return handle_write(conn);
    }   // else: want read
}

/*static void do_something(int connfd) {
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
}  */

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
    // a map of all client connections, keyed by fd
    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;  /*A list of file descriptors we want to monitor.*/
    while(true){
        poll_args.clear();   /*We clear and rebuild this list every loop iteration because the state (want_read/want_write) changes constantly.*/
        struct pollfd pfd = {fd, POLLIN, 0};  /*Listening Socket: Always added first, always watching for POLLIN (new connections).*/
        poll_args.push_back(pfd);
        for (Conn *conn : fd2conn) {
            /*Checks every active connection.*/
            if (!conn) continue;
            struct pollfd pfd = {conn->fd, POLLERR, 0};  /*Converts our application flags (want_read) into poll flags (POLLIN).*/
            if (conn->want_read) {
                pfd.events |= POLLIN;  /*POLLIN: Notify me when data is available to read.*/
            }
            if (conn->want_write) {
                pfd.events |= POLLOUT;      /*POLLOUT: Notify me when space is available to write.*/
            }
            poll_args.push_back(pfd);
        }
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);  /*The Blocking Point. The program stops here and sleeps until the OS wakes it up because an event occurred on one of the FDs (or an error).*/
        if (rv < 0 && errno == EINTR) {
            continue;   // not an error
        }
        if (rv < 0) {
            die("poll");
        }
        // handle the listening socket
        if (poll_args[0].revents) {        /*Checks the first entry (listening socket). If revents (returned events) is non-zero, it means a new client is waiting. We accept them and add them to the fd2conn map.*/
            if (Conn *conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }  
        // handle connection sockets
        for (size_t i = 1; i < poll_args.size(); ++i) {  /*Loops through the rest of the sockets.*/
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];
            
            if (ready & POLLIN) {      /*If POLLIN is set, call handle_read.*/
                handle_read(conn); 
            }
            if (ready & POLLOUT) {     /*If POLLOUT is set, call handle_write.*/
                handle_write(conn);
            }
            
            // close the socket from socket error or application logic
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
            }

    }
}
    return 0;
}