#include "cachehash.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <Judy.h>

#define EVICTION_NEEDED 1
#define EVICTION_UNNEC 0

// doubly-linked-list node
typedef struct node {
    struct node *next;
    struct node *prev;
    void *key;
    size_t keylen;
    void *data;
} node_t;

// data structure that contains the enterity of a linked list
// we typedef this as cachehash in cachehash.h s.t. external interface is clean
struct cachehash_s {
    Pvoid_t *judy;
    void *malloced;
    node_t *start;
    node_t *end;
    size_t maxsize;
    size_t currsize;
    cachehash_evict_cb *evict_cb;
};

cachehash* cachehash_init(size_t maxitems, cachehash_free_cb *cb)
{
    assert(maxitems > 0);
    cachehash *retv = malloc(sizeof(cachehash));
    assert(retv);
    memset(retv, 0, sizeof(cachehash));
    // allocate nodes all at once to avoid fragmented memory
    node_t *nodes = calloc(maxitems, sizeof(node_t));
    retv->malloced = nodes;
    assert(nodes);
    retv->start = nodes;
    retv->end = &nodes[maxitems-1];
    retv->maxsize = maxitems;
    retv->currsize = 0;
    // initial node
    nodes[0].next = &nodes[1];
    // middle nodes
    for (int i=1; i < maxitems - 1; i++) {
        nodes[i].prev = &nodes[i-1];
        nodes[i].next = &nodes[i+1];
    }
    // last node
    nodes[maxitems-1].prev = &nodes[maxitems-2];
    return retv;
}

// is the hashcache full?
static inline int eviction_needed(cachehash *ch)
{
    assert(ch);
    return ch->currsize == ch->maxsize;
}

// completely evict the LRU object
// does not cb to user w/ object
static inline void* evict(cachehash *ch)
{
    assert(ch);
    node_t *last = ch->end;
    // remove item from judy array
    Pvoid_t j = *ch->judy;
    int rc;
    JHSD(rc, j, last->key, last->keylen);
    // we should never end up with something in the linked list
    // that's not in the judy array.
    assert(rc);
    *ch->judy = j;
    // reset linked list node
    void *retv = last->data;
    last->data = NULL;
    free(last->key);
    last->key = NULL;
    last->keylen = 0;
    ch->currsize--;
    return retv;
}

static inline void use(cachehash *ch, node_t *n)
{
    assert(ch);
    assert(n);
    // if the first node, nothing to do
    if (!n->prev) {
        return;
    }
    // remove from current spot in linked list
    node_t *prev = n->prev;
    n->prev->next = n->next;
    // if last node then no next, but must update LL
    if (n->next) {
        n->next->prev = prev;
    } else {
        ch->end = prev;
    }
    // front of list
    n->next = ch->start;
    ch->start = n;
    n->prev = NULL;
}

static inline node_t* judy_get(cachehash *ch, void *key, size_t keylen)
{
    assert(ch);
    assert(key);
    assert(keylen);
    Pvoid_t j = *ch->judy;
    Word_t *v_;
    JHSG(v_, j, key, keylen);
    *ch->judy = j;
    return (node_t*) *v_;
}


void* cachehash_has(cachehash *ch, void *key, size_t keylen)
{
    assert(ch);
    assert(key);
    assert(keylen);

    node_t *n = judy_get(ch, key, keylen);
    if (n) {
        return n->data;
    } else {
        return NULL;
    }
}

void* cachehash_get(cachehash *ch, void *key, size_t keylen)
{
    assert(ch);
    assert(key);
    assert(keylen);

    node_t *n = judy_get(ch, key, keylen);
    if (n) {
        use(ch, n);
        return n->data;
    } else {
        return NULL;
    }
}

void* cachehash_evict_if_full(cachehash *ch)
{
    assert(ch);

    if (eviction_needed(ch) == EVICTION_UNNEC) {
        return NULL;
    }
    return evict(ch);
}

void cachehash_put(cachehash *ch, void *key, size_t keylen, void *value)
{
    assert(ch);
    assert(key);
    assert(keylen);

    void *evicted = cachehash_evict_if_full(ch);
    if (evicted && ch->evict_cb) {
       ch->evict_cb(evicted); 
    }
    // create new node
    node_t *n = ch->end; 
    void *newkey = malloc(keylen);
    memcpy(newkey, key, keylen);
    n->key = newkey;
    n->keylen = keylen;
    n->data = value;
    // move to the front
    use(ch, n);
    ch->currsize++;
    // add to judy array
    Pvoid_t j = *ch->judy;
    Word_t *v_;
    JHSI(v_, j, key, keylen);
    // key should not already be in hash table
    assert(!*v_);
    *v_ = (Word_t) value;
}

void cachehash_free(cachehash *ch, cachehash_free_cb *cb)
{
    assert(ch);
    int rc;
    Pvoid_t j = *ch->judy;
    JHSFA(rc, j);
    node_t *n = ch->start;
    while (n->next) {
        if (n->key) {
            free(n->key);
            if (cb) {
                cb(n->data);
            }
        }
        n = n->next;
    }
    free(ch->malloced);
    free(ch);
} 