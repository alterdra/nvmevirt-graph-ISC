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
#include <time.h>

#include "../core/proc_edge_struct.h"
#include "hmb_mmap.h"

#define SECTOR_SIZE 512
#define NUM_SECTORS 8  // 4KB total
#define PAGE_SIZE 4096
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

// Vertex data and aggregation info
struct hmb_device hmb_dev = {0};

// Edge Processing;
int outdegree_slba;
int*** edge_blocks_slba;     // edge_blocks_slba[num_partitions][num_partitions][num_csds]
int*** edge_blocks_length;   // edge_blocks_length[num_partitions][num_partitions][num_csds]

// Aggregation latency
const int aggregation_read_time = 10000;
const int aggregation_write_time = 10000;

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

int max(int x, int y){
    return x > y ? x : y;
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

int init_csds_data(int* fd, void *buffer)
{
    /* Open NVMeVirt devices: Blocking */
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        fd[csd_id] = open_nvme_device(device[csd_id], 1);
        if (fd[csd_id] < 0) {
            return -1;
        }
    }

    /* Initialize HMB */
    if (hmb_init(&hmb_dev) < 0) {
        fprintf(stderr, "Failed to initialize HMB\n");
        return 1;
    }
    printf("HMB initialized successfully\n");
    
    int ret;
    char filename[50];
    struct nvme_user_io io;

    // Initialize all v_t values
    for(int i = 0; i < num_vertices; i++){
        hmb_dev.buf0.virt_addr[i] = 1.0;
        hmb_dev.buf1.virt_addr[i] = 0.0f;
        hmb_dev.buf2.virt_addr[i] = 0.0f;
    }

    // Read outdegree and write 4KB buffers into nvme virtual devices (csd_id)
    outdegree_slba = 0;
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
    int total_edges_saved = 0;
    for(int i = 0; i < num_partitions; i++){
        for(int j = 0; j < num_partitions; j++)
        {
            sprintf(filename, "../LiveJournal.pl/block-%d-%d", i, j);
            int edge_block_size = getFileSize(filename);
            int num_edge = edge_block_size / edge_size;
            total_edges_saved += num_edge;
            // printf("Edge block %d-%d Size: %d, Number of edges: %d\n", i, j, edge_block_size, num_edge);

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

                    // Print the edges for edge saving correctness
                    // for(int* e = buffer; e < buffer + bytes; e += edge_size / vertex_size){
                    //     int u = *e, v = *(e + 1);
                    //     if(v == 4847562)
                    //         printf("Dst[%d], Src[%d]\n", v, u);
                    // }

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

                // printf("Wrote Edge block %d-%d for CSD %d: slba: %d, size: %d, aligned size: %d\n", i, j, csd_id, edge_blocks_slba[i][j][csd_id], edge_blocks_length[i][j][csd_id], offset);
            }
        }
    }
    printf("Wrote %d edges to CSDs\n", total_edges_saved);


    // Closing the fds (Blocking I/O)
    for(int i = 0; i < num_csds; i++){
        if (fd[i] >= 0) {
            close(fd[i]);
        }
    }

    return 0;
}

int setup_nvme_csd_proc_edge_command(struct nvme_user_io *io, struct PROC_EDGE *proc_edge_struct, int is_sync) {
    memset(io, 0, sizeof(*io));
    io->opcode = 0x66;  // 0x66 for csd_proc_edge
    io->apptag = is_sync;    // 1 for sync, -1 for async
    io->addr = (unsigned long long)proc_edge_struct;
    return 0;
}

long long get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

// Graph processing utility functions
int send_proc_edge(int r, int c, int csd_id, int iter, int is_sync)
{
    struct nvme_user_io io;
    int ret;
    struct PROC_EDGE proc_edge_struct = 
    {
        .outdegree_slba = outdegree_slba,
        .edge_block_slba = edge_blocks_slba[r][c][csd_id],
        .edge_block_len = edge_blocks_length[r][c][csd_id],
        .iter = iter,
        .r = r, .c = c, .csd_id = csd_id,
        .num_partitions = num_partitions,
        .num_csds = num_csds
    };

    setup_nvme_csd_proc_edge_command(&io, &proc_edge_struct, is_sync);
    ret = nvme_io_submit(fd[csd_id], &io);
    return ret;
}

