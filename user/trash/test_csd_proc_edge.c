#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <stdbool.h>
#include <stdint.h>

#define NVME_DEVICE "/dev/nvme0n1"
#define SECTOR_SIZE 512
#define NUM_SECTORS 24  // 4KB total

struct ADDR_PROC_EDGE 
{
    __u64 src_vertex_slba;
    __u64 outdegree_slba;
    __u64 edge_slba; 
    __u64 dst_vertex_addr;
    __u32 vertex_data_len;
    __u32 outdegree_data_len;
    __u32 edge_data_len; 

    __u32 version;
}__attribute__((packed));

// Function prototypes
int open_nvme_device(const char *device_path);
void *allocate_dma_buffer(size_t size);
int setup_nvme_command(struct nvme_user_io *io, void *buffer, __u8 opcode);
int nvme_io_submit(int fd, struct nvme_user_io *io);
int verify_data(void *buffer, size_t size, unsigned char expected);
void cleanup(int fd, void *buffer);


int64_t get_physical_address(void *virtual_address) 
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
int setup_nvme_csd_proc_edge_command(struct nvme_user_io *io, struct ADDR_PROC_EDGE *proc_edge, __u8 opcode) {
    memset(io, 0, sizeof(*io));
    io->opcode = opcode;  // 0x01 for write, 0x02 for read
    io->slba = (unsigned long long)proc_edge;
    io->addr = (unsigned long long)proc_edge;
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
    const char *test_data = "Hello, NVMe Virtual Device!";
    
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
    memset(buffer, 0, buffer_size);
    strncpy(buffer, test_data, strlen(test_data));

    // CSD i, version j
    int num_csd = 2;
    int num_partition = 3;
    int num_version = 3;
    
    int val = 1;
    bool little_endian = *(char*)(&val);
    printf("%s\n", little_endian ? "Little Endian" : "Big Endian");
    
    struct ADDR_PROC_EDGE addr_proc_edge = {1, 2, 3, 4, 5, 6, 7, 8};
    printf("addr_proc_edge virt: %llx\n", (unsigned long long)&addr_proc_edge);
    printf("addr_proc_edge physical: %llx\n", get_physical_address(&addr_proc_edge));

    printf("src_vertex_slba: %llu\n", addr_proc_edge.src_vertex_slba);
    printf("outdegree_slba: %llu\n", addr_proc_edge.outdegree_slba);
    printf("edge_slba: %llu\n", addr_proc_edge.edge_slba);
    

    // Write operation
    printf("Writing data to NVMe device...\n");
    ret = setup_nvme_csd_proc_edge_command(&io, &addr_proc_edge, 0x66);  // Setup write command
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }
    printf("io->addr->src_vertex_slba: %llu\n", ((struct ADDR_PROC_EDGE*)(io.addr))->src_vertex_slba);
    

    for(int i = 0; i < 20; i++){
        ((struct ADDR_PROC_EDGE*)(io.addr))->src_vertex_slba = i;
        ret = nvme_io_submit(fd, &io);
        if (ret < 0) {
            cleanup(fd, buffer);
            return -1;
        }
    }
    // Cleanup
    cleanup(fd, buffer);
    // sleep(2);
    return 0;
}