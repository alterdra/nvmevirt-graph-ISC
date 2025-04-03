#include "csd_vertex_buffer.h"

void vertex_buffer_init(struct vertex_buffer *buf)
{
    INIT_LIST_HEAD(&buf->head);
    mutex_init(&buf->lock);
    buf->size = 0;
    buf->capacity = vertex_buffer_size;
    buf->hit_cnt = buf->total_access_cnt = 0;

    printk(KERN_INFO "Vertex buffer size: %lld", buf->capacity);
}

void vertex_buffer_destroy(struct vertex_buffer *buf)
{
    struct vertex_buffer_unit *unit, *tmp;

    list_for_each_entry_safe(unit, tmp, &buf->head, list) {
        list_del(&unit->list);
        kfree(unit);
    }
    buf->size = 0;
    buf->hit_cnt = buf->total_access_cnt = 0;
    INIT_LIST_HEAD(&buf->head);
}

long long access_partition(struct vertex_buffer *buf, int pid, int version, long long size)
{
    long long curr_size;
    if(size == 0)
        return 0;
    
    curr_size = get_partition_size(buf, pid, version);
    if(curr_size == -1)
    {
        struct vertex_buffer_unit *unit;
        evict_partition(buf, size);
        unit = kmalloc(sizeof(struct vertex_buffer_unit), GFP_KERNEL);
        if (!unit) {
            pr_err("Failed to allocate memory for new vertex buffer unit\n");
            return -1;
        }
        unit->pid = pid;
        unit->version = version;
        unit->size = size > buf->capacity ? buf->capacity : size;
        buf->size += unit->size;
        buf->total_access_cnt += size / PAGE_SIZE;
        list_add_tail(&unit->list, &buf->head);

        return size;
    }
    else{
        buf->hit_cnt += curr_size / PAGE_SIZE;
        buf->total_access_cnt += size / PAGE_SIZE;
        return size - curr_size;
    }
}

void evict_partition(struct vertex_buffer *buf, long long size) 
{   
    // FIFO cache eviction
    struct vertex_buffer_unit *unit;
    while(!list_empty(&buf->head) && buf->size + size > buf->capacity)
    {
        unit = list_first_entry(&buf->head, struct vertex_buffer_unit, list);

        // Flash write on eviction
        {
            long long end_time = ktime_get_ns() + (long long) FLASH_WRITE_LATENCY * unit->size / PAGE_SIZE;
            // printk(KERN_INFO "Vertex flash write time: %lld, pid: %d, version: %d", (long long) FLASH_WRITE_LATENCY * unit->size / PAGE_SIZE, unit->pid, unit->version);
            while(ktime_get_ns() < end_time);
        }

        buf->size -= unit->size;
        list_del(&unit->list);
        kfree(unit);
    }
}

long long get_partition_size(struct vertex_buffer *buf, int pid, int version)
{
    struct vertex_buffer_unit *unit;
    list_for_each_entry(unit, &buf->head, list) {
        if (unit->pid == pid && unit->version == version) {
            return unit->size;
        }
    }
    return -1;      // Not found
}
