#pragma once
#include <stddef.h>
#include <stdint.h>

struct HeapItem {
    uint64_t val = 0; /*This is the expiration timestamp (the timer). The heap will constantly sort the items so the smallest val is always at index 0.*/
    size_t *ref = NULL;  /*. When the heap shuffles items around to keep them sorted, the main database needs to know where its timers went. *ref is a pointer to the database's record. When the heap moves an item, it uses this pointer to secretly update the database, saying: "Hey, I just moved this timer to index 5!".*/
};


void heap_update(HeapItem *a, size_t pos, size_t len);