void aggr_partition(int c, int csd_id){
    bool can_aggr = false;
    do {
        can_aggr = true;
        for(int r = 0; r < num_partitions; r++){
            int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
            if(!hmb_dev.done1.virt_addr[id]){
                can_aggr = false;
                break;
            }
        }
    } while(!can_aggr);
}

void conv_partition(size_t partition_id){
    // LUMOS's get_partition_range in partition.hpp
    size_t begin, end;
    const size_t split_partition = num_vertices % num_partitions;
    const size_t partition_size = num_vertices / num_partitions + 1;
    if (partition_id < split_partition) {
        begin = partition_id * partition_size;
        end = (partition_id + 1) * partition_size;
    }
    else{
        const size_t split_point = split_partition * partition_size;
        begin = split_point + (partition_id - split_partition) * (partition_size - 1);
        end = split_point + (partition_id - split_partition + 1) * (partition_size - 1);
    }

    // Conv the values, for convergence
    for(size_t v = begin; v < end; v++)
        hmb_dev.buf1.virt_addr[v] = 0.15f + 0.85f * hmb_dev.buf1.virt_addr[v];
    
    // Notify CSD that partition c finish aggregation
    int num_pages = __ceil(begin - end, PAGE_SIZE);
    int end_time = get_time_ns() + num_pages * (aggregation_read_time + aggregation_write_time);
    while(get_time_ns() < end_time);
    hmb_dev.done_partition.virt_addr[partition_id] = true;
}

int csd_proc_edge_loop_normal(void* buffer, int num_iter)
{
    int ret;
    for(int iter = 0; iter < num_iter; iter++){
        for(int c = 0; c < num_partitions; c++){
            for(int r = 0; r < num_partitions; r++){
                for(int csd_id = 0; csd_id < num_csds; csd_id++){
                    ret = send_proc_edge(r, c, csd_id, 0, SYNC);
                    if(ret < 0){
                        cleanup(buffer);
                        return -1;
                    }
                }
            }
            conv_partition(c);
        }
        for(int v = 0; v < num_vertices; v++){
            hmb_dev.buf0.virt_addr[v] = hmb_dev.buf1.virt_addr[v];
            hmb_dev.buf1.virt_addr[v] = 0;
        }
    }

    for(int i = max(0, num_vertices - 10); i < num_vertices; i++)
        printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

    hmb_cleanup(&hmb_dev);
    printf("HMB cleaned up\n");
    return 0;
}

int csd_proc_edge_loop_grafu(void* buffer, int num_iter)
{
    int ret;
    
    for(int iter = 0; iter < num_iter; iter++)
    {
        if(iter % 2 == 0){
            for(int c = 0; c < num_partitions; c++){
                for(int r = 0; r < num_partitions; r++){
                    if(iter > 0 && c < r)
                        continue;
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        ret = send_proc_edge(r, c, csd_id, 0, SYNC);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                        if(r < c){
                            ret = send_proc_edge(r, c, csd_id, 1, SYNC);
                            if(ret < 0){
                                cleanup(buffer);
                                return -1;
                            }
                        }
                    }
                }
                conv_partition(c);

                // Diagnonal edge block
                for(int csd_id = 0; csd_id < num_csds; csd_id++){
                    ret = send_proc_edge(c, c, csd_id, 1, SYNC);
                    if(ret < 0){
                        cleanup(buffer);
                        return -1;
                    }
                }
            }
        }
        else{
            for(int c = num_partitions - 1; c >= 0; c--){
                for(int r = num_partitions - 1; r >= 0; r--){
                    if(c >= r)
                        continue;
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        ret = send_proc_edge(r, c, csd_id, 0, SYNC);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                        ret = send_proc_edge(r, c, csd_id, 1, SYNC);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                }
                conv_partition(c);
            }
        }

        // End of the iter update
        for(int v = 0; v < num_vertices; v++){
            hmb_dev.buf0.virt_addr[v] = hmb_dev.buf1.virt_addr[v];
            hmb_dev.buf1.virt_addr[v] = hmb_dev.buf2.virt_addr[v];
            hmb_dev.buf2.virt_addr[v] = 0.0;
        }
    }

    for(int i = max(0, num_vertices - 10); i < num_vertices; i++)
        printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

    hmb_cleanup(&hmb_dev);
    printf("HMB cleaned up\n");
    return 0;
}

int csd_proc_edge_loop(void *buffer, int num_iter)
{
    int ret;
    
    for(int iter = 0; iter < num_iter; iter++)
    {
        // 1. Iter: Sending ioctl command for all edge blocks
        if(iter % 2 == 0){
            for(int c = 0; c < num_partitions; c++){
                for(int r = 0; r < num_partitions; r++){
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                        if(hmb_dev.done1.virt_addr[id])
                            continue;
                        ret = send_proc_edge(r, c, csd_id, iter, ASYNC);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                }
            }
        }
        else{
            for(int c = num_partitions - 1; c >= 0; c--){
                for(int r = num_partitions - 1; r >= 0; r--){
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                        if(hmb_dev.done1.virt_addr[id])
                            continue;
                        ret = send_proc_edge(r, c, csd_id, iter, ASYNC);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                }
            }
        }

        // 2. Aggregate for each columns
        for(int c = 0; c < num_partitions; c++){
            for(int csd_id = 0; csd_id < num_csds; csd_id++)
                aggr_partition(c, csd_id);
            conv_partition(c);
        }

        // 3. End of the iter update
        for(int v = 0; v < num_vertices; v++){
            hmb_dev.buf0.virt_addr[v] = hmb_dev.buf1.virt_addr[v];
            hmb_dev.buf1.virt_addr[v] = hmb_dev.buf2.virt_addr[v];
            hmb_dev.buf2.virt_addr[v] = 0.0;
        }
        for(int c = 0; c < num_partitions; c++){
            for(int r = 0; r < num_partitions; r++){
                for(int csd_id = 0; csd_id < num_csds; csd_id++){
                    int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                    hmb_dev.done1.virt_addr[id] = hmb_dev.done2.virt_addr[id];
                    hmb_dev.done2.virt_addr[id] = false;
                }
            }
        }
        for(int c = 0; c < num_partitions; c++)
            hmb_dev.done_partition.virt_addr[c] = false;
    }

    for(int i = max(0, num_vertices - 10); i < num_vertices; i++)
        printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

    hmb_cleanup(&hmb_dev);
    printf("HMB cleaned up\n");
    
    return 0;
}

void test_sync_async(){
    int s, e;
    int total_sync = 0, total_async = 0;
    for(int i = 0; i < 10; i++){
        s = get_time_ns();
        send_proc_edge(5, 4, 0, 0, ASYNC);
        e = get_time_ns();
        total_async += e - s;
        s = get_time_ns();
        send_proc_edge(5, 4, 0, 0, SYNC);
        e = get_time_ns();
        total_sync += e - s;
    }
    printf("ASYNC avg.: %d ns\n", total_async);
    printf("SYNC avg.: %d ns\n", total_sync);
}

int main() 
{
    // Allocate buffer
    void *buffer = allocate_dma_buffer(buffer_size);
    if (!buffer) {
        cleanup(NULL);
        return -1;
    }
    init_csds_data(fd, buffer);

    // Open NVMeVirt devices: Non_Blocking
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        fd[csd_id] = open_nvme_device(device[csd_id], 0);
        if (fd[csd_id] < 0) {
            return -1;
        }
    }

    // Open NVMeVirt devices: Blocking
    // for(int csd_id = 0; csd_id < num_csds; csd_id++){
    //     fd[csd_id] = open_nvme_device(device[csd_id], 1);
    //     if (fd[csd_id] < 0) {
    //         return -1;
    //     }
    // }
    csd_proc_edge_loop(buffer, 5);
    // csd_proc_edge_loop_grafu(buffer, 5);
    // csd_proc_edge_loop_normal(buffer, 5);
    // test_sync_async();
    cleanup(buffer);
    
    return 0;
}