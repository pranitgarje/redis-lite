
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
#include <math.h>   // isnan
#include "common.h"
#include "zset.h"
#include <time.h>
#include "list.h"
#include "hashtable.h"
#include "heap.h"
/*Remember how hashtable.cpp only gives us back an HNode*? We need to find the actual data (the string key/value) attached to it. This macro does pointer math. It takes the memory address of the HNode, subtracts its position inside the struct, and returns a pointer to the entire wrapper struct!*/

typedef std::vector<uint8_t> Buffer;



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
/*This grabs the current time in milliseconds. It uses CLOCK_MONOTONIC instead of the regular wall clock. Why? If the server administrator manually changes the system clock (or daylight saving time happens), a regular clock might jump backward, causing timers to break. A "monotonic" clock only ticks forward, making it perfect for measuring elapsed time.*/
static uint64_t get_monotonic_msec() {
    struct timespec tv = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return uint64_t(tv.tv_sec) * 1000 + tv.tv_nsec / 1000 / 1000;
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
    uint64_t last_active_ms = 0;    //This acts as the timestamp for when the client last did something.
    DList idle_node;               //This is the physical "link" that connects this specific client to the rest of the clients in the line.
};
static struct {
    HMap db;    // top-level hashtable
    std::vector<Conn *> fd2conn; // a map of all client connections, keyed by fd
    DList idle_list;     //This is the dummy "head" of the line. It sits in your global data tracker and represents the starting point of the queue.
    std::vector<HeapItem> heap;  //The array that holds our Min-Heap timers
} g_data;
/*Instead of std::map, your global database is now just a wrapper around your HMap manager from hashtable.h*/
/*std::map: It's a binary search tree (usually Red-Black Tree) under the hood. It maps a string (Key) to a string (Value).*/


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
    conn->last_active_ms = get_monotonic_msec();  //Stamps the client with the exact millisecond they connected.
    dlist_insert_before(&g_data.idle_list, &conn->idle_node);  //Inserts the client's idle_node right before the idle_list head. Because the list loops in a circle, inserting "before the head" places them exactly at the back of the line.
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

// error code for TAG_ERR
enum {
    ERR_UNKNOWN = 1,    // unknown command
    ERR_TOO_BIG = 2,    // response too big
    ERR_BAD_TYP = 3,    // unexpected value type
    ERR_BAD_ARG = 4,    // bad arguments
};

// data types of serialized data
enum {
    TAG_NIL = 0,    // nil
    TAG_ERR = 1,    // error code + msg
    TAG_STR = 2,    // string
    TAG_INT = 3,    // int64
    TAG_DBL = 4,    // double
    TAG_ARR = 5,    // array
};
// help functions for the serialization
static void buf_append_u8(Buffer &buf, uint8_t data) {
    buf.push_back(data);
}
static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, (const uint8_t *)&data, 4);
}
static void buf_append_i64(Buffer &buf, int64_t data) {
    buf_append(buf, (const uint8_t *)&data, 8);
}
static void buf_append_dbl(Buffer &buf, double data) {
    buf_append(buf, (const uint8_t *)&data, 8);
}


// append serialized data types to the back
static void out_nil(Buffer &out) {
    buf_append_u8(out, TAG_NIL);
}

static void out_str(Buffer &out, const char *s, size_t size) {
    buf_append_u8(out, TAG_STR);
    buf_append_u32(out, (uint32_t)size);
    buf_append(out, (const uint8_t *)s, size);
}
static void out_int(Buffer &out, int64_t val) {
    buf_append_u8(out, TAG_INT);
    buf_append_i64(out, val);
}
static void out_dbl(Buffer &out, double val) {
     buf_append_u8(out, TAG_DBL);
    buf_append_dbl(out, val);
 }
static void out_err(Buffer &out, uint32_t code, const std::string &msg) {
    buf_append_u8(out, TAG_ERR);
    buf_append_u32(out, code);
    buf_append_u32(out, (uint32_t)msg.size());
    buf_append(out, (const uint8_t *)msg.data(), msg.size());
}
static void out_arr(Buffer &out, uint32_t n) {
    buf_append_u8(out, TAG_ARR);
    buf_append_u32(out, n);
}
static size_t out_begin_arr(Buffer &out) {
    out.push_back(TAG_ARR);
    buf_append_u32(out, 0);     // filled by out_end_arr()
    return out.size() - 4;      // the `ctx` arg
}
static void out_end_arr(Buffer &out, size_t ctx, uint32_t n) {
    assert(out[ctx - 1] == TAG_ARR);
    memcpy(&out[ctx], &n, 4);
}

