#ifndef PROC_EDGE_H
#define PROC_EDGE_H

#include <linux/types.h> // For __u64 and __u32 definitions
#define CMD_PROC_EDGE 0

struct PROC_EDGE 
{
    __u64 src_vertex_slba;
    __u64 outdegree_slba;
    __u64 edge_block_slba; 
    // __u64 dst_vertex_addr;   // HMB
    // __u32 vertex_len;
    // __u32 outdegree_len;
    __u32 edge_block_len; 
    __u32 version;

    __u32 nsid;     // Namespace id, used by kernel module

    // For aggregation to HMB done buffer
    __u32 r, c, csd_id; 
    __u32 num_partitions, num_csds;
    __u32 nsecs_target;

} __attribute__((packed));

#endif // PROC_EDGE_H