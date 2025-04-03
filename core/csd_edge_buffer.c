#include "csd_edge_buffer.h"

void edge_buffer_init(struct edge_buffer *buf)
{
    INIT_LIST_HEAD(&buf->head);
    mutex_init(&buf->lock);
    buf->size = 0;
    buf->capacity = edge_buffer_size;
    buf->hit_cnt = buf->total_access_cnt = 0;

    printk(KERN_INFO "Edge buffer size: %lld", buf->capacity);
    printk(KERN_INFO "Cache eviction policy: %s", cache_eviction_policy);
    printk(KERN_INFO "Partial eviction?: %d", partial_edge_eviction);
    printk(KERN_INFO "Invalidation at future value?: %d", partial_edge_eviction);
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
    long long curr_size;
    if(size == 0)
        return 0;
    
    curr_size = get_edge_block_size(buf, r, c);
    if(curr_size == -1){
        struct edge_buffer_unit *unit;
        // Edge block not in cache
        evict_edge_block(buf, size);
        unit = kmalloc(sizeof(struct edge_buffer_unit), GFP_KERNEL);
        if (!unit) {
            pr_err("Failed to allocate memory for new csd edge buffer unit\n");
            return -1;
        }
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
        if(partial_edge_eviction){
            evict_edge_block(buf, size - curr_size);
        }
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

        if(strcmp(cache_eviction_policy, "LIFO") == 0){
            unit = list_last_entry(&buf->head, struct edge_buffer_unit, list);
        }
        else if(strcmp(cache_eviction_policy, "FIFO") == 0){
            unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
        }
        else{
            unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
        }

        if(partial_edge_eviction){
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
        }
        else{
            buf->size -= unit->size;
            list_del(&unit->list);
            kfree(unit);
        }
    }
}

void invalidate_edge_block_fifo(struct edge_buffer *buf)
{
    struct edge_buffer_unit *unit;
    if(list_empty(&buf->head))
        return;
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
            // printk(KERN_INFO "Remove edge-block-%u-%u, size: %lld", r, c, unit->size);
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
