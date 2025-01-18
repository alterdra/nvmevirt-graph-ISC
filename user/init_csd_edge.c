#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "../core/proc_edge_struct.h"

#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total
#define MAX_NUM_CSDS 4

// Virtual devices and file descripters
const int num_csds = 2;
const char device[MAX_NUM_CSDS][20] = {"/dev/nvme0n1", "/dev/nvme1n1"};
int fd[2] = {0};

// Graph Dataset: Ex, LiveJournal
const int num_partitions = 8;
const int num_vertices = 4847571;
const int vertex_size = 4;
const int edge_size = 8;    //Unweighted
const size_t buffer_size = SECTOR_SIZE * NUM_SECTORS;

// Edge Processing;
int src_vertices_slba;
int dst_vertices_slba;
int outdegree_slba;
int*** edge_blocks_slba;     // edge_blocks_slba[num_partitions][num_partitions][num_csds]
int*** edge_blocks_length;   // edge_blocks_length[num_partitions][num_partitions][num_csds]

int *normal_hmb, *future_hmb;

// Opens the NVMe device and returns file descriptor
int open_nvme_device(const char *device_path, int blocking) {

    int fd;
    if(blocking)
        fd = open(device_path, O_RDWR);
    else 
        fd = open(device_path, O_RDWR | O_NONBLOCK);
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
    if (mlock(buffer, size)) {
        perror("Failed to lock buffer in memory");
        free(buffer);
        return NULL;
    }
    return buffer;
}

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

int nvme_io_submit(int fd, struct nvme_user_io *io) {
    int ret = ioctl(fd, NVME_IOCTL_SUBMIT_IO, io);
    if (ret < 0) {
        perror("NVMe I/O ioctl failed");
        return -1;
    }
    return 0;
}

