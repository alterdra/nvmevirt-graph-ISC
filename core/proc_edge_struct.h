#ifndef PROC_EDGE_H
#define PROC_EDGE_H

#include <linux/types.h> // For __u64 and __u32 definitions
#define CMD_PROC_EDGE 0
#define CMD_INIT_HMB 1

struct PROC_EDGE {
    __u64 src_vertex_slba;
    __u64 outdegree_slba;
    __u64 edge_block_slba; 
    // __u64 dst_vertex_addr;
    __u32 vertex_len;
    __u32 outdegree_len;
    __u32 edge_block_len; 
    __u32 version;

    __u32 nsid;     // Namespace id, used by kernel module
    __u32 r, c;    // Edge block id, for debugging
} __attribute__((packed));

struct HMB {
    __u64 normal_hmb_phys_addr;
    __u64 future_hmb_phys_addr;
    __u64 edge_block_finished;
};

#endif // PROC_EDGE_H