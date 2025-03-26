#ifndef PARAMS_H
#define PARAMS_H

// Virtual NVMe devices
#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total
#define PAGE_SIZE 4096

#define MAX_NUM_CSDS 8
#define MAX_PARTITION 128

// Integer vertex and unweighted edges
#define VERTEX_SIZE 4
#define EDGE_SIZE 8

// Latency in us
#define AGG_FLASH_READ_LATENCY 20000
#define AGG_FLASH_WRITE_LATENCY 350000
#define FLASH_READ_LATENCY 20000

// In-storage computing
#define CPU_MCU_SPEED_RATIO 10

// CSD DRAM Capacity
#define _KB 1024LL
#define _MB (_KB * _KB)
#define _GB (_KB * _KB * _KB)
#define CSD_DRAM_SIZE (32 * _MB)

// Cache eviction policy
// #define CONFIG_CSD_DRAM_LIFO
#define CONFIG_CSD_DRAM_FIFO
// #define CONFIG_CSD_DRAM_NO_EVICTION

// #define CONFIG_PARTIAL_EDGE_EVICTION
// #define CONFIG_INVALIDATION_AT_FUTURE_VALUE


#endif