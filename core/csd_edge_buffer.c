#include "csd_edge_buffer.h"

void edge_buffer_init(struct edge_buffer *buf)
{
    INIT_LIST_HEAD(&buf->head);
    mutex_init(&buf->lock);
    buf->size = 0;
    buf->capacity = edge_buffer_size;
    buf->hit_cnt = buf->total_access_cnt = 0;

    buf->prefetched_r = -1;
    buf->prefetched_c = -1;
    buf->prefetched_iter = -1;
    buf->prefetched_size = 0;
    buf->total_prefetch_cnt = buf->prefetch_hit_cnt = 0;
    buf->total_prefetch_block_cnt = buf->prefetch_block_hit_cnt = 0;

    printk(KERN_INFO "Edge buffer size: %lld", buf->capacity);
    printk(KERN_INFO "Cache eviction policy: %s", cache_eviction_policy);
    printk(KERN_INFO "Partial eviction?: %d", partial_edge_eviction);
    printk(KERN_INFO "Invalidation at future value?: %d", invalidation_at_future_value);

    // Todo: execution time composition to a new header file
    buf->edge_proc_time = buf->edge_internal_io_time = buf->edge_external_io_time = 0;
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

    buf->prefetched_r = -1;
    buf->prefetched_c = -1;
    buf->prefetched_iter = -1;
    buf->prefetched_size = 0;
    buf->total_prefetch_cnt = buf->prefetch_hit_cnt = 0;
    buf->total_prefetch_block_cnt = buf->prefetch_block_hit_cnt = 0;

    // Todo: execution time composition to a new header file
    buf->edge_proc_time = buf->edge_internal_io_time = buf->edge_external_io_time = 0;
    
    INIT_LIST_HEAD(&buf->head);
}

long long access_edge_block(struct edge_buffer *buf, bool* aggregated, int r, int c, long long size, int is_prefetch)
{
    long long curr_size;
    if(size == 0)
        return 0;
    
    // initatialize the inserted unit
    struct edge_buffer_unit *unit;
    unit = kmalloc(sizeof(struct edge_buffer_unit), GFP_KERNEL);
    if (!unit) {
        pr_err("Failed to allocate memory for existing csd edge buffer unit\n");
        return -1;
    }
    unit->r = r;
    unit->c = c;
    unit->size = size > buf->capacity ? buf->capacity : size;
    // 0: not prefetched, 1: prefetching future, 2: prefetching normal, 3: prefetching next iter normal
    // 0, 1 should be future
    unit->is_prefetched_normal = (is_prefetch == 2 || is_prefetch == 3) ? is_prefetch - 1 : 0;
    
    curr_size = get_edge_block_size(buf, r, c);
    if(curr_size == -1)
    {
        // Edge block not in cache
        long long evicted_size = evict_edge_block(buf, aggregated, unit, is_prefetch);
        if(!is_prefetch){
            buf->total_access_cnt += size / PAGE_SIZE;
        }
        else{
            unit->size = min(unit->size, evicted_size);
        }
        buf->size += unit->size;
        list_add_tail(&unit->list, &buf->head);

        return size;
    }
    // Partial (or full) edge block in cache, must be list head for FVC access pattern
    else{
        long long evicted_size = 0;
        invalidate_edge_block(buf, r, c);
        if(partial_edge_eviction){
            unit->size -= curr_size;
            evicted_size = evict_edge_block(buf, aggregated, unit, is_prefetch);
            unit->size += curr_size;
        }
        if(!is_prefetch){
            buf->hit_cnt += curr_size / PAGE_SIZE;
            buf->total_access_cnt += size / PAGE_SIZE;
        }
        else{
            unit->size = min(unit->size, curr_size + evicted_size);
        }
        buf->size += unit->size; 
        list_add_tail(&unit->list, &buf->head);
        // printk(KERN_INFO "Cache hit Processing edge-block-%u-%u, size: %lld", r, c, size);

        return size - curr_size;
    }
}

long long evict_edge_block(struct edge_buffer *buf, bool* aggregated, struct edge_buffer_unit* inserted_unit, int is_prefetch) 
{
    struct edge_buffer_unit *unit;
    long long size = inserted_unit->size;
    while(!list_empty(&buf->head) && buf->size + size > buf->capacity)
    {

        if(strcmp(cache_eviction_policy, "LIFO") == 0){
            unit = list_last_entry(&buf->head, struct edge_buffer_unit, list);
        }
        else if(strcmp(cache_eviction_policy, "FIFO") == 0){
            unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
        }
        else if(strcmp(cache_eviction_policy, "PRIORITY") == 0){
            struct edge_buffer_unit *evict_unit = NULL;
            list_for_each_entry(unit, &buf->head, list) {
                if(evict_unit == NULL || lower(unit, evict_unit, aggregated)){
                    evict_unit = unit;
                }
                // Early stop if we find a unit that is not aggregated or normal task
                // if(!aggregated[unit->r] || unit->r.is_prefetched_normal){
                //     break;
                // }
            }
            unit = evict_unit;
        }
        else{
            unit = list_first_entry(&buf->head, struct edge_buffer_unit, list);
        }

        if(is_prefetch && lower(inserted_unit, unit, aggregated)){
            // If the unit is lower priority than the inserted unit, skip eviction
            break;
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
    return buf->capacity - buf->size;
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

bool lower(struct edge_buffer_unit *unit, struct edge_buffer_unit *evict_unit, bool* aggregated){
    // Check if unit < evict_unit, evict unit is earlier in the list

    // 1. LIFO for prefetched normal (2 for next iteration, lowest priority)
    if(unit->is_prefetched_normal == 2)
        return true;
    if(evict_unit->is_prefetched_normal == 2)
        return false;

    // 2. FIFO for unexecutable future edges (since we process latest future that the row is ready)
    if(evict_unit->is_prefetched_normal == 0 && !aggregated[evict_unit->r])
        return false;
    if(unit->is_prefetched_normal == 0 && !aggregated[unit->r])
        return true; 
    
    // 3. LIFO for prefetched normal
    if(unit->is_prefetched_normal == 1)
        return true;
    if(evict_unit->is_prefetched_normal == 1)
        return false;

    // 4. FIFO for executable future edges (since we process latest future that the row is ready)
    return false;   
}