// // 1. The Output format
// struct Response{
//     uint32_t status=0;  //A simple integer code (0 = OK, 1 = Error, etc.).
//     std::vector<uint8_t> data;  //The actual payload we want to send back to the client (like the value of a key).
// };

/*This is your actual payload. Notice how HNode node; is embedded directly inside it.
 This is the "handle glued to the back" concept we discussed earlier.
  The hash table only cares about node, but you care about key and val.*/

  // value types
enum {
    T_INIT  = 0,
    T_STR   = 1,    // string
    T_ZSET  = 2,    // sorted set
};
// KV pair for the top-level hashtable
struct Entry {
    struct HNode node;  // hashtable node
    std::string key;
    // value
    size_t heap_idx = -1; // Tracks where this key's timer is in the heap (-1 means no TTL)
    uint32_t type = 0;    // one of the following
    std::string str;
    ZSet zset;
};

// --- NEW HEAP HELPERS ---
static void heap_delete(std::vector<HeapItem> &a, size_t pos) {
    a[pos] = a.back(); // Swap the deleted item with the last item
    a.pop_back();      // Remove the last item
    if (pos < a.size()) {
        heap_update(a.data(), pos, a.size()); // Re-sort the heap
    }
}

static void heap_upsert(std::vector<HeapItem> &a, size_t pos, HeapItem t) {
    if (pos < a.size()) {
        a[pos] = t;         // Update existing timer
    } else {
        pos = a.size();
        a.push_back(t);     // Add new timer
    }
    heap_update(a.data(), pos, a.size()); // Re-sort the heap
}
static Entry *entry_new(uint32_t type) {
    Entry *ent = new Entry();
    ent->type = type;
    return ent;
}
static void entry_del(Entry *ent) {
   // Remove from TTL heap if it has a timer
    if (ent->heap_idx != (size_t)-1) {
        heap_delete(g_data.heap, ent->heap_idx);
        ent->heap_idx = -1;
    }
    if (ent->type == T_ZSET) {
        zset_clear(&ent->zset);
    }
    delete ent;
}
static void entry_set_ttl(Entry *ent, int64_t ttl_ms) {
    if (ttl_ms < 0 && ent->heap_idx != (size_t)-1) {
        // A negative TTL means "remove the timer"
        heap_delete(g_data.heap, ent->heap_idx);
        ent->heap_idx = -1;
    } else if (ttl_ms >= 0) {
        // Add or update the timer in the heap
        uint64_t expire_at = get_monotonic_msec() + (uint64_t)ttl_ms;
        HeapItem item = {expire_at, &ent->heap_idx}; // Pass a pointer to the entry's heap_idx!
        heap_upsert(g_data.heap, ent->heap_idx, item);
    }
}





// -----------------------
struct LookupKey {
    struct HNode node;  // hashtable node
    std::string key;
};
// equality comparison for the top-level hashstable
static bool entry_eq(HNode *node, HNode *key) {
    struct Entry *ent = container_of(node, struct Entry, node);
    struct LookupKey *keydata = container_of(key, struct LookupKey, node);
    return ent->key == keydata->key;
}
/*This is an implementation of the FNV-1a hash algorithm.
 It loops through every character in your string and scrambles it into a 64-bit integer (hcode).
  This number tells the hash table which bucket to put the entry in.*/
// static uint64_t str_hash(const uint8_t *data, size_t len) {
//     uint32_t h = 0x811C9DC5;
//     for (size_t i = 0; i < len; i++) {
//         h = (h + data[i]) * 0x01000193;
//     }
//     return h;            moved to common.h 
// }


