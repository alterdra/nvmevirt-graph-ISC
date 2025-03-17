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

    list_for_each_entry_safe(unit, tmp, &buf->head, list) {
        list_del(&unit->list);
        kfree(unit);
    }
    buf->size = 0;
    buf->hit_cnt = buf->total_access_cnt = 0;
    INIT_LIST_HEAD(&buf->head);
}

long long access_edge_block(struct edge_buffer *buf, int r, int c, long long size)
{
    if(size == 0)
        return 0;
    
    long long curr_size = get_edge_block_size(buf, r, c);
    if(curr_size == -1){
        // Edge block not in cache
        evict_edge_block(buf, size);
        struct edge_buffer_unit *unit = kmalloc(sizeof(struct edge_buffer_unit), GFP_KERNEL);
        unit->r = r;
        unit->c = c;
        unit->size = size > buf->capacity ? buf->capacity : size;
        buf->size += unit->size;
        buf->total_access_cnt += size / PAGE_SIZE;
        list_add_tail(&unit->list, &buf->head);

        return size;
    }
    // Partial (or full) edge block in cache, must be list head for FVC access pattern
    else{
#ifdef CONFIG_PARTIAL_EDGE_EVICTION
        evict_edge_block(buf, size - curr_size);
#endif
        buf->hit_cnt += curr_size / PAGE_SIZE;
        buf->total_access_cnt += size / PAGE_SIZE;
        // printk(KERN_INFO "Cache hit Processing edge-block-%u-%u, size: %lld", r, c, size);

        return size - curr_size;
    }
}

void evict_edge_block(struct edge_buffer *buf, long long size) 
{
    struct edge_buffer_unit *unit;
    while(!list_empty(&buf->head) && buf->size + size > buf->capacity)
    {

#ifdef CONFIG_CSD_DRAM_LIFO
        unit = list_last_entry(&buf->head, struct edge_buffer_unit, list);
#endif
#ifdef CONFIG_CSD_DRAM_FIFO
        unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
#endif

#ifdef CONFIG_PARTIAL_EDGE_EVICTION
        if(buf->size - unit->size + size >= buf->capacity){
            buf->size -= unit->size;
            list_del(&unit->list);
            kfree(unit);
        }
        else{
            // Partial evict the edge block
            long long diff = buf->size + size - buf->capacity;
            unit->size -= diff;
            buf->size -= diff;
            break;
        }
#else
        buf->size -= unit->size;
        list_del(&unit->list);
        kfree(unit);
#endif
    }
}

void invalidate_edge_block_fifo(struct edge_buffer *buf)
{
    if(list_empty(&buf->head))
        return;

    struct edge_buffer_unit *unit;
    unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
    buf->size -= unit->size;
    list_del(&unit->list);
    kfree(unit);
}

void invalidate_edge_block(struct edge_buffer * buf, int r, int c)
{
    struct edge_buffer_unit *unit;
    list_for_each_entry(unit, &buf->head, list) {
        if (unit->r == r && unit->c == c) {
            printk(KERN_INFO "Remove edge-block-%u-%u, size: %lld", r, c, unit->size);
            buf->size -= unit->size;
            list_del(&unit->list);
            kfree(unit);
            return;
        }
    }
}

long long get_edge_block_size(struct edge_buffer *buf, int r, int c)
{
    struct edge_buffer_unit *unit;
    list_for_each_entry(unit, &buf->head, list) {
        if (unit->r == r && unit->c == c) {
            return unit->size;
        }
    }
    return -1;      // Not found
}
