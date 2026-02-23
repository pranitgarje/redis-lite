#include <assert.h>
#include <stdlib.h>     // calloc(), free()
#include "hashtable.h"


static void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);                //A safety check making sure n (the table size) is a power of 2 (like 4, 8, 16)
    htab->tab = (HNode **)calloc(n, sizeof(HNode *));   //Asks the computer for memory for the array of buckets and fills them with zeroes (NULL).
    htab->mask = n - 1;
    htab->size = 0;                                    //Sets up our quick-math mask.
}

static void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;             //Finds the right bucket using bitwise math (super fast).
    HNode *next = htab->tab[pos];                       //This takes the new node and pushes it to the very front of the linked list in that bucket
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}


static HNode **h_lookup(HTab *htab, HNode *key, bool (*eq)(HNode *, HNode *)) {
    if (!htab->tab) return NULL;
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];     //This is the trickiest part. Instead of returning the node itself, it returns the pointer that is pointing to the node.
    for (HNode *cur; (cur = *from) != NULL; from = &cur->next) {           //It walks down the linked list in the bucket.  
        if (cur->hcode == key->hcode && eq(cur, key)) {                   //It uses a custom function (provided by me) to check if the keys actually match (since different keys can have the same hash).
            return from;
        }
    }
    return NULL;
}

//Uses that tricky double-pointer from to neatly remove a node from the chain by telling the previous item to point to the next item, skipping the target.
static HNode *h_detach(HTab *htab, HNode **from) {
    HNode *node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

const size_t k_rehashing_work = 128;
/*It loops up to 128 times. It finds items in the older table, detaches them, and inserts them into the newer table. If the older table becomes empty, it frees the memory.*/
static void hm_help_rehashing(HMap *hmap) {
    size_t nwork = 0;
    while (nwork < k_rehashing_work && hmap->older.size > 0) {
        // find a non-empty slot
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if (!*from) {
            hmap->migrate_pos++;
            continue;   // empty slot
        }
        // move the first list item to the newer table
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    // discard the old table if done
    if (hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};
    }
}
/*When the newer table is too full, it moves the whole table into the older slot, creates a new table twice the size, and resets the moving tracker.*/
static void hm_trigger_rehashing(HMap *hmap) {
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->newer.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);                               //Does a little bit of moving work (help_rehashing).
    HNode **from = h_lookup(&hmap->newer, key, eq);       //Searches the newer table.
    if (!from) {                                          //If it's not there, searches the older table.
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : NULL;
}
const size_t k_max_load_factor = 8;
void hm_insert(HMap *hmap, HNode *node) {
    if (!hmap->newer.tab) { h_init(&hmap->newer, 4); }          //Initializes the table if it's completely empty (starts with size 4).
    h_insert(&hmap->newer, node);                               //Always puts new items into the newer table.
     
    if (!hmap->older.tab) { 
        size_t shreshold = (hmap->newer.mask + 1) * k_max_load_factor;     //Checks if the table is too full (size > buckets * 8). If it is, it triggers a resize.
        if (hmap->newer.size >= shreshold) {
            hm_trigger_rehashing(hmap);
        }
    }
    hm_help_rehashing(hmap);                                       //Does a little bit of moving work.
}

HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);                                              //Helps rehash, then tries to remove the item from newer, then older.
    if (HNode **from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    if (HNode **from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return NULL;
}

void hm_clear(HMap *hmap) {                   //Frees all the memory
    free(hmap->newer.tab);
    free(hmap->older.tab);
    *hmap = HMap{};
}

/*Adds up the items in both newer and older to tell you the total count.*/
size_t hm_size(HMap *hmap) {
    return hmap->newer.size + hmap->older.size;
}