#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <errno.h>
#include <sys/mman.h>
#include "../nvme.h"

#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB transfer size
#define BUFFER_SIZE (SECTOR_SIZE * NUM_SECTORS)

// Structure to hold our DMA buffer information
struct dma_buffer {
    void *virt_addr;    // Virtual address for CPU access
    uint64_t phys_addr; // Physical address for device access
    size_t size;        // Size of the buffer
};

uint64_t get_physical_address(void *virtual_address) 
{
    uint64_t virtual_page = (uint64_t)virtual_address / sysconf(_SC_PAGESIZE);
    uint64_t page_offset = (uint64_t)virtual_address % sysconf(_SC_PAGESIZE);
    uint64_t physical_page;
    uint64_t physical_address = 0;

    // Open /proc/self/pagemap
    int pagemap_fd = open("/proc/self/pagemap", O_RDONLY);
    if (pagemap_fd < 0) {
        perror("Failed to open /proc/self/pagemap");
        return 0;
    }

    // Seek to the relevant entry in pagemap
    off_t offset = virtual_page * sizeof(uint64_t);
    if (lseek(pagemap_fd, offset, SEEK_SET) == (off_t)-1) {
        perror("Failed to seek in /proc/self/pagemap");
        close(pagemap_fd);
        return 0;
    }

    // Read the entry
    if (read(pagemap_fd, &physical_page, sizeof(uint64_t)) != sizeof(uint64_t)) {
        perror("Failed to read from /proc/self/pagemap");
        close(pagemap_fd);
        return 0;
    }

    close(pagemap_fd);

    // Check if the page is present in memory
    if (!(physical_page & (1ULL << 63))) {
        fprintf(stderr, "Page is not present in memory\n");
        return 0;
    }

    // Extract the physical page number (bits 0-54)
    physical_page &= ((1ULL << 54) - 1);

    // Calculate the physical address
    physical_address = (physical_page * sysconf(_SC_PAGESIZE)) + page_offset;

    return physical_address;
}

// Allocate DMA-able buffer
struct dma_buffer *allocate_dma_buffer(size_t size) {
    struct dma_buffer *buf = malloc(sizeof(struct dma_buffer));
    if (!buf) {
        return NULL;
    }

    // Allocate memory with proper alignment for DMA
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    
    if (addr == MAP_FAILED) {
        // Fallback to regular page-aligned allocation if huge pages not available
        if (posix_memalign(&addr, getpagesize(), size)) {
            free(buf);
            return NULL;
        }
        
        // Lock the memory to prevent it from being swapped
        if (mlock(addr, size) != 0) {
            free(addr);
            free(buf);
            return NULL;
        }
    }

    buf->virt_addr = addr;
    buf->size = size;
    // Note: In a real implementation, you'd need to get the actual physical address
    // This usually requires kernel support or specific hardware interfaces
    buf->phys_addr = get_physical_address(addr);  // This is simplified

    return buf;
}

// Free DMA buffer
void free_dma_buffer(struct dma_buffer *buf) {
    if (buf) {
        if (buf->virt_addr) {
            munlock(buf->virt_addr, buf->size);
            if (munmap(buf->virt_addr, buf->size) != 0) {
                free(buf->virt_addr);
            }
        }
        free(buf);
    }
}

int nvme_write(int fd, struct dma_buffer *buf, uint64_t slba, uint16_t num_sectors) {
    struct nvme_rw_command cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = nvme_cmd_write;
    cmd.nsid = 1;
    cmd.slba = slba;
    cmd.length = num_sectors - 1;
    cmd.prp1 = buf->phys_addr;  // Use physical address for DMA

    int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
    if (ret) {
        fprintf(stderr, "Write failed at LBA %lu: %s\n", slba, strerror(errno));
        return -1;
    }

    printf("Write successful: %d sectors at LBA %lu\n", num_sectors, slba);
    return 0;
}

int nvme_read(int fd, struct dma_buffer *buf, uint64_t slba, uint16_t num_sectors) {
    struct nvme_rw_command cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = nvme_cmd_read;
    cmd.nsid = 1;
    cmd.slba = slba;
    cmd.length = num_sectors - 1;
    cmd.prp1 = buf->phys_addr;  // Use physical address for DMA

    int ret = ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
    if (ret) {
        fprintf(stderr, "Read failed at LBA %lu: %s\n", slba, strerror(errno));
        return -1;
    }

    printf("Read successful: %d sectors from LBA %lu\n", num_sectors, slba);
    return 0;
}

int main() {
    int fd;
    int ret = 0;
    struct dma_buffer *buf;

    fd = open("/dev/nvme0n1", O_RDWR | O_DIRECT);
    if (fd < 0) {
        perror("Failed to open NVMe device");
        return -1;
    }

    // Allocate DMA buffer
    buf = allocate_dma_buffer(BUFFER_SIZE);
    if (!buf) {
        perror("Failed to allocate DMA buffer");
        close(fd);
        return -1;
    }
    // printf("Virt Address: %s", (char*)(uint64_t)(buf->virt_addr));
    // printf("Phys Address: %s", (char*)(uint64_t)(buf->phys_addr));

    // Prepare write data
    memset(buf->virt_addr, 0xAA, BUFFER_SIZE);

    // Write data
    ret = nvme_write(fd, buf, 0, NUM_SECTORS);
    if (ret)
        goto cleanup;

    // Clear buffer for read verification
    memset(buf->virt_addr, 0, BUFFER_SIZE);

    // Read data back
    ret = nvme_read(fd, buf, 0, NUM_SECTORS);
    if (ret)
        goto cleanup;

    // Verify data
    printf("Read Data: %d\n", ((char *)buf->virt_addr)[0]);
    if (((char *)buf->virt_addr)[0] == 0xAA) {
        printf("Data verification successful\n");
    } else {
        printf("Data verification failed\n");
        ret = -1;
    }

cleanup:
    free_dma_buffer(buf);
    close(fd);
    return ret;
}