// Cleanup resources
void cleanup(void *buffer) 
{
    if(buffer) free(buffer);
    if(normal_hmb) {
        munlock(normal_hmb, sizeof(normal_hmb));
        free(normal_hmb);
    }
    if(future_hmb){
        munlock(future_hmb, sizeof(future_hmb));
        free(future_hmb);
    }

    for(int i = 0; i < num_csds; i++){
        if (fd[i] >= 0) {
            close(fd[i]);
        }
    }

    // Free the edge blocks metadata
    if(edge_blocks_slba){
        for (int i = 0; i < num_partitions; i++) {
            for (int j = 0; j < num_partitions; j++) {
                free(edge_blocks_slba[i][j]);
            }
            free(edge_blocks_slba[i]);
        }
        free(edge_blocks_slba);
    }
    if(edge_blocks_length){
        for (int i = 0; i < num_partitions; i++) {
            for (int j = 0; j < num_partitions; j++) {
                free(edge_blocks_length[i][j]);
            }
            free(edge_blocks_length[i]);
        }
        free(edge_blocks_length);
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

int min(int x, int y){
    return x < y ? x : y;
}

void malloc_edge_blocks_info()
{
    // Initialize 3d array for edge blocks metadata
    edge_blocks_slba = malloc(num_partitions * sizeof(int**));
    for (int i = 0; i < num_partitions; i++) {
        edge_blocks_slba[i] = malloc(num_partitions * sizeof(int*));
        for (int j = 0; j < num_partitions; j++) {
            edge_blocks_slba[i][j] = malloc(num_csds * sizeof(int)); // Adjust "num_levels" as needed
        }
    }
    edge_blocks_length = malloc(num_partitions * sizeof(int**));
    for (int i = 0; i < num_partitions; i++) {
        edge_blocks_length[i] = malloc(num_partitions * sizeof(int*));
        for (int j = 0; j < num_partitions; j++) {
            edge_blocks_length[i][j] = malloc(num_csds * sizeof(int)); // Adjust "num_levels" as needed
        }
    }
}

int __ceil(int x, int y){
    return (x + y - 1) / y;
}

int init_csds_data(int* fd, void *buffer){
    
    int ret;
    char filename[50];
    struct nvme_user_io io;

    // Vertices data are all placed in CSDs
    int total_vertex_size_aligned = __ceil(num_vertices * vertex_size, buffer_size) * buffer_size;
    src_vertices_slba = 0;
    dst_vertices_slba = src_vertices_slba + total_vertex_size_aligned;
    outdegree_slba = dst_vertices_slba + total_vertex_size_aligned;
    printf("Src Vertices SLBA: %d\n", src_vertices_slba);
    printf("Dst Vertices SLBA: %d\n", dst_vertices_slba);
    printf("Outdegree SLBA: %d\n", outdegree_slba);

    // Initialize all src vertex values to be 1 into nvme virtual devices (csd_id)
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        float* buffer_as_float = (float*)buffer;
        for (size_t i = 0; i < buffer_size / sizeof(int); i++) {
            buffer_as_float[i] = 1.0;    // For float
        }
        for(int offset = 0; offset < total_vertex_size_aligned; offset += buffer_size){
            setup_nvme_command(&io, buffer, 0x01, (src_vertices_slba + offset) / SECTOR_SIZE);  // Setup write command
            ret = nvme_io_submit(fd[csd_id], &io);
            if (ret < 0) {
                cleanup(buffer);
                return -1;
            }
        }
    }

    // Read outdegree and write 4KB buffers into nvme virtual devices (csd_id)
    sprintf(filename, "../LiveJournal.pl/outdegrees");
    int edge_block_base_slba[num_csds];
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        FILE *file = fopen(filename, "rb");
        int offset = 0;
        while(fread(buffer, 1, buffer_size, file) > 0){
            setup_nvme_command(&io, buffer, 0x01, (outdegree_slba + offset) / SECTOR_SIZE);  // Setup write command
            ret = nvme_io_submit(fd[csd_id], &io);
            if (ret < 0) {
                cleanup(buffer);
                return -1;
            }
            offset += buffer_size;
            memset(buffer, 0, buffer_size);
        }
        // Set up edge block base slba for csd i
        // For initializing edge_blocks_slba and edge_blocks_length latter
        edge_block_base_slba[csd_id] = outdegree_slba + offset;
    }

    // Read edge blocks and write 4KB buffer outdegree into nvme virtual device
    malloc_edge_blocks_info();
    for(int i = 0; i < num_partitions; i++){
        for(int j = 0; j < num_partitions; j++)
        {
            sprintf(filename, "../LiveJournal.pl/block-%d-%d", i, j);
            int edge_block_size = getFileSize(filename);
            int num_edge = edge_block_size / edge_size;
            printf("Edge block %d-%d Size: %d, Number of edges: %d\n", i, j, edge_block_size, num_edge);

            // Divide an edge block into num_csds portions
            int csd_num_edges = num_edge / num_csds;
            int csd_num_edges_remainder = num_edge % num_csds;
            for(int csd_id = 0; csd_id < num_csds; csd_id++){
                edge_blocks_length[i][j][csd_id] = edge_size * (csd_num_edges + (csd_id < csd_num_edges_remainder));
            }
            
            // Read edges from binary file "block-i-j", and write each portion to corresponding csds
            FILE *file = fopen(filename, "rb");
            for(int csd_id = 0; csd_id < num_csds; csd_id++)
            {
                int offset = 0;
                int remaining = edge_blocks_length[i][j][csd_id];
                while (remaining > 0) {
                    int bytes = min(buffer_size, remaining);
                    fread(buffer, 1, bytes, file);
                    setup_nvme_command(&io, buffer, 0x01, (edge_block_base_slba[csd_id] + offset) / SECTOR_SIZE);  // Setup write command
                    ret = nvme_io_submit(fd[csd_id], &io);
                    if (ret < 0) {
                        cleanup(buffer);
                        return -1;
                    }
                    offset += buffer_size;
                    remaining -= buffer_size;
                    memset(buffer, 0, buffer_size);
                }
                edge_blocks_slba[i][j][csd_id] = edge_block_base_slba[csd_id];
                edge_block_base_slba[csd_id] += offset;

                printf("Wrote Edge block %d-%d for CSD %d: slba: %d, size: %d, aligned size: %d\n"
                , i, j, csd_id, edge_blocks_slba[i][j][csd_id], edge_blocks_length[i][j][csd_id], offset);
            }
            printf("\n");
        }
    }
    return 0;
}

int test_edge_block(int* fd, void *buffer, int r, int c, int csd_id)
{
    int ret;
    struct nvme_user_io io;
    memset(buffer, 0, sizeof(buffer));
    int offset = 0;

    printf("Edge block %d-%d size at CSD %d: %d\n", r, c, csd_id, edge_blocks_length[r][c][csd_id]);
    while(offset < edge_blocks_length[r][c][csd_id]){
        setup_nvme_command(&io, buffer, 0x02, (edge_blocks_slba[r][c][csd_id] + offset) / SECTOR_SIZE);  // Setup read command
        ret = nvme_io_submit(fd[csd_id], &io);
        if (ret < 0) {
            cleanup(buffer);
            return -1;
        }
        int min_val = min(edge_blocks_length[r][c][csd_id] - offset, buffer_size);
        printf("-----page----------: %d bytes valid\n", min_val);
        // for(int i = 0; i < min_val; i += edge_size){
        //     int u = *(int*)(buffer + i);
        //     int v = *(int*)(buffer + i + vertex_size);
        //     printf("%d %d\n", u, v);
        // }
        offset += buffer_size;
    }
}

int setup_nvme_csd_proc_edge_command(struct nvme_user_io *io, struct PROC_EDGE *proc_edge_struct) {
    memset(io, 0, sizeof(*io));
    io->opcode = 0x66;  // 0x66 for csd_proc_edge
    io->apptag = CMD_PROC_EDGE;    // 0 for edge processing
    io->addr = (unsigned long long)proc_edge_struct;
    return 0;
}

int csd_proc_edge_loop(int* fd, void *buffer)
{
    struct nvme_user_io io;
    int ret;

    for(int iter = 0; iter < 1; iter++){
        for(int c = 0; c < num_partitions; c++){
            for(int r = 0; r < num_partitions; r++){
                for(int csd_id = 0; csd_id < num_csds; csd_id++){
                    // if(!(r == 5 && c == 1)) continue;
                    struct PROC_EDGE proc_edge_struct = 
                    {
                        .src_vertex_slba = src_vertices_slba,
                        .outdegree_slba = outdegree_slba,
                        .edge_block_slba = edge_blocks_slba[r][c][csd_id],
                        .vertex_len = vertex_size * num_vertices,
                        .outdegree_len = vertex_size * num_vertices,
                        .edge_block_len = edge_blocks_length[r][c][csd_id],
                        .version = iter,
                        .r = r, .c = c,
                    };
                    setup_nvme_csd_proc_edge_command(&io, &proc_edge_struct);
                    ret = nvme_io_submit(fd[csd_id], &io);
                    if (ret < 0) {
                        cleanup(buffer);
                        return -1;
                    }
                }
            }
        }
    }
}

int main() 
{
    void *buffer;
    struct nvme_user_io io;
    
    // Open NVMeVirt devices: Blocking
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        fd[csd_id] = open_nvme_device(device[csd_id], 1);
        if (fd[csd_id] < 0) {
            return -1;
        }
    }
    
    // Allocate buffer
    buffer = allocate_dma_buffer(buffer_size);
    if (!buffer) {
        cleanup(NULL);
        return -1;
    }

    init_csds_data(fd, buffer);
    // test_edge_block(fd, buffer, 5, 1, 0);
    for(int i = 0; i < num_csds; i++){
        if (fd[i] >= 0) {
            close(fd[i]);
        }
    }

    // Open NVMeVirt devices: Blocking
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        fd[csd_id] = open_nvme_device(device[csd_id], 0);
        if (fd[csd_id] < 0) {
            return -1;
        }
    }
    csd_proc_edge_loop(fd, buffer);
    cleanup(buffer);
    
    return 0;
}