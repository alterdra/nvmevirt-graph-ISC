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

    /* Map third buffer */
    dev->done.size = HMB_SIZE / 2;
    dev->done.virt_addr = mmap(NULL, HMB_SIZE / 2, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE * 3);
    if (dev->done.virt_addr == MAP_FAILED) {
        perror("mmap buffer done failed");
        munmap((void*)dev->buf0.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        munmap((void*)dev->buf2.virt_addr, HMB_SIZE);
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
    if (dev->done.virt_addr)
        munmap((void*)dev->done.virt_addr, dev->done.size);
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

    /* Test write to done boolean buffer */
    printf("Writing to done boolean buffer...\n");
    for (i = 0; i < 10; i++) {
        dev.done.virt_addr[i] = i;
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

    printf("\nReading from done boolean buffer:\n");
    for (i = 0; i < 10; i++) {
        printf("done[%d] = %d\n", i, dev.done.virt_addr[i]);
    }

    /* Test large offset access */
    printf("\nTesting larger offsets...\n");
    size_t large_offset = (MB_256/sizeof(float)) - 1;

    dev.buf0.virt_addr[large_offset] = 0.97;
    dev.buf1.virt_addr[large_offset] = 0.99;
    dev.buf2.virt_addr[large_offset] = 1.98;
    dev.done.virt_addr[large_offset] = 0;
    
    printf("Offset: %ld\n", large_offset);
    printf("Last element buf0: %f\n", dev.buf0.virt_addr[large_offset]);
    printf("Last element buf1: %f\n", dev.buf1.virt_addr[large_offset]);
    printf("Last element buf2: %f\n", dev.buf2.virt_addr[large_offset]);
    printf("Last element done: %d\n", dev.done.virt_addr[large_offset]);

    /* Clean up */
    hmb_cleanup(&dev);
    printf("HMB cleaned up\n");
    return 0;
}

// int main()
// {
//     test_hmb();
// }