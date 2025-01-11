#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <sys/stat.h>

#define NVME_DEVICE_0 "/dev/nvme0n1"
#define NVME_DEVICE_1 "/dev/nvme0n1"

#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total

// Device file descripters
int fd[2];

// Graph Dataset: Ex, LiveJournal
const int num_partition = 8;
const int num_vertices = 4847571;
const int vertex_size = 4;
const int edge_size = 8;    //Unweighted
const size_t buffer_size = SECTOR_SIZE * NUM_SECTORS;

// Edge Processing;
int src_vertices_slba;
int dst_vertices_slba;
int outdegree_slba;
int** edge_blocks_slba;     // edge_blocks_slba[num_partition][num_partition]
int** edge_blocks_length;   // edge_blocks_length[num_partition][num_partition]

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
void cleanup(void *buffer) {
    if (buffer) {
        free(buffer);
    }
    for(int i = 0; i < 2; i++){
        if (fd[i] >= 0) {
            close(fd[i]);
        }
    }
    // Free the edge blocks metadata
    for (int i = 0; i < num_partition; i++) {
        free(edge_blocks_slba[i]);
    }
    free(edge_blocks_slba);
    for (int i = 0; i < num_partition; i++) {
        free(edge_blocks_length[i]);
    }
    free(edge_blocks_length);
}

long getFileSize(const char *filename) {
    struct stat fileStat;
    if (stat(filename, &fileStat) != 0) {
        perror("Error getting file stats");
        return -1;
    }
    return fileStat.st_size;
}

int init_csd_data(int fd, void *buffer){
    
    int ret;
    char filename[50];
    struct nvme_user_io io;

    src_vertices_slba = 0;
    dst_vertices_slba = src_vertices_slba + num_vertices * vertex_size;
    outdegree_slba = dst_vertices_slba + num_vertices * vertex_size;

    // Read outdegree and write 4KB buffer outdegree into nvme virtual device
    sprintf(filename, "../LiveJournal.pl/outdegrees");
    FILE *file = fopen(filename, "rb");
    int total_size = 0;
    while(fread(buffer, 1, buffer_size, file) > 0){
        ret = setup_nvme_command(&io, buffer, 0x01, (outdegree_slba + total_size) / SECTOR_SIZE);  // Setup write command
        if (ret < 0) {
            cleanup(buffer);
            return -1;
        }
        ret = nvme_io_submit(fd, &io);
        if (ret < 0) {
            cleanup(buffer);
            return -1;
        }
        total_size += buffer_size;
    }
    printf("outdegree size: %d(KB)\n", total_size / 1024);

    // Initialize 2d array for edge blocks
    edge_blocks_slba = malloc(num_partition * sizeof(int*));
    for (int i = 0; i < num_partition; i++) {
        edge_blocks_slba[i] = malloc(num_partition * sizeof(int));
    }
    edge_blocks_length = malloc(num_partition * sizeof(int*));
    for (int i = 0; i < num_partition; i++) {
        edge_blocks_length[i] = malloc(num_partition * sizeof(int));
    }
    
    // Read edge blocks and write 4KB buffer outdegree into nvme virtual device
    int edge_block_base_slba = outdegree_slba + total_size;
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
                    cleanup(buffer);
                    return -1;
                }
                ret = nvme_io_submit(fd, &io);
                if (ret < 0) {
                    cleanup(buffer);
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
    return 0;
}

int test_edge_block(int fd, void *buffer, int r, int c)
{
    int ret;
    struct nvme_user_io io;
    memset(buffer, 0, sizeof(buffer));
    int offset = 0;

    printf("Total cnt of bytes of edge block %d-%d: %d\n", r, c, edge_blocks_length[r][c]);
    while(offset < edge_blocks_length[r][c]){
        ret = setup_nvme_command(&io, buffer, 0x02, (edge_blocks_slba[r][c] + offset) / SECTOR_SIZE);  // Setup read command
        if (ret < 0) {
            cleanup(buffer);
            return -1;
        }
        ret = nvme_io_submit(fd, &io);
        if (ret < 0) {
            cleanup(buffer);
            return -1;
        }
        int min_val = (edge_blocks_length[r][c] - offset) < buffer_size ? (edge_blocks_length[r][c] - offset) : buffer_size;
        printf("-----page----------: %d bytes valid\n", min_val);
        for(int i = 0; i < min_val; i += edge_size){
            int u = *(int*)(buffer + i);
            int v = *(int*)(buffer + i + vertex_size);
            // printf("%d %d\n", u, v);
        }
        offset += buffer_size;
    }
}

// int csd_proc_edge_loop(int fd, void *buffer){
//     for(int r = 0; r < num_partition; r++){
//         for(int c = 0; c < num_partition; c++){

//         }
//     }
// }

int main() 
{
    void *buffer;
    struct nvme_user_io io;
    
    // Open NVMeVirt devices
    fd[0] = open_nvme_device(NVME_DEVICE_0);
    if (fd[0] < 0) {
        return -1;
    }
    fd[1] = open_nvme_device(NVME_DEVICE_1);
    if (fd[1] < 0) {
        return -1;
    }
    
    // Allocate buffer
    buffer = allocate_dma_buffer(buffer_size);
    if (!buffer) {
        cleanup(NULL);
        return -1;
    }

    for(int i = 0; i < 2; i++){
        init_csd_data(fd[i], buffer);
        test_edge_block(fd[i], buffer, 5, 1);
    }

    cleanup(buffer);
    
    return 0;
}