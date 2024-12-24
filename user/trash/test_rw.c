#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/types.h>
#include <linux/nvme_ioctl.h>

#include "../nvme.h"

#define NVME_DEVICE "/dev/nvme0n1"
#define BUFFER_SIZE 4096
#define TEST_NSID 1


/**
 * Get the physical address of a virtual address.
 * @param virtual_address The virtual address to translate.
 * @return The physical address, or 0 on failure.
 */
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

int perform_nvme_write(int fd, void *buffer, __le16 size, __le64 slba) {
    struct nvme_rw_command cmd = {0};

    printf("Write virtual address: %llx\n", (__le64)buffer);
    __le64 pa = get_physical_address(buffer);
    printf("Write physical address: %llx\n", pa);
    
    cmd.opcode = nvme_cmd_write; // Write command
    cmd.flags = 0;
    cmd.command_id = 0;
    cmd.nsid = TEST_NSID;
    cmd.metadata = 0;
    cmd.prp1 = pa;
    cmd.prp2 = 0;
    cmd.slba = slba;
    cmd.length = (size / 512) - 1; // Convert to number of 512-byte sectors
    cmd.control = 0;
    cmd.dsmgmt = 0;
    cmd.reftag = 0;
    cmd.apptag = 0;
    cmd.appmask = 0;

    // Use NVME_IOCTL_IO_CMD for the write operation
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
        perror("Write operation failed");
        return -1;
    }
    
    return 0;
}

int perform_nvme_read(int fd, void *buffer, __le16 size, __le64 slba) {
    struct nvme_rw_command cmd = {0};

    printf("Read virtual address: %llx\n", (__le64)buffer);
    __le64 pa = get_physical_address(buffer);
    printf("Read physical address: %llx\n", pa);
    
    cmd.opcode = nvme_cmd_read; // Read command
    cmd.flags = 0;
    cmd.command_id = 0;
    cmd.nsid = TEST_NSID;
    cmd.metadata = 0;
    cmd.prp1 = pa;
    cmd.prp2 = 0;
    cmd.slba = slba;
    cmd.length = (size / 512) - 1; // Convert to number of 512-byte sectors
    cmd.control = 0;
    cmd.dsmgmt = 0;
    cmd.reftag = 0;
    cmd.apptag = 0;
    cmd.appmask = 0;

    // Use NVME_IOCTL_IO_CMD for the read operation
    if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd) < 0) {
        perror("Read operation failed");
        return -1;
    }
    
    return 0;
}

int main() {
    int fd;
    void *buffer;
    const char *test_data = "Hello, NVMe Virtual Device!";
    
    // Open the NVMe device
    fd = open(NVME_DEVICE, O_RDWR | O_DIRECT);
    if (fd < 0) {
        perror("Failed to open NVMe device");
        return -1;
    }

    // Allocate aligned buffer for DMA
    if (posix_memalign(&buffer, 4096, BUFFER_SIZE)) {
        perror("Failed to allocate aligned memory");
        close(fd);
        return -1;
    }

    // Clear the buffer
    memset(buffer, 0, BUFFER_SIZE);
    
    // Copy test data to buffer
    strncpy(buffer, test_data, strlen(test_data));

    printf("Writing data to NVMe device...\n");
    if (perform_nvme_write(fd, buffer, BUFFER_SIZE, 0) < 0) {
        free(buffer);
        close(fd);
        return -1;
    }
    printf("Write data: %s\n", (char *)buffer);

    // Clear the buffer before reading
    memset(buffer, 0, BUFFER_SIZE);
    printf("\n");

    sleep(5);

    printf("Reading data from NVMe device...\n");
    if (perform_nvme_read(fd, buffer, BUFFER_SIZE, 0) < 0) {
        free(buffer);
        close(fd);
        return -1;
    }

    printf("Read data: %s\n", (char *)buffer);

    // Clean up
    free(buffer);
    close(fd);

    return 0;
}