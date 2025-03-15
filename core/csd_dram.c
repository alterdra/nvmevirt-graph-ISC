#include "csd_dram.h"

void edge_buffer_init(struct edge_buffer *buf)
{
    INIT_LIST_HEAD(&buf->head);
    mutex_init(&buf->lock);
    buf->size = 0;
    buf->capacity = CSD_DRAM_SIZE;
    buf->hit_cnt = buf->total_access_cnt = 0;
}

void edge_buffer_destroy(struct edge_buffer *buf)
{
    struct edge_buffer_unit *unit, *tmp;

    mutex_lock(&buf->lock);
    list_for_each_entry_safe(unit, tmp, &buf->head, list) {
        list_del(&unit->list);
        kfree(unit);
    }
    buf->size = 0;
    buf->hit_cnt = buf->total_access_cnt = 0;
    mutex_unlock(&buf->lock);
}

long long get_size(struct edge_buffer *buf){
    mutex_lock(&buf->lock);
    long long res = buf->size;
    mutex_unlock(&buf->lock);
    return res;
}

long long access_edge_block(struct edge_buffer *buf, int r, int c, long long size)
{
    long long curr_size = get_edge_block_size(buf, r, c);
    if(curr_size == -1){
        // Edge block not in cache
        evict_edge_block_lifo(buf, size);
        struct edge_buffer_unit *unit = kmalloc(sizeof(struct edge_buffer_unit), GFP_KERNEL);
        unit->r = r;
        unit->c = c;
        unit->size = min(buf->capacity, size);

        mutex_lock(&buf->lock);
        buf->size += unit->size;
        buf->total_access_cnt += size / PAGE_SIZE;
        list_add_tail(&unit->list, &buf->head);
        mutex_unlock(&buf->lock);

        return size;
    }
    // Partial (or full) edge block in cache, must be list head for FVC access pattern
    else{
        evict_edge_block_lifo(buf, size - curr_size);
        invalidate_edge_block_fifo(buf);

        mutex_lock(&buf->lock);
        buf->hit_cnt += curr_size / PAGE_SIZE;
        buf->total_access_cnt += size / PAGE_SIZE;
        mutex_unlock(&buf->lock);

        return size - curr_size;
    }
}

void evict_edge_block_lifo(struct edge_buffer *buf, long long size) 
{
    struct edge_buffer_unit *unit;
    while(!list_empty(&buf->head) && buf->size + size > buf->capacity)
    {
        mutex_lock(&buf->lock);
        unit = list_last_entry(&buf->head, struct edge_buffer_unit, list);
        if(buf->size - unit->size + size >= buf->capacity){
            buf->size -= unit->size;
            list_del(&unit->list);
            mutex_unlock(&buf->lock);
            kfree(unit);
        }
        else{
            // Partial evict the edge block
            long long diff = buf->size + size - buf->capacity;
            unit->size -= diff;
            buf->size -= diff;
            mutex_unlock(&buf->lock);
            break;
        }
    }
}

void invalidate_edge_block_fifo(struct edge_buffer *buf)
{
    if(list_empty(&buf->head))
        return;

    struct edge_buffer_unit *unit;
    mutex_lock(&buf->lock);
    unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
    buf->size -= unit->size;
    list_del(&unit->list);
    mutex_unlock(&buf->lock);
    kfree(unit);
}

long long get_edge_block_size(struct edge_buffer *buf, int r, int c)
{
    struct edge_buffer_unit *unit;
    mutex_lock(&buf->lock);
    list_for_each_entry(unit, &buf->head, list) {
        if (unit->r == r && unit->c == c) {
            mutex_unlock(&buf->lock);
            return unit->size;
        }
    }
    mutex_unlock(&buf->lock);
    return -1;      // Not found
}