// static void do_get(std::vector<std::string> &cmd, Buffer &out) {
//     // a dummy `Entry` just for the lookup
//     Entry key;              //We create a temporary, fake Entry just to hold the key we are looking for.
//     key.key.swap(cmd[1]);
//     key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());         //We calculate the hash of the target string so the lookup function can find it fast.
//     // hashtable lookup
//     HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);                   //We ask your custom hash table to find the node. We pass in the dummy key and our entry_eq comparison function. If it returns NULL, the key doesn't exist (RES_NX).
//     if (!node) {
//        return out_nil(out);
//     }
//     // copy the value
//     const std::string &val = container_of(node, Entry, node)->val;
//     out_str(out, val.data(),val.size());
// }

static void do_get(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy struct just for the lookup
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable lookup
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!node) {
        return out_nil(out);
    }
    // copy the value
    Entry *ent = container_of(node, Entry, node);
    if (ent->type != T_STR) {
        return out_err(out, ERR_BAD_TYP, "not a string value");
    }
    return out_str(out, ent->str.data(), ent->str.size());
}


// static void do_set(std::vector<std::string> &cmd, Buffer &out) {
//     // a dummy `Entry` just for the lookup
//     Entry key;   //Similar to get, we create a dummy key and try to find it in the database first.
//     key.key.swap(cmd[1]);                                       //
//     key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
//     // hashtable lookup
//     HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
//     if (node) {       //If found: We don't need to insert a new node. We just find the existing Entry and use .swap() to quickly replace the old value with the new one.
//         // found, update the value
//         container_of(node, Entry, node)->val.swap(cmd[2]);
//     } else {
//         // not found, allocate & insert a new pair
//         Entry *ent = new Entry();
//         ent->key.swap(key.key);
//         ent->node.hcode = key.node.hcode;
//         ent->val.swap(cmd[2]);
//         hm_insert(&g_data.db, &ent->node);
//     }
//     /*If not found: We allocate fresh memory (new Entry()). 
//     We fill it with the key, the pre-calculated hash code,
//      and the value. Finally, we hand its &ent->node over to hm_insert to link it into the hash table.*/

//      return out_nil(out);  // NEW: Successfully set the data? Redis traditionally replies with NIL to save bandwidth.
// }
static void do_set(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy struct just for the lookup
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable lookup
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        // found, update the value
        Entry *ent = container_of(node, Entry, node);
        if (ent->type != T_STR) {
            return out_err(out, ERR_BAD_TYP, "a non-string value exists");
        }
        ent->str.swap(cmd[2]);
    } else {
        // not found, allocate & insert a new pair
        Entry *ent = entry_new(T_STR);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->str.swap(cmd[2]);
        ent->heap_idx = -1;
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}
// static void do_del(std::vector<std::string> &cmd, Buffer &out) {
//     // a dummy `Entry` just for the lookup
//     Entry key;
//     key.key.swap(cmd[1]);
//     key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
//     // hashtable delete
//     HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);         //: This removes the node from the hash table's linked list and returns the detached node to us.
//     if (node) { // deallocate the pair
//         delete container_of(node, Entry, node);        //Because the hash table only manages links (not memory), it is our job to free the memory. We find the parent Entry and delete it so we don't cause a memory leak.
//         return out_int(out, 1); // NEW: Send an INT tag with '1' meaning "1 item deleted"
//     }
//     return out_int(out,0); // NEW: If the key doesn't exist, we reply with '0' meaning "0 items deleted"
// }
static void do_del(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy struct just for the lookup
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    // hashtable delete
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) { // deallocate the pair
        entry_del(container_of(node, Entry, node));
    }
    return out_int(out, node ? 1 : 0);
}
static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *(Buffer *)arg;
    const std::string &key = container_of(node, Entry, node)->key;
    out_str(out, key.data(), key.size());
    return true;
}





static bool str2int(const std::string &s, int64_t &out) {
    char *endp = NULL;
    out = strtoll(s.c_str(), &endp, 10);
    return endp == s.c_str() + s.size();
}
// PEXPIRE key ttl_ms
static void do_expire(std::vector<std::string> &cmd, Buffer &out) {
    int64_t ttl_ms = 0;
    if (!str2int(cmd[2], ttl_ms)) {
        return out_err(out, ERR_BAD_ARG, "expect int64");
    }
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    
    if (node) {
        Entry *ent = container_of(node, Entry, node);
        entry_set_ttl(ent, ttl_ms);
    }
    return out_int(out, node ? 1: 0);
}

