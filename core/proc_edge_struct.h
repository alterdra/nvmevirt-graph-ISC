#ifndef PROC_EDGE_H
#define PROC_EDGE_H

#include <linux/types.h> // For __u64 and __u32 definitions

// Define the PROC_EDGE struct
struct PROC_EDGE {
    __u64 src_vertex_slba;    // Source vertex starting logical block address
    __u64 outdegree_slba;     // Outdegree starting logical block address
    __u64 edge_block_slba;          // Edge starting logical block address
    // __u64 dst_vertex_addr;    // Destination vertex address
    __u32 vertex_len;    // Length of vertex data
    __u32 outdegree_len; // Length of outdegree data
    __u32 edge_block_len;      // Length of edge data
    __u32 version;            // Version of the structure

    __u32 nsid;     // Namespace id, used by kernel module
    __u32 r, c;    // Edge block id, for debugging
} __attribute__((packed));

#endif // PROC_EDGE_H