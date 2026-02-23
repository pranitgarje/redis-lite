#pragma once
#include <stddef.h>
#include <stdint.h>

// hashtable node, should be embedded into the payload
/*
This is the "handle" you attach to your own data
*/
struct HNode {
    HNode *next = NULL;        //This is the "handle" you attach to your own data
    uint64_t hcode = 0;        //Stores the calculated hash value so we don't waste time recalculating it later.
};

/*A single, basic hash table.*/
struct HTab {
    HNode **tab = NULL;     //An array of pointers to HNodes. These are your "buckets."
    size_t mask = 0;       //A clever math trick. If your table size is 8 (binary 1000), the mask is 7 (0111). We use this to quickly find which bucket an item belongs in.
    size_t size = 0;        //How many items are currently in this specific table.
};

/*The "Manager" that handles our progressive resizing.
Holds two HTabs. When the table gets too full, newer becomes older, and a fresh, bigger table becomes newer*/
/*Keeps track of which bucket we are currently moving from older to newer*/
struct HMap {
    HTab newer;            
    HTab older;
    size_t migrate_pos = 0;
};


HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void   hm_insert(HMap *hmap, HNode *node);
HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void   hm_clear(HMap *hmap);
size_t hm_size(HMap *hmap);