// PTTL key
static void do_ttl(std::vector<std::string> &cmd, Buffer &out) {
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    
    if (!node) {
        return out_int(out, -2);    // -2 means key not found
    }
    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx == (size_t)-1) {
        return out_int(out, -1);    // -1 means no TTL set
    }
    
    uint64_t expire_at = g_data.heap[ent->heap_idx].val;
    uint64_t now_ms = get_monotonic_msec();
    return out_int(out, expire_at > now_ms ? (expire_at - now_ms) : 0);
}
// -----------------------
static void do_keys(std::vector<std::string> &, Buffer &out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void *)&out);
}

static bool str2dbl(const std::string &s, double &out) {
    char *endp = NULL;
    out = strtod(s.c_str(), &endp);
    return endp == s.c_str() + s.size() && !isnan(out);
}



// zadd zset score name
static void do_zadd(std::vector<std::string> &cmd, Buffer &out) {
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_BAD_ARG, "expect float");
    }

    // look up or create the zset
    LookupKey key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);

    Entry *ent = NULL;
    if (!hnode) {   // insert a new key
        ent = entry_new(T_ZSET);
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        hm_insert(&g_data.db, &ent->node);
    } else {        // check the existing key
        ent = container_of(hnode, Entry, node);
        if (ent->type != T_ZSET) {
            return out_err(out, ERR_BAD_TYP, "expect zset");
        }
    }

    // add or update the tuple
    const std::string &name = cmd[3];
    bool added = zset_insert(&ent->zset, name.data(), name.size(), score);
    return out_int(out, (int64_t)added);
}

static const ZSet k_empty_zset;

static ZSet *expect_zset(std::string &s) {
    LookupKey key;
    key.key.swap(s);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *hnode = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (!hnode) {   // a non-existent key is treated as an empty zset
        return (ZSet *)&k_empty_zset;
    }
    Entry *ent = container_of(hnode, Entry, node);
    return ent->type == T_ZSET ? &ent->zset : NULL;
}

// zrem zset name
static void do_zrem(std::vector<std::string> &cmd, Buffer &out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name.data(), name.size());
    if (znode) {
        zset_delete(zset, znode);
    }
    return out_int(out, znode ? 1 : 0);
}

// zscore zset name
static void do_zscore(std::vector<std::string> &cmd, Buffer &out) {
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }

    const std::string &name = cmd[2];
    ZNode *znode = zset_lookup(zset, name.data(), name.size());
    return znode ? out_dbl(out, znode->score) : out_nil(out);
}

// zquery zset score name offset limit
static void do_zquery(std::vector<std::string> &cmd, Buffer &out) {
    // parse args
    double score = 0;
    if (!str2dbl(cmd[2], score)) {
        return out_err(out, ERR_BAD_ARG, "expect fp number");
    }
    const std::string &name = cmd[3];
    int64_t offset = 0, limit = 0;
    if (!str2int(cmd[4], offset) || !str2int(cmd[5], limit)) {
        return out_err(out, ERR_BAD_ARG, "expect int");
    }

    // get the zset
    ZSet *zset = expect_zset(cmd[1]);
    if (!zset) {
        return out_err(out, ERR_BAD_TYP, "expect zset");
    }

    // seek to the key
    if (limit <= 0) {
        return out_arr(out, 0);
    }
    ZNode *znode = zset_seekge(zset, score, name.data(), name.size());
    znode = znode_offset(znode, offset);

    // output
    size_t ctx = out_begin_arr(out);
    int64_t n = 0;
    while (znode && n < limit) {
        out_str(out, znode->name, znode->len);
        out_dbl(out, znode->score);
        znode = znode_offset(znode, +1);
        n += 2;
    }
    out_end_arr(out, ctx, (uint32_t)n);
}

