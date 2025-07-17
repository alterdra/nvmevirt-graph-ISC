#ifndef PARAMS_H
#define PARAMS_H

// Virtual NVMe devices
#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total

#define MAX_NUM_CSDS 24
#define MAX_PARTITION 128

// Integer vertex and unweighted edges
#define VERTEX_SIZE 4
#define EDGE_SIZE 8

// Latency in us
#define AGG_LATENCY 0       // DMA write
#define DMA_READ_LATENCY 238
#define FLASH_READ_LATENCY 10000

// In-storage computing
#define CPU_MCU_SPEED_RATIO 1.5

// CSD DRAM Capacity
// #define _KB 1024LL
// #define _MB (_KB * _KB)
// #define _GB (_KB * _KB * _KB)
// #define CSD_DRAM_SIZE (32 * _MB)

// Cache eviction policy
// #define CONFIG_CSD_DRAM_LIFO
// #define CONFIG_CSD_DRAM_FIFO
// #define CONFIG_CSD_DRAM_NO_EVICTION

// #define CONFIG_PARTIAL_EDGE_EVICTION
// #define CONFIG_INVALIDATION_AT_FUTURE_VALUE


#endif