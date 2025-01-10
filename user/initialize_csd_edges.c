#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <sys/stat.h>

#define NVME_DEVICE "/dev/nvme0n1"
#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total

// Function prototypes
int open_nvme_device(const char *device_path);
void *allocate_dma_buffer(size_t size);
int setup_nvme_command(struct nvme_user_io *io, void *buffer, __u8 opcode, __u64 slba);
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
int setup_nvme_command(struct nvme_user_io *io, void *buffer, __u8 opcode, __u64 slba) {
    memset(io, 0, sizeof(*io));
    io->opcode = opcode;  // 0x01 for write, 0x02 for read
    io->flags = 0;
    io->nblocks = NUM_SECTORS - 1;  // 0-based count
    io->rsvd = 0;
    io->metadata = 0;
    io->addr = (unsigned long)buffer;
    io->slba = slba;  // Starting logical block address
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

// Cleanup resources
void cleanup(int fd, void *buffer) {
    if (buffer) {
        free(buffer);
    }
    if (fd >= 0) {
        close(fd);
    }
}

long getFileSize(const char *filename) {
    struct stat fileStat;
    if (stat(filename, &fileStat) != 0) {
        perror("Error getting file stats");
        return -1;
    }
    return fileStat.st_size;
}

int main() 
{
    char filename[50];
    int fd, ret;
    void *buffer;
    struct nvme_user_io io;
    size_t buffer_size = SECTOR_SIZE * NUM_SECTORS;

    // Open NVMeVirt device
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

    // Graph Dataset: Ex, LiveJournal
    const int num_partition = 8;
    const int num_vertices = 4847571;
    const int vertex_size = 4;
    const int edge_size = 8;    //Unweighted

    int src_vertices_slba = 0;
    int dst_vertices_slba = src_vertices_slba + num_vertices * vertex_size;
    int outdegree_slba = dst_vertices_slba + num_vertices * vertex_size;

    // Read outdegree and write 4KB buffer outdegree into nvme virtual device
    sprintf(filename, "../LiveJournal.pl/outdegrees");
    FILE *file = fopen(filename, "rb");
    int total_size = 0;
    while(fread(buffer, 1, buffer_size, file) > 0){
        ret = setup_nvme_command(&io, buffer, 0x01, (outdegree_slba + total_size) / SECTOR_SIZE);  // Setup write command
        if (ret < 0) {
            cleanup(fd, buffer);
            return -1;
        }
        ret = nvme_io_submit(fd, &io);
        if (ret < 0) {
            cleanup(fd, buffer);
            return -1;
        }
        total_size += buffer_size;
    }
    printf("outdegree size: %d(KB)\n", total_size / 1024);

    int edge_blocks_slba[num_partition][num_partition];
    int edge_blocks_length[num_partition][num_partition];
    int edge_block_base_slba = outdegree_slba + total_size;

    // Todo: read edge blocks and write 4KB buffer outdegree into nvme virtual device
    for(int i = 0; i < num_partition; i++){
        for(int j = 0; j < num_partition; j++)
        {
            sprintf(filename, "../LiveJournal.pl/block-%d-%d", i, j);
            FILE *file = fopen(filename, "rb");
            // printf("File: %s\n", filename);
            int u, v;
            int total_size = 0;
            while (fread(buffer, 1, buffer_size, file) > 0) {
                ret = setup_nvme_command(&io, buffer, 0x01, (edge_block_base_slba + total_size) / SECTOR_SIZE);  // Setup write command
                if (ret < 0) {
                    cleanup(fd, buffer);
                    return -1;
                }
                ret = nvme_io_submit(fd, &io);
                if (ret < 0) {
                    cleanup(fd, buffer);
                    return -1;
                }
                total_size += buffer_size;
            }
            edge_blocks_slba[i][j] = edge_block_base_slba;
            edge_blocks_length[i][j] = getFileSize(filename);
            edge_block_base_slba += total_size;
            printf("Size of edge block %s: %dB, %dB, SLBA: %llu\n", filename, total_size, edge_blocks_length[i][j], edge_blocks_slba[i][j]);
        }
    }

    // test the edge block
    memset(buffer, 0, sizeof(buffer));
    ret = setup_nvme_command(&io, buffer, 0x02, edge_blocks_slba[5][5] / SECTOR_SIZE);  // Setup read command
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }
    ret = nvme_io_submit(fd, &io);
    if (ret < 0) {
        cleanup(fd, buffer);
        return -1;
    }
    for(int i = 0; i < buffer_size; i += edge_size){
        int u = *(int*)(buffer + i);
        int v = *(int*)(buffer + i + vertex_size);
        printf("%d %d\n", u, v);
    }
    cleanup(fd, buffer);
    return 0;
}