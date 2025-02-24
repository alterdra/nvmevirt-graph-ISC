#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include "hmb_mmap.h"

int hmb_init(struct hmb_device *dev)
{
    /* Open the device */
    dev->fd = open("/dev/hmb_mem", O_RDWR);
    if (dev->fd < 0) {
        perror("open /dev/hmb_mem failed");
        return -1;
    }

    /* Map v_t between host and csds*/
    dev->buf0.size = HMB_SIZE;
    dev->buf0.virt_addr = mmap(NULL, HMB_SIZE, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, 0);
    if (dev->buf0.virt_addr == MAP_FAILED) {
        perror("mmap buffer 0 (v_t) failed");
        close(dev->fd);
        return -1;
    }

    /* Map v_t+1 */
    dev->buf1.size = HMB_SIZE;
    dev->buf1.virt_addr = mmap(NULL, HMB_SIZE, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE);
    if (dev->buf1.virt_addr == MAP_FAILED) {
        perror("mmap buffer 1 (v_t+1)failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        close(dev->fd);
        return -1;
    }

    /* Map v_t+2 */
    dev->buf2.size = HMB_SIZE;
    dev->buf2.virt_addr = mmap(NULL, HMB_SIZE, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE * 2);
    if (dev->buf2.virt_addr == MAP_FAILED) {
        perror("mmap buffer 2 (v_t+2) failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        close(dev->fd);
        return -1;
    }

    /* Map done for normal values for edge blocks */
    dev->done1.size = HMB_SIZE / 4;
    dev->done1.virt_addr = mmap(NULL, HMB_SIZE / 4, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE * 3);
    if (dev->done1.virt_addr == MAP_FAILED) {
        perror("mmap buffer done failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf2.virt_addr, HMB_SIZE);
        close(dev->fd);
        return -1;
    }

    /* Map done for future values */
    dev->done2.size = HMB_SIZE / 4;
    dev->done2.virt_addr = mmap(NULL, HMB_SIZE / 4, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE * 3 + HMB_SIZE / 4);
    if (dev->done2.virt_addr == MAP_FAILED) {
        perror("mmap buffer done failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf2.virt_addr, HMB_SIZE);
        munmap((void*)dev->done1.virt_addr, HMB_SIZE / 4);
        close(dev->fd);
        return -1;
    }

    /* Map done for a partition (v_t+1) */
    dev->done_partition.size = HMB_SIZE / 4;
    dev->done_partition.virt_addr = mmap(NULL, HMB_SIZE / 4, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE * 3 + HMB_SIZE * 2 / 4);
    if (dev->done_partition.virt_addr == MAP_FAILED) {
        perror("mmap buffer done failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf2.virt_addr, HMB_SIZE);
        munmap((void*)dev->done1.virt_addr, HMB_SIZE / 4);
        munmap((void*)dev->done2.virt_addr, HMB_SIZE / 4);
        close(dev->fd);
        return -1;
    }

    return 0;
}

void hmb_cleanup(struct hmb_device *dev)
{
    if (dev->buf0.virt_addr)
        munmap((void*)dev->buf1.virt_addr, dev->buf0.size);
    if (dev->buf1.virt_addr)
        munmap((void*)dev->buf1.virt_addr, dev->buf1.size);
    if (dev->buf2.virt_addr)
        munmap((void*)dev->buf2.virt_addr, dev->buf2.size);
    if (dev->done1.virt_addr)
        munmap((void*)dev->done1.virt_addr, dev->done1.size);
    if (dev->done2.virt_addr)
        munmap((void*)dev->done2.virt_addr, dev->done2.size);
    if (dev->done_partition.virt_addr)
        munmap((void*)dev->done_partition.virt_addr, dev->done_partition.size);
    if (dev->fd >= 0)
        close(dev->fd);
}

int test_hmb()
{
    struct hmb_device dev = {0};
    int i;

    /* Initialize HMB */
    if (hmb_init(&dev) < 0) {
        fprintf(stderr, "Failed to initialize HMB\n");
        return 1;
    }

    printf("HMB initialized successfully\n");

    /* Test write to 0-th buffer */
    printf("Writing to buffer 0...\n");
    for (i = 0; i < 10; i++) {
        dev.buf0.virt_addr[i] = (i) * 1.0;
    }

    /* Test write to first buffer */
    printf("Writing to buffer 1...\n");
    for (i = 0; i < 10; i++) {
        dev.buf1.virt_addr[i] = (i + 100) * 1.0;
    }

    /* Test write to second buffer */
    printf("Writing to buffer 2...\n");
    for (i = 0; i < 10; i++) {
        dev.buf2.virt_addr[i] = (i + 200) * 1.0;
    }

    /* Test write to done1 boolean buffer */
    printf("Writing to done1 boolean buffer...\n");
    for (i = 0; i < 10; i++) {
        dev.done1.virt_addr[i] = i;
    }

    /* Test write to done2 boolean buffer */
    printf("Writing to done2 boolean buffer...\n");
    for (i = 0; i < 10; i++) {
        dev.done2.virt_addr[i] = i;
    }

    /* Test write to done_partition boolean buffer */
    printf("Writing to done_partition boolean buffer...\n");
    for (i = 0; i < 10; i++) {
        dev.done_partition.virt_addr[i] = i;
    }


    /* Read and verify data from both buffers */
    printf("Reading from buffer 0:\n");
    for (i = 0; i < 10; i++) {
        printf("buf0[%d] = %f\n", i, dev.buf0.virt_addr[i]);
    }

    printf("Reading from buffer 1:\n");
    for (i = 0; i < 10; i++) {
        printf("buf1[%d] = %f\n", i, dev.buf1.virt_addr[i]);
    }

    printf("\nReading from buffer 2:\n");
    for (i = 0; i < 10; i++) {
        printf("buf2[%d] = %f\n", i, dev.buf2.virt_addr[i]);
    }

    printf("\nReading from done1 boolean buffer:\n");
    for (i = 0; i < 10; i++) {
        printf("done1[%d] = %d\n", i, dev.done1.virt_addr[i]);
    }

    printf("\nReading from done2 boolean buffer:\n");
    for (i = 0; i < 10; i++) {
        printf("done2[%d] = %d\n", i, dev.done2.virt_addr[i]);
    }

    printf("\nReading from done_partition boolean buffer:\n");
    for (i = 0; i < 10; i++) {
        printf("done_partition[%d] = %d\n", i, dev.done_partition.virt_addr[i]);
    }

    /* Test large offset access */
    printf("\nTesting larger offsets...\n");
    size_t large_offset1 = (MB_256/sizeof(float)) - 1;
    size_t large_offset2 = (MB_256/4/sizeof(float)) - 1;

    dev.buf0.virt_addr[large_offset1] = 0.97;
    dev.buf1.virt_addr[large_offset1] = 0.99;
    dev.buf2.virt_addr[large_offset1] = 1.98;
    dev.done1.virt_addr[large_offset2] = 1;
    dev.done2.virt_addr[large_offset2] = 1;
    dev.done_partition.virt_addr[large_offset2] = 1;
    
    printf("Last element buf0: %f\n", dev.buf0.virt_addr[large_offset1]);
    printf("Last element buf1: %f\n", dev.buf1.virt_addr[large_offset1]);
    printf("Last element buf2: %f\n", dev.buf2.virt_addr[large_offset1]);
    printf("Last element done: %d\n", dev.done1.virt_addr[large_offset2]);
    printf("Last element done: %d\n", dev.done2.virt_addr[large_offset2]);
    printf("Last element done: %d\n", dev.done_partition.virt_addr[large_offset2]);

    /* Clean up */
    hmb_cleanup(&dev);
    printf("HMB cleaned up\n");
    return 0;
}

// int main()
// {
//     test_hmb();
// }