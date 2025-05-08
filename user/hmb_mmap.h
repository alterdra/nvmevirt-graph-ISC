#ifndef HMB_MMAP_H
#define HMB_MMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define MB_256 (256 * 1024 * 1024UL)
#define GB_20 (20LL * 1024 * 1024 * 1024)
#define HMB_SIZE GB_20

struct hmb_buffer {
    volatile float *virt_addr;  /* Virtual address of mapped memory */
    size_t size;             /* Size of memory region */
};

struct hmb_bitmap_buffer {
    volatile bool *virt_addr;  /* Virtual address of mapped memory */
    size_t size;             /* Size of memory region */
};

struct hmb_device {
    struct hmb_buffer buf0; // v_t
    struct hmb_buffer buf1; // v_t+1
    struct hmb_buffer buf2; // v_t+2
    struct hmb_bitmap_buffer done1, done2;      // v_t+1, v_t+2 aggregation from CSDs
    struct hmb_bitmap_buffer done_partition;    // v_t+1 conv notification to CSDs
    int fd;                  /* Device file descriptor */
};

/* Initialize HMB device and map both buffers */
int hmb_init(struct hmb_device *dev);

/* Clean up HMB device */
void hmb_cleanup(struct hmb_device *dev);

int test_hmb();

#endif /* HMB_MMAP_H */
