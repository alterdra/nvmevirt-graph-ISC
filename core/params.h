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
#define FLASH_READ_LATENCY 20000
#define FLASH_WRITE_LATENCY 350000

// In-storage computing
#define CPU_MCU_SPEED_RATIO 10

// CSD DRAM Capacity
#define KB 1024LL
#define MB (KB * KB)
#define GB (KB * KB * KB)
#define CSD_DRAM_SIZE GB

#endif