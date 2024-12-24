#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>

#define NVME_DEVICE "/dev/nvme0n1"
#define SECTOR_SIZE 512
#define NUM_SECTORS 24  // 4KB total

// Function prototypes
int open_nvme_device(const char *device_path);
void *allocate_dma_buffer(size_t size);
int setup_nvme_command(struct nvme_user_io *io, void *buffer, __u8 opcode);
int nvme_io_submit(int fd, struct nvme_user_io *io);
int verify_data(void *buffer, size_t size, unsigned char expected);
void cleanup(int fd, void *buffer);

// Opens the NVMe device and returns file descriptor
int open_nvme_device(const char *device_path) {
    int fd = open(device_path, O_RDWR);
    if (fd < 0) {
        perror("Failed to open NVMe device");
        return -1;
    }
    return fd;
}

// Allocates page-aligned buffer for DMA operations
void *allocate_dma_buffer(size_t size) {
    void *buffer;
    int ret = posix_memalign(&buffer, getpagesize(), size);
    if (ret) {
        perror("Failed to allocate aligned buffer");
        return NULL;
    }
    return buffer;
}

// Sets up the NVMe command structure
int setup_nvme_command(struct nvme_user_io *io, void *buffer, __u8 opcode) {
    memset(io, 0, sizeof(*io));
    io->opcode = opcode;  // 0x01 for write, 0x02 for read
    io->flags = 0;
    io->nblocks = NUM_SECTORS - 1;  // 0-based count
    io->rsvd = 0;
    io->metadata = 0;
    io->addr = (unsigned long)buffer;
    io->slba = 0;  // Starting logical block address
    io->dsmgmt = 0;
    io->reftag = 0;
    io->apptag = 0;
    io->appmask = 0;
    return 0;
}

// Performs the actual I/O operation
int nvme_io_submit(int fd, struct nvme_user_io *io) {
    int ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, io);
    if (ret < 0) {
        perror("NVMe I/O ioctl failed");
        return -1;
    }
    return 0;
}

// Verifies the data in the buffer
int verify_data(void *buffer, size_t size, unsigned char expected) {
    unsigned char *data = (unsigned char *)buffer;
    for (size_t i = 0; i < size; i++) {
        if (data[i] != expected) {
            printf("Data verification failed at offset %zu\n", i);
            return 0;
        }
    }
    return 1;
}

// Cleanup resources
void cleanup(int fd, void *buffer) {
    if (buffer) {
        free(buffer);
    }
    if (fd >= 0) {
        close(fd);
    }
}

int main() {
    int fd, ret;
    void *buffer;
    struct nvme_user_io io;
    size_t buffer_size = SECTOR_SIZE * NUM_SECTORS;
    
    // Open device
    fd = open_nvme_device(NVME_DEVICE);
    if (fd < 0) {
        return -1;
    }

    // Allocate buffer
    buffer = allocate_dma_buffer(buffer_size);
    if (!buffer) {
        cleanup(fd, NULL);
        return -1;
    }

    // Write operation
    printf("Writing data to NVMe device...\n");
    memset(buffer, 0xAA, buffer_size);  // Fill with test pattern
    
    ret = setup_nvme_command(&io, buffer, 0x01);  // Setup write command
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }

    ret = nvme_io_submit(fd, &io);
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }

    // Read operation
    printf("Reading data from NVMe device...\n");
    memset(buffer, 0, buffer_size);  // Clear buffer
    
    ret = setup_nvme_command(&io, buffer, 0x66);  // Setup read command
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }

    ret = nvme_io_submit(fd, &io);
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }

    // Verify data
    if (verify_data(buffer, buffer_size, 0xAA)) {
        printf("Data verification successful!\n");
    }

    // Cleanup
    cleanup(fd, buffer);
    return 0;
}