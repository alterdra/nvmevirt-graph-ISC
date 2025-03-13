#ifndef CSD_DRAM_H
#define CSD_DRAM_H

#define CPU_MCU_SPEED_RATIO 10
#define MAX_PARTITION 128
#define CSD_DRAM_SIZE 1024LL * 1024 * 1024

#include <linux/list.h>
#include <linux/kernel.h>

struct edge_buffer_unit {
    long long size;
    int r, int c;
    struct list_head list;
};

struct edge_buffer {
    struct list_head head;
    struct mutex lock;
    long long size, capacity;
};

void edge_buffer_init(struct edge_buffer *buf);
void edge_buffer_destroy(struct edge_buffer *buf);

void cache_edge_block(struct edge_buffer *buf, int r, int c, int size);
void evict_edge_block_lifo(struct edge_buffer *buf, int size);
void evict_edge_block_fifo(struct edge_buffer *buf, int size);
void invalidate_edge_block_fifo(struct edge_buffer *buf, int r, int c);
int get_edge_block_size(struct edge_buffer *buf, int r, int c);

void cache_src_partition(struct edge_buffer *buf, int partition_id, int size);

#endif