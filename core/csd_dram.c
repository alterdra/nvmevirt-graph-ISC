#include "csd_dram.h"

void init_edge_buffer(struct edge_buffer *buf);
void distroy_edge_buffer(struct edge_buffer *buf);

void cache_edge_block(struct edge_buffer *buf, int r, int c, int size)
{
    evict_edge_block_lifo(buf, size);

    struct edge_buffer_unit *unit = kmalloc(sizeof(struct edge_buffer_unit), GFP_KERNEL);
    unit->r = r;
    unit->c = c;
    unit->size = size;

    mutex_lock(&buf->lock);
    buf->size += size;
    list_add_tail(&unit->list, &buf->head);
    mutex_unlock(&buf->lock);
}

void evict_edge_block_lifo(struct edge_buffer_unit *buf, int size) 
{
    if (list_empty(&buf->head)) {
        return -1; // Queue is empty
    }

    struct edge_buffer_unit *unit;
    while(buf->size + size > buf->capacity)
    {
        mutex_lock(&buf->lock);
        unit = list_last_entry(&buf->head, struct edge_buffer_unit, list);
        if(buf->size - unit->size + size >= buf->capacity){
            buf->size -= unit->size;
            list_del(&unit->list);
            mutex_unlock(&buf->lock);
        }
        else{
            // Partial evict the edge block
            unit->size -= buf->size + size - buf->capacity;
            buf->size = buf->capacity - size;
            mutex_unlock(&buf->lock);
            break;
        }
    }
    kfree(unit);

    return 0; // Success
}

void invalidate_edge_block_fifo(struct edge_buffer_unit *buf)
{
    if (list_empty(&buf->head)) {
        return -1; // Queue is empty
    }

    struct edge_buffer_unit *unit;
    mutex_lock(&buf->lock);
    unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
    buf->size -= unit->size;
    list_del(&unit->list);
    mutex_unlock(&buf->lock);
    kfree(unit);

    return 0; // Success
}

int get_edge_block_size(struct edge_buffer *buf, int r, int c)
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
    return NULL;
}
