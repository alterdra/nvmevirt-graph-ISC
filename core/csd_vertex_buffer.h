#ifndef CSD_VERTEX_BUFFER_H
#define CSD_VERTEX_BUFFER_H

#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include "params.h"

struct vertex_buffer_unit {
    long long size;
    int pid, version;
    struct list_head list;
};

struct vertex_buffer {
    struct list_head head;
    struct mutex lock;
    long long size, capacity;
    long long total_access_cnt, hit_cnt;
};

extern unsigned long vertex_buffer_size;

void vertex_buffer_init(struct vertex_buffer *buf);
void vertex_buffer_destroy(struct vertex_buffer *buf);

long long access_partition(struct vertex_buffer *buf, int pid, int version, long long size);
void evict_partition(struct vertex_buffer *buf, long long size);
long long get_partition_size(struct vertex_buffer *buf, int pid, int version);

#endif