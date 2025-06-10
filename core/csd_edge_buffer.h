#ifndef CSD_EDGE_BUFFER_H
#define CSD_EDGE_BUFFER_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "params.h"

struct edge_buffer_unit {
    long long size;
    int r, c;
    int is_prefetched_normal;
    struct list_head list;
};

struct edge_buffer {
    struct list_head head;
    struct mutex lock;
    long long size, capacity;
    long long total_access_cnt, hit_cnt;

    // Todo: execution time composition to a new header file
    long long edge_proc_time, edge_internal_io_time, edge_external_io_time;
};

extern char *cache_eviction_policy;
extern int partial_edge_eviction;
extern int invalidation_at_future_value;
extern unsigned long edge_buffer_size;

void edge_buffer_init(struct edge_buffer *buf);
void edge_buffer_destroy(struct edge_buffer *buf);

long long access_edge_block(struct edge_buffer *buf, bool* aggregated, int r, int c, long long size, int is_prefetch);
long long evict_edge_block(struct edge_buffer *buf, bool* aggregated, struct edge_buffer_unit* inserted_unit, int is_prefetch);
void invalidate_edge_block(struct edge_buffer *buf, int r, int c);
void invalidate_edge_block_fifo(struct edge_buffer *buf);
long long get_edge_block_size(struct edge_buffer *buf, int r, int c);
bool lower(struct edge_buffer_unit *unit, struct edge_buffer_unit *evict_unit, bool* aggregated);

#endif