static void do_request(std::vector<std::string> &cmd, Buffer &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else if (cmd.size() == 1 && cmd[0] == "keys") {
        return do_keys(cmd, out);
    } else if (cmd.size() == 4 && cmd[0] == "zadd") {
        return do_zadd(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "zrem") {
        return do_zrem(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "zscore") {
        return do_zscore(cmd, out);
    } else if (cmd.size() == 6 && cmd[0] == "zquery") {
        return do_zquery(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "pexpire") {  
        return do_expire(cmd, out);                       
    } else if (cmd.size() == 2 && cmd[0] == "pttl") {    
        return do_ttl(cmd, out); 
    } else {
        return out_err(out, ERR_UNKNOWN, "unknown command.");
    }
}


// /*Structure: The response protocol is simple: [Total Length] [Status Code] [Data Payload].
// resp_len: It calculates 4 (for the status code) + size of data.
// buf_append: This is just a helper (likely using std::vector::insert) to push raw bytes onto the output buffer*/
// static void make_response(const Response &resp, std::vector<uint8_t> &out) {
//     uint32_t resp_len = 4 + (uint32_t)resp.data.size();
//     // 1. Append Total Length (4 bytes)
//     buf_append(out, (const uint8_t *)&resp_len, 4);
//     // 2. Append Status Code (4 bytes)
//     buf_append(out, (const uint8_t *)&resp.status, 4);
//     // 3. Append the Actual Data
//     buf_append(out, resp.data.data(), resp.data.size());
// }

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

static void response_begin(Buffer &out, size_t *header) {
    *header = out.size();       // messege header position
    buf_append_u32(out, 0);     // reserve space
}

static size_t response_size(Buffer &out, size_t header) {
    return out.size() - header - 4;
}

static void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);
    if (msg_size > k_max_msg) {
        out.resize(header + 4);
        out_err(out, ERR_TOO_BIG, "response is too big.");
        msg_size = response_size(out, header);
    }
    // message header
    uint32_t len = (uint32_t)msg_size;
    memcpy(&out[header], &len, 4);
}
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
    // // 2. Process the command
    //  Response resp;
    // do_request(cmd, resp);
    // //// 3. Send the response back
    // make_response(resp, conn->outgoing);
    // // application logic done! remove the request message.

    
  size_t header_pos = 0;
    response_begin(conn->outgoing, &header_pos);
    do_request(cmd, conn->outgoing);
    response_end(conn->outgoing, header_pos);
    
    // application logic done! remove the request message.
    buf_consume(conn->incoming, 4 + len);
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
static void conn_destroy(Conn *conn) {
    (void)close(conn->fd);
    g_data.fd2conn[conn->fd] = NULL;
    dlist_detach(&conn->idle_node);
    delete conn;
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
const uint64_t k_idle_timeout_ms = 5 * 1000; // 5 seconds ,Sets the strict timeout limit (5 seconds in this code).

static uint32_t next_timer_ms() {
    uint64_t now_ms = get_monotonic_msec();
    uint64_t next_ms = (uint64_t)-1; // Set to the absolute maximum possible value

    // 1. Check idle connections
    if (!dlist_empty(&g_data.idle_list)) {
        Conn *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        next_ms = conn->last_active_ms + k_idle_timeout_ms;
    }

    // 2. Check the TTL heap (Compare it to the idle connection timer)
    if (!g_data.heap.empty() && g_data.heap[0].val < next_ms) {
        next_ms = g_data.heap[0].val; // The heap timer is sooner, use this one instead!
    }

    if (next_ms == (uint64_t)-1) {
        return -1;  // no timers at all
    }
    if (next_ms <= now_ms) {
        return 0;   // timer already expired
    }
    return (int32_t)(next_ms - now_ms);
}
static bool hnode_same(HNode *node, HNode *key) {
    return node == key;
}
static void process_timers() {
    uint64_t now_ms = get_monotonic_msec();

    // 1. Clean up expired idle connections (unchanged)
    while (!dlist_empty(&g_data.idle_list)) {
       // Look at the oldest connection at the front of the line
        Conn *conn = container_of(g_data.idle_list.next, Conn, idle_node);
        uint64_t next_timeout = conn->last_active_ms + k_idle_timeout_ms;

        if (next_timeout > now_ms) {
            // Not expired yet! Since the list is ordered by age, 
            // if this one hasn't expired, nobody else behind it has either.
            break;
        }

        // Connection is too old, kick them out to free up resources
        msg("idle connection expired");
        conn_destroy(conn);
    }

    // 2. Clean up expired database keys from the Heap (NEW)
    const size_t k_max_works = 2000; // Don't delete more than 2000 at once to prevent lag
    size_t nworks = 0;
    const std::vector<HeapItem> &heap = g_data.heap;
    
    while (!heap.empty() && heap[0].val < now_ms) {
        Entry *ent = container_of(heap[0].ref, Entry, heap_idx); // Find the database entry
        
        // Remove it from the database hashtable
        HNode *node = hm_delete(&g_data.db, &ent->node, &hnode_same);
        if (!node) {
            // SAFE FALLBACK: The key was somehow already missing. 
            // Just delete the memory to clean up the ghost timer and move on.
            ent->heap_idx = -1;
            entry_del(ent);
            continue;
        }
        
        // Actually delete the memory (this also safely removes it from the heap!)
        entry_del(ent); 
        
        if (nworks++ >= k_max_works) {
            break; // Stop if we've been deleting for too long
        }
    }
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
    dlist_init(&g_data.idle_list);
    if(fd<0){
        die("socket()");
    }
    int val=1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    struct sockaddr_in addr={};
    addr.sin_family=AF_INET;
    addr.sin_port=htons(1234);
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
    int rv=bind(fd,(const struct sockaddr*)&addr, sizeof(addr));
    if(rv){
        die("bind()");
    }
    rv=listen(fd,SOMAXCONN);
    if(rv){
        die("listen()");
    }
  
    std::vector<struct pollfd> poll_args;  /*A list of file descriptors we want to monitor.*/
    while(true){
        poll_args.clear();   /*We clear and rebuild this list every loop iteration because the state (want_read/want_write) changes constantly.*/
        struct pollfd pfd = {fd, POLLIN, 0};  /*Listening Socket: Always added first, always watching for POLLIN (new connections).*/
        poll_args.push_back(pfd);
        for (Conn *conn : g_data.fd2conn ) {
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
        /*Instead of passing -1 to poll() (which means "sleep forever until a message arrives"), we now pass timeout_ms. The server will wake up automatically if the timer runs out.*/
        int32_t timeout_ms = next_timer_ms();
        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), timeout_ms);  /*The Blocking Point. The program stops here and sleeps until the OS wakes it up because an event occurred on one of the FDs (or an error).*/
        if (rv < 0 && errno == EINTR) {
            continue;   // not an error
        }
        if (rv < 0) {
            die("poll");
        }
        // handle the listening socket
        if (poll_args[0].revents) {        /*Checks the first entry (listening socket). If revents (returned events) is non-zero, it means a new client is waiting. We accept them and add them to the fd2conn map.*/
            if (Conn *conn = handle_accept(fd)) {
                if (g_data.fd2conn.size() <= (size_t)conn->fd) {
                    g_data.fd2conn.resize(conn->fd + 1);
                }
                assert(!g_data.fd2conn[conn->fd]);
                g_data.fd2conn[conn->fd] = conn;
            }
        }  
        // handle connection sockets
        for (size_t i = 1; i < poll_args.size(); ++i) {  /*Loops through the rest of the sockets.*/
            uint32_t ready = poll_args[i].revents;
            Conn *conn = g_data.fd2conn[poll_args[i].fd];
            //If a client did something, refresh their timer!
            /*If a connection was active (they sent or received data), we update their timestamp to "now", rip them out of their current spot in line (dlist_detach), and shove them to the back of the line (dlist_insert_before).*/
            conn->last_active_ms = get_monotonic_msec();
            dlist_detach(&conn->idle_node);
            dlist_insert_before(&g_data.idle_list, &conn->idle_node);
            
            if (ready & POLLIN) {      /*If POLLIN is set, call handle_read.*/
                handle_read(conn); 
            }
            if (ready & POLLOUT) {     /*If POLLOUT is set, call handle_write.*/
                handle_write(conn);
            }
            
            // close the socket from socket error or application logic
            if ((ready & POLLERR) || conn->want_close) {
                conn_destroy(conn);
            }

    }
    // Kick out anyone who expired while we were sleeping
    //Calls our cleanup function at the end of every loop.
        process_timers();
}
    return 0;
}