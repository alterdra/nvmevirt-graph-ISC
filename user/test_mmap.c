#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

#define MB_256 (256 * 1024 * 1024UL)
#define HMB_SIZE MB_256

struct hmb_buffer {
    volatile float *virt_addr;  /* Virtual address of mapped memory */
    size_t size;             /* Size of memory region */
};

struct hmb_device {
    struct hmb_buffer buf1;
    struct hmb_buffer buf2;
    int fd;                  /* Device file descriptor */
};

/* Initialize HMB device and map both buffers */
static int hmb_init(struct hmb_device *dev)
{
    /* Open the device */
    dev->fd = open("/dev/hmb_mem", O_RDWR);
    if (dev->fd < 0) {
        perror("open /dev/hmb_mem failed");
        return -1;
    }

    /* Map first buffer */
    dev->buf1.size = HMB_SIZE;
    dev->buf1.virt_addr = mmap(NULL, HMB_SIZE, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, 0);
    if (dev->buf1.virt_addr == MAP_FAILED) {
        perror("mmap buffer 1 failed");
        close(dev->fd);
        return -1;
    }

    /* Map second buffer */
    dev->buf2.size = HMB_SIZE;
    dev->buf2.virt_addr = mmap(NULL, HMB_SIZE, 
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED, dev->fd, HMB_SIZE);
    if (dev->buf2.virt_addr == MAP_FAILED) {
        perror("mmap buffer 2 failed");
        munmap((void*)dev->buf1.virt_addr, HMB_SIZE);
        close(dev->fd);
        return -1;
    }

    return 0;
}

/* Clean up HMB device */
static void hmb_cleanup(struct hmb_device *dev)
{
    if (dev->buf1.virt_addr)
        munmap((void*)dev->buf1.virt_addr, dev->buf1.size);
    if (dev->buf2.virt_addr)
        munmap((void*)dev->buf2.virt_addr, dev->buf2.size);
    if (dev->fd >= 0)
        close(dev->fd);
}

int main()
{
    struct hmb_device dev = {0};
    int i;

    /* Initialize HMB */
    if (hmb_init(&dev) < 0) {
        fprintf(stderr, "Failed to initialize HMB\n");
        return 1;
    }

    printf("HMB initialized successfully\n");

    /* Test write to first buffer */
    printf("Writing to buffer 1...\n");
    for (i = 0; i < 10; i++) {
        dev.buf1.virt_addr[i] = i + 100;
    }

    /* Test write to second buffer */
    printf("Writing to buffer 2...\n");
    for (i = 0; i < 10; i++) {
        dev.buf2.virt_addr[i] = i + 200;
    }

    /* Read and verify data from both buffers */
    printf("Reading from buffer 1:\n");
    for (i = 0; i < 10; i++) {
        printf("buf1[%d] = %d\n", i, dev.buf1.virt_addr[i]);
    }

    printf("\nReading from buffer 2:\n");
    for (i = 0; i < 10; i++) {
        printf("buf2[%d] = %d\n", i, dev.buf2.virt_addr[i]);
    }

    /* Test large offset access */
    printf("\nTesting larger offsets...\n");
    size_t large_offset = (MB_256/sizeof(int)) - 1;
    dev.buf1.virt_addr[large_offset] = 0xDEADBEEF;
    dev.buf2.virt_addr[large_offset] = 0xCAFEBABE;
    
    printf("Last element buf1: 0x%X\n", dev.buf1.virt_addr[large_offset]);
    printf("Last element buf2: 0x%X\n", dev.buf2.virt_addr[large_offset]);

    /* Clean up */
    hmb_cleanup(&dev);
    printf("HMB cleaned up\n");

    return 0;
}