#ifndef PROC_EDGE_H
#define PROC_EDGE_H

#include <linux/types.h> // For __u64 and __u32 definitions
#define SYNC 1
#define ASYNC 2
#define FLUSH_CSD_DRAM 3

struct PROC_EDGE 
{
    __u64 outdegree_slba;
    __u64 edge_block_slba; 
    __u64 edge_block_len; 
    __u32 iter, num_iters;
    __u32 is_fvc;
    __u32 is_prefetching;
    __u32 row_overlap;

    __u32 nsid;     // Namespace id, used by kernel module

    // For aggregation to HMB done buffer
    __u32 r, c, csd_id; 
    __u32 num_partitions, num_csds;
    __u32 num_vertices;
    __u64 nsecs_target;

} __attribute__((packed));

#endif // PROC_EDGE_H