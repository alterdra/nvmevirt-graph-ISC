#ifndef CSD_DRAM_H
#define CSD_DRAM_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include "params.h"

struct edge_buffer_unit {
    long long size;
    int r, c;
    struct list_head list;
};

struct edge_buffer {
    struct list_head head;
    struct mutex lock;
    long long size, capacity;
    long long total_access_cnt, hit_cnt;
};

void edge_buffer_init(struct edge_buffer *buf);
void edge_buffer_destroy(struct edge_buffer *buf);

long long access_edge_block(struct edge_buffer *buf, int r, int c, long long size);
void evict_edge_block(struct edge_buffer *buf, long long size);
void invalidate_edge_block(struct edge_buffer *buf, int r, int c);
void invalidate_edge_block_fifo(struct edge_buffer *buf);
long long get_edge_block_size(struct edge_buffer *buf, int r, int c);



#endif