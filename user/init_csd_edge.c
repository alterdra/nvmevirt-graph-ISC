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
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>

#include "../core/proc_edge_struct.h"
#include "../core/params.h"
#include "hmb_mmap.h"

#define PAGE_SIZE  sysconf(_SC_PAGESIZE)

// Virtual devices and file descripters
int num_csds;
// const char device[MAX_NUM_CSDS][20] = {"/dev/nvme0n1", "/dev/nvme1n1", "/dev/nvme2n1", "/dev/nvme3n1"};
// imdt
const char device[MAX_NUM_CSDS][20] = {
    "/dev/nvme1n1", "/dev/nvme2n1", "/dev/nvme3n1", "/dev/nvme4n1",
    "/dev/nvme5n1", "/dev/nvme6n1", "/dev/nvme7n1", "/dev/nvme8n1",
    "/dev/nvme9n1", "/dev/nvme10n1", "/dev/nvme11n1", "/dev/nvme12n1",
    "/dev/nvme13n1", "/dev/nvme14n1", "/dev/nvme15n1", "/dev/nvme16n1",
    "/dev/nvme17n1", "/dev/nvme18n1", "/dev/nvme19n1", "/dev/nvme20n1",
    "/dev/nvme21n1", "/dev/nvme22n1", "/dev/nvme23n1", "/dev/nvme24n1"
};
int fd[MAX_NUM_CSDS] = {0};

// Graph Dataset: Ex, LiveJournal
char dataset_path[30];
int num_partitions;
long long num_vertices;
const size_t buffer_size = SECTOR_SIZE * NUM_SECTORS;

// Vertex data and aggregation info
struct hmb_device hmb_dev = {0};

// Edge Processing;
long long outdegree_slba;
long long*** edge_blocks_slba;     // edge_blocks_slba[num_partitions][num_partitions][num_csds]
long long*** edge_blocks_length;   // edge_blocks_length[num_partitions][num_partitions][num_csds]

// Aggregation latency
long long aggregation_time = AGG_LATENCY;

// Monitoring
long long total_aggr_time; 
int curr_edge_column_normal; //HMB size
int curr_edge_column_future;
int curr_iter;   

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
    int ret = posix_memalign(&buffer, PAGE_SIZE, size);
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

    hmb_cleanup(&hmb_dev);
    printf("HMB cleaned up\n");
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
    edge_blocks_slba = malloc(num_partitions * sizeof(long long**));
    for (int i = 0; i < num_partitions; i++) {
        edge_blocks_slba[i] = malloc(num_partitions * sizeof(long long*));
        for (int j = 0; j < num_partitions; j++) {
            edge_blocks_slba[i][j] = malloc(num_csds * sizeof(long long)); // Adjust "num_levels" as needed
        }
    }
    edge_blocks_length = malloc(num_partitions * sizeof(long long**));
    for (int i = 0; i < num_partitions; i++) {
        edge_blocks_length[i] = malloc(num_partitions * sizeof(long long*));
        for (int j = 0; j < num_partitions; j++) {
            edge_blocks_length[i][j] = malloc(num_csds * sizeof(long long)); // Adjust "num_levels" as needed
        }
    }
}

int __ceil(int x, int y){
    return (x + y - 1) / y;
}

int init_csds_data(int* fd, void *buffer)
{
    int ret;
    char filename[50];
    struct nvme_user_io io;

    // Initialize all v_t values
    for(long long i = 0; i < num_vertices * (num_csds + 1); i++){
        hmb_dev.buf0.virt_addr[i] = 0.0f;
        hmb_dev.buf1.virt_addr[i] = 0.0f;
        hmb_dev.buf2.virt_addr[i] = 0.0f;
    }
    for(long long i = 0; i < num_vertices; i++){
        hmb_dev.buf0.virt_addr[i] = 1.0;
    }
    for(int c = 0; c < num_partitions; c++){
        for(int r = 0; r < num_partitions; r++){
            for(int csd_id = 0; csd_id < num_csds; csd_id++){
                int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                hmb_dev.done1.virt_addr[id] = false;
                hmb_dev.done2.virt_addr[id] = false;
            }
        }
    }
    for(int c = 0; c < num_partitions; c++)
        hmb_dev.done_partition.virt_addr[c] = false;
    
    // printf("HMB Reset done");
    // fflush(stdout);

    // Read outdegree and write 4KB buffers into nvme virtual devices (csd_id)
    outdegree_slba = 0;
    sprintf(filename, "%s/outdegrees", dataset_path);
    long long edge_block_base_slba[num_csds];
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        FILE *file = fopen(filename, "rb");
        long long offset = 0;
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
        fclose(file);
        // Set up edge block base slba for csd i
        // For initializing edge_blocks_slba and edge_blocks_length latter
        edge_block_base_slba[csd_id] = outdegree_slba + offset;
    }

    // Read edge blocks and write 4KB buffer outdegree into nvme virtual device
    malloc_edge_blocks_info();
    long long total_edges_saved = 0;
    for(int i = 0; i < num_partitions; i++){
        for(int j = 0; j < num_partitions; j++)
        {
            sprintf(filename, "%s/block-%d-%d", dataset_path, i, j);
            long long edge_block_size = getFileSize(filename);
            long long num_edge = edge_block_size / EDGE_SIZE;
            total_edges_saved += num_edge;
            // printf("Edge block %d-%d Size: %d, Number of edges: %d\n", i, j, edge_block_size, num_edge);

            // Divide an edge block into num_csds portions
            long long csd_num_edges = num_edge / num_csds;
            int csd_num_edges_remainder = num_edge % num_csds;
            for(int csd_id = 0; csd_id < num_csds; csd_id++){
                edge_blocks_length[i][j][csd_id] = 1LL * EDGE_SIZE * (csd_num_edges + (csd_id < csd_num_edges_remainder));
            }
            
            // Read edges from binary file "block-i-j", and write each portion to corresponding csds
            FILE *file = fopen(filename, "rb");
            for(int csd_id = 0; csd_id < num_csds; csd_id++)
            {
                long long offset = 0;
                long long remaining = edge_blocks_length[i][j][csd_id];
                while (remaining > 0) {
                    int bytes = min(buffer_size, remaining);
                    fread(buffer, 1, bytes, file);

                    // Print the edges for edge saving correctness
                    // for(int* e = buffer; e < buffer + bytes; e += EDGE_SIZE / VERTEX_SIZE){
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
            fclose(file);
        }
    }
    printf("Wrote %lld edges to CSDs\n", total_edges_saved);

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
int send_proc_edge(int r, int c, int csd_id, int iter, int num_iters, int is_sync, int is_fvc, int is_prefetching, int row_overlap)
{
    struct nvme_user_io io;
    int ret;
    struct PROC_EDGE proc_edge_struct = 
    {
        .outdegree_slba = outdegree_slba,
        .edge_block_slba = edge_blocks_slba[r][c][csd_id],
        .edge_block_len = edge_blocks_length[r][c][csd_id],
        .iter = iter,
        .num_iters = num_iters,
        .is_fvc = is_fvc,
        .is_prefetching = is_prefetching,
        .row_overlap = row_overlap,
        .r = r, .c = c, .csd_id = csd_id,
        .num_partitions = num_partitions,
        .num_csds = num_csds,
        .num_vertices = num_vertices,
    };

    setup_nvme_csd_proc_edge_command(&io, &proc_edge_struct, is_sync);
    ret = nvme_io_submit(fd[csd_id], &io);
    return ret;
}

void get_partition_range(size_t partition_id, size_t *begin, size_t *end){
    // LUMOS's get_partition_range in partition.hpp
    const size_t split_partition = num_vertices % num_partitions;
    const size_t partition_size = num_vertices / num_partitions + 1;
    if (partition_id < split_partition) {
        *begin = partition_id * partition_size;
        *end = (partition_id + 1) * partition_size;
    }
    else{
        const size_t split_point = split_partition * partition_size;
        *begin = split_point + (partition_id - split_partition) * (partition_size - 1);
        *end = split_point + (partition_id - split_partition + 1) * (partition_size - 1);
    }
}

void aggr_partition(int c){
    for(int r = 0; r < num_partitions; r++){
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
            while(!hmb_dev.done1.virt_addr[id]);
        }
    }
}

void aggr_edge_block(int r, int c, bool is_normal){
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
        if(is_normal){
            while(!hmb_dev.done1.virt_addr[id]);
        }
        else{
            while(!hmb_dev.done2.virt_addr[id]);
        }
    }
}

void conv_partition(size_t partition_id){
    size_t begin, end;
    get_partition_range(partition_id, &begin, &end);

    // Add up vertex values to Host DRAM
    for(size_t v = begin; v < end; v++){
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            hmb_dev.buf1.virt_addr[v] += hmb_dev.buf1.virt_addr[v + (csd_id + 1) * num_vertices];
            hmb_dev.buf1.virt_addr[v + (csd_id + 1) * num_vertices] = 0.0;
        }
    }

    // Conv the values, for convergence
    for(size_t v = begin; v < end; v++)
        hmb_dev.buf1.virt_addr[v] = 0.15f + 0.85f * hmb_dev.buf1.virt_addr[v];
    
    // Notify CSD that partition c finish aggregation
    long long num_pages = 4LL * __ceil(end - begin, PAGE_SIZE);
    long long end_time = get_time_ns() + num_pages * (aggregation_time);
    // printf("Aggregation time span: %lld\n", num_pages * (aggregation_time));
    while(get_time_ns() < end_time);
    hmb_dev.done_partition.virt_addr[partition_id] = true;
}

void end_of_iter_waiting(){
    bool can_end_of_iter_update = false;
    do {
        can_end_of_iter_update = true;
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            if(!hmb_dev.done_partition.virt_addr[num_partitions + csd_id + 1]){
                can_end_of_iter_update = false;
                break;
            }
        }
    } while(!can_end_of_iter_update);
}

void end_of_iter_replacing()
{
    // Vertices update in Host DRAM (Mapped with CSD)
    for(long long v = 0; v < num_vertices; v++){
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

    // Notify end of iteration done in CSDs
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        hmb_dev.done_partition.virt_addr[num_partitions + csd_id + 1] = false;
    }
}

int flush_csd_dram(void* buffer)
{
    int ret;
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        hmb_dev.done2.virt_addr[csd_id] = false;
        ret = send_proc_edge(0, 0, csd_id, 0, 0, FLUSH_CSD_DRAM, false, false, false);
        if(ret < 0){
            cleanup(buffer);
            return -1;
        }
    }
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        while(!hmb_dev.done2.virt_addr[csd_id]);
    }
    return 0;
}

int csd_proc_edge_loop_normal(void* buffer, int num_iter)
{
    int ret;
    for(int iter = 0; iter < num_iter; iter++){
        for(int c = 0; c < num_partitions; c++){
            for(int r = 0; r < num_partitions; r++){
                for(int csd_id = 0; csd_id < num_csds; csd_id++){
                    ret = send_proc_edge(r, c, csd_id, iter, num_iter, SYNC, false, false, false);
                    if(ret < 0){
                        cleanup(buffer);
                        return -1;
                    }
                }
                aggr_edge_block(r, c, true);
            }
            conv_partition(c);
        }
        end_of_iter_replacing();
    }
    if(flush_csd_dram(buffer) == -1)
        return -1;

    // for(int i = max(0, num_vertices - 5); i < num_vertices; i++)
    //     printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

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
                        ret = send_proc_edge(r, c, csd_id, iter, num_iter, SYNC, false, false, false);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                    aggr_edge_block(r, c, true);
                    if(r < c && iter != num_iter - 1){
                        for(int csd_id = 0; csd_id < num_csds; csd_id++){
                            ret = send_proc_edge(r, c, csd_id, iter + 1, num_iter, SYNC, true, false, false);
                            if(ret < 0){
                                cleanup(buffer);
                                return -1;
                            }
                        }
                        aggr_edge_block(r, c, false);
                    }
                }
                conv_partition(c);

                // Diagnonal edge block
                if(iter != num_iter - 1){
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        ret = send_proc_edge(c, c, csd_id, iter + 1, num_iter, SYNC, true, false, false);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                    aggr_edge_block(c, c, false);
                }

                // Aggregate future values
                if(iter != num_iter - 1){
                    size_t begin, end;
                    get_partition_range(c, &begin, &end);
                    for(size_t v = begin; v < end; v++){
                        for(int csd_id = 0; csd_id < num_csds; csd_id++){
                            hmb_dev.buf2.virt_addr[v] += hmb_dev.buf2.virt_addr[v + (csd_id + 1) * num_vertices];
                            hmb_dev.buf2.virt_addr[v + (csd_id + 1) * num_vertices] = 0.0;
                        }
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
                        ret = send_proc_edge(r, c, csd_id, iter, num_iter, SYNC, false, false, false);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                    aggr_edge_block(r, c, true);
                    if(iter != num_iter - 1){
                        for(int csd_id = 0; csd_id < num_csds; csd_id++){
                            ret = send_proc_edge(r, c, csd_id, iter + 1, num_iter, SYNC, true, false, false);
                            if(ret < 0){
                                cleanup(buffer);
                                return -1;
                            }
                        }
                        aggr_edge_block(r, c, false);
                    }
                }
                conv_partition(c);

                // Aggregate future values
                if(iter != num_iter - 1){
                    size_t begin, end;
                    get_partition_range(c, &begin, &end);
                    for(size_t v = begin; v < end; v++){
                        for(int csd_id = 0; csd_id < num_csds; csd_id++){
                            hmb_dev.buf2.virt_addr[v] += hmb_dev.buf2.virt_addr[v + (csd_id + 1) * num_vertices];
                            hmb_dev.buf2.virt_addr[v + (csd_id + 1) * num_vertices] = 0.0;
                        }
                    }
                }
            }
        }
        end_of_iter_replacing();
    }
    if(flush_csd_dram(buffer) == -1)
        return -1;

    // for(int i = max(0, num_vertices - 5); i < num_vertices; i++)
    //     printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

    return 0;
}

int csd_proc_edge_loop_dual_queue(void *buffer, int num_iter, int is_prefetching, int row_overlap)
{
    int ret;

    // For HMB size monitoring
    curr_edge_column_normal = curr_edge_column_future = 0;
    
    for(int iter = 0; iter < num_iter; iter++)
    {
        // For HMB size monitoring
        curr_iter = iter;
        if(iter % 2 == 0){
            // 1. Iter: Sending ioctl command for all edge blocks
            for(int c = 0; c < num_partitions; c++){
                for(int r = 0; r < num_partitions; r++){
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                        if(edge_blocks_length[r][c][csd_id] == 0 && !(r == num_partitions - 1 && c == num_partitions - 1))
                            hmb_dev.done1.virt_addr[id] = true; // size = 0 && not the last task, mark as done
                        if(hmb_dev.done1.virt_addr[id])
                            continue;
                        ret = send_proc_edge(r, c, csd_id, iter, num_iter, ASYNC, false, is_prefetching, row_overlap);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                }
            }
            // 2. Aggregate for each columns
            for(int c = 0; c < num_partitions; c++){
                aggr_partition(c);
                long long end_start = get_time_ns();
                conv_partition(c);
                long long end_end = get_time_ns();
                total_aggr_time += (end_end - end_start);

                // HMB size monitoring
                curr_edge_column_normal = c;
            }

        }
        else{
            for(int c = num_partitions - 1; c >= 0; c--){
                for(int r = 0; r < num_partitions; r++){
                    for(int csd_id = 0; csd_id < num_csds; csd_id++){
                        int id = csd_id * num_partitions * num_partitions + r * num_partitions + c;
                        if(edge_blocks_length[r][c][csd_id] == 0 && !(r == num_partitions - 1 && c == 0))
                            hmb_dev.done1.virt_addr[id] = true; // No edge block, mark as done
                        if(hmb_dev.done1.virt_addr[id])
                            continue;
                        ret = send_proc_edge(r, c, csd_id, iter, num_iter, ASYNC, false, is_prefetching, row_overlap);
                        if(ret < 0){
                            cleanup(buffer);
                            return -1;
                        }
                    }
                }
            }
            for(int c = num_partitions - 1; c >= 0; c--){
                aggr_partition(c);
                long long end_start = get_time_ns();
                conv_partition(c);
                long long end_end = get_time_ns();
                total_aggr_time += (end_end - end_start);

                // HMB size monitoring
                curr_edge_column_normal = c;
            }
        }
        // 3. End of the iter update
        // Performed after last column aggregation end (e.g. Ensuring CSD are notified)
        // long long end_start = get_time_ns();
        end_of_iter_waiting();
        end_of_iter_replacing();
        // long long end_end = get_time_ns();
        // printf("End-of-iter update time: %lld us\n", (end_end - end_start) / 1000);
    }
    if(flush_csd_dram(buffer) == -1)
        return -1;

    // for(int i = max(0, num_vertices - 5); i < num_vertices; i++)
    //     printf("Vertex[%d]: %f\n", i, hmb_dev.buf0.virt_addr[i]);

    return 0;
}

void run_normal_grafu_dq(void* buffer, int __num_iter){
    
    long long s, e;
    int ms_ns_ratio = 1000000;
    
    printf("Normal-----------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_normal(buffer, __num_iter);
    e = get_time_ns();
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);

    printf("Grafu-----------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_grafu(buffer, __num_iter);
    e = get_time_ns();
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);

    printf("DQ--------------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, 0, 0);
    e = get_time_ns();
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);

    printf("DQ_PF-----------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, 2, 2);
    e = get_time_ns();
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
}

void run_dq_prefetch(void* buffer, int __num_iter)
{
    float cache_hit_rate;
    long long s, e;
    int ms_ns_ratio = 1000000;

    printf("DQ--------------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, false, false);
    e = get_time_ns();
    cache_hit_rate = 0.0;
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        cache_hit_rate += hmb_dev.buf2.virt_addr[csd_id];
    }
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
    printf("Avg. cache hit rate: %f\n", cache_hit_rate / num_csds);

    printf("DQ_PF-----------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, true, false);
    e = get_time_ns();
    cache_hit_rate = 0.0;
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        cache_hit_rate += hmb_dev.buf2.virt_addr[csd_id];
    }
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
    printf("Avg. cache hit rate: %f\n", cache_hit_rate / num_csds);
}

void run_dq_prefetch_priorities(void* buffer, int __num_iter)
{
    float cache_hit_rate;
    long long s, e;
    int ms_ns_ratio = 1000000;

    printf("DQ_PF-----------");
    init_csds_data(fd, buffer);
    s = get_time_ns();
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, 2, 2);
    e = get_time_ns();
    cache_hit_rate = 0.0;
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        cache_hit_rate += hmb_dev.buf2.virt_addr[csd_id];
    }
    printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
    printf("Avg. cache hit rate: %f\n", cache_hit_rate / num_csds);

    for(int i = 4; i <= 7; i++){
        float cnt = 0.0;
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            cnt += hmb_dev.buf2.virt_addr[csd_id + num_csds * i];
        }
        printf("Priority %d: %lld\n", i - 2, (long long)cnt / num_csds);
    }
}

void run_dq_row_overlap(void* buffer, int __num_iter)
{
    float cache_hit_rate;
    long long s, e;
    int ms_ns_ratio = 1000000;
    char prefetching_str[4][40] = {"No Prefetch", "Prefetch Current Edge", "Prefetch All"};
    char row_overlap_str[3][40] = {"No Row Overlap", "Row Overlap Front", "Row Overlap Back"};

    for(int prefetching = 0; prefetching <= 0; prefetching++)
    {
        for(int row_overlap = 0; row_overlap <= 2; row_overlap++)
        {
            printf("DQ, %s, %s------", prefetching_str[prefetching], row_overlap_str[row_overlap]);
            init_csds_data(fd, buffer);
            s = get_time_ns();
            csd_proc_edge_loop_dual_queue(buffer, __num_iter, prefetching, row_overlap);
            e = get_time_ns();
            cache_hit_rate = 0.0;
            for(int csd_id = 0; csd_id < num_csds; csd_id++){
                cache_hit_rate += hmb_dev.buf2.virt_addr[csd_id];
            }
            printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
            printf("Avg. cache hit rate: %f\n", cache_hit_rate / num_csds);
        }
        printf("\n");
    }
}

void run_dq_cache_hitrate(void* buffer, int __num_iter)
{
    float cache_hit_rate = 0.0;
    int experiment_num = 1;
    long long total_time = 0;
    long long s, e;
    int ms_ns_ratio = 1000000;

    for(int i = 0; i < experiment_num; i++)
    {
        printf("DQ--------------");
        if(init_csds_data(fd, buffer) == -1){
            printf("Init CSD edge data failed");
            break;
        }

        s = get_time_ns();
        csd_proc_edge_loop_dual_queue(buffer, __num_iter, false, false);
        e = get_time_ns();
        printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
        total_time += (e - s) / ms_ns_ratio;

        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            cache_hit_rate += hmb_dev.buf2.virt_addr[csd_id];
            printf("%f ", hmb_dev.buf2.virt_addr[csd_id]);
        }
        printf("\n");
    }
    printf("Avg. execution time: %lld ms\n", total_time / experiment_num);
    printf("Avg. cache hit rate: %f\n", cache_hit_rate / num_csds / experiment_num);
}

void run_dq_composition(void* buffer, int __num_iter, int prefetching)
{
    int experiment_num = 1;
    long long total_time = 0;
    long long s, e;
    int ms_ns_ratio = 1000000;

    for(int i = 0; i < experiment_num; i++){
        printf("DQ--------------");
        if(init_csds_data(fd, buffer) == -1){
            printf("Init CSD edge data failed");
            break;
        }

        s = get_time_ns();
        csd_proc_edge_loop_dual_queue(buffer, __num_iter, prefetching, 2);
        e = get_time_ns();
        printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
        total_time += (e - s) / ms_ns_ratio;

        long long edge_proc_time = 0, edge_internal_io_time = 0, edge_external_io_time = 0;
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            // Already being divided by ms_ns_ratio in kernel modules
            edge_proc_time += hmb_dev.buf2.virt_addr[csd_id + num_csds];
            edge_internal_io_time += hmb_dev.buf2.virt_addr[csd_id + num_csds * 2];
            edge_external_io_time += hmb_dev.buf2.virt_addr[csd_id + num_csds * 3];
        }
        printf("Avg. edge processing time: %lld ms\n", edge_proc_time / num_csds);
        printf("Avg. edge IO time (Internal I/O): %lld ms\n", edge_internal_io_time / num_csds);
        printf("Avg. vertex IO time (External I/O): %lld ms\n", edge_external_io_time / num_csds);
        printf("Total aggregation time: %lld ms\n", num_csds == 1 ? 0 : total_aggr_time / ms_ns_ratio);
        printf("\n");
    }
}

void run_all_composition(void* buffer, int __num_iter)
{
    long long total_time = 0;
    long long s, e;
    int ms_ns_ratio = 1000000;

    for(int i = 0; i < 4; i++){
        if(i == 0){
            if(init_csds_data(fd, buffer) == -1){
                printf("Init CSD edge data failed");
                break;
            }
            printf("Normal-----------");
            s = get_time_ns();
            csd_proc_edge_loop_normal(buffer, __num_iter);
            e = get_time_ns();
        }
        else if(i == 1){
            if(init_csds_data(fd, buffer) == -1){
                printf("Init CSD edge data failed");
                break;
            }
            printf("Grafu-----------");
            s = get_time_ns();
            csd_proc_edge_loop_grafu(buffer, __num_iter);
            e = get_time_ns();
        }
        else if(i == 2){
            if(init_csds_data(fd, buffer) == -1){
                printf("Init CSD edge data failed");
                break;
            }
            printf("DQ--------------");
            s = get_time_ns();
            csd_proc_edge_loop_dual_queue(buffer, __num_iter, false, 0);
            e = get_time_ns();
        }
        else{
            if(init_csds_data(fd, buffer) == -1){
                printf("Init CSD edge data failed");
                break;
            }
            printf("DQ_PF-----------");
            s = get_time_ns();
            csd_proc_edge_loop_dual_queue(buffer, __num_iter, true, 0);
            e = get_time_ns();
        }
        
        printf("Execution time: %lld ms\n", (e - s) / ms_ns_ratio);
        total_time += (e - s) / ms_ns_ratio;

        long long edge_proc_time = 0, edge_internal_io_time = 0, edge_external_io_time = 0;
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            // Already being divided by ms_ns_ratio in kernel modules
            edge_proc_time += hmb_dev.buf2.virt_addr[csd_id + num_csds];
            edge_internal_io_time += hmb_dev.buf2.virt_addr[csd_id + num_csds * 2];
            edge_external_io_time += hmb_dev.buf2.virt_addr[csd_id + num_csds * 3];
        }
        printf("Avg. edge processing time: %lld ms\n", edge_proc_time / num_csds);
        printf("Avg. edge IO time (Internal I/O): %lld ms\n", edge_internal_io_time / num_csds);
        printf("Avg. vertex IO time (External I/O): %lld ms\n", edge_external_io_time / num_csds);
        printf("\n");
    }
}

volatile atomic_bool monitor_running = true;
void* monitor_window_size(void*) {
    while (atomic_load(&monitor_running)) {
        long long max_partition_offset = (long long)(num_csds + 2) * num_vertices;
        int csd_column_normal, csd_column_future;
        csd_column_normal = curr_iter % 2 == 1 ? 0 : num_partitions - 1;
        for(int csd_id = 0; csd_id < num_csds; csd_id++){
            if(curr_iter % 2 == 0)
                csd_column_normal = min(csd_column_normal, hmb_dev.buf1.virt_addr[max_partition_offset + csd_id]);
            else
                csd_column_normal = max(csd_column_normal, hmb_dev.buf1.virt_addr[max_partition_offset + csd_id]);
            // max_csd_column_future = max(max_csd_column_future, hmb_dev.buf2.virt_addr[max_partition_offset + csd_id]);
        }
        printf("Host aggr: %d, CSD normal: %d\n", curr_edge_column_normal, csd_column_normal);
        usleep(100 * 1000); // 100 milliseconds
    }
    return NULL;
}

void run_dq_hmb_size(void* buffer, int __num_iter)
{
    printf("DQ--------------");
    if(init_csds_data(fd, buffer) == -1){
        printf("Init CSD edge data failed");
        return;
    }

    pthread_t monitor_thread;
    if (pthread_create(&monitor_thread, NULL, monitor_window_size, NULL) != 0) {
        perror("Failed to create monitoring thread");
        return;
    }
    csd_proc_edge_loop_dual_queue(buffer, __num_iter, true, false);
    atomic_store(&monitor_running, false);
    pthread_join(monitor_thread, NULL);
}

int main(int argc, char* argv[]) 
{
    if (argc<4) {
		fprintf(stderr, "usage: ./init_csd_edge [dataset_path] [num_csds] [num_iters] [aggregation_time: optional]\n");
		exit(-1);
	}
    strcpy(dataset_path, argv[1]);
    char meta_path[50];
    sprintf(meta_path, "%s/meta", dataset_path);
    num_csds = atoi(argv[2]);
    int __num_iter = atoi(argv[3]);
    if(argc >= 5)
        aggregation_time = atoi(argv[4]);

    // Initialize graph dataset metadata
    FILE * fin_meta = fopen(meta_path, "r");
    long tmp[3];
    fscanf(fin_meta, "%ld %lld %ld %d %ld", &tmp[0], &num_vertices, &tmp[1], &num_partitions, &tmp[2]);
    fclose(fin_meta);
    
    // Allocate buffer
    void *buffer = allocate_dma_buffer(buffer_size);
    if (!buffer) {
        cleanup(NULL);
        return -1;
    }

    // Open NVMeVirt devices: Blocking
    for(int csd_id = 0; csd_id < num_csds; csd_id++){
        fd[csd_id] = open_nvme_device(device[csd_id]);
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

    printf("num iter: %d, num csds: %d, num vertices: %lld\n", __num_iter, num_csds, num_vertices);

    total_aggr_time = 0;
    // run_normal_grafu_dq(buffer, __num_iter);
    // run_dq_cache_hitrate(buffer, __num_iter);
    // run_dq_composition(buffer, __num_iter, 2);
    run_dq_hmb_size(buffer, __num_iter);
    // run_dq_prefetch(buffer, __num_iter);
    // run_dq_row_overlap(buffer, __num_iter);
    // run_dq_prefetch_priorities(buffer, __num_iter);
    // run_all_composition(buffer, __num_iter);
    
    cleanup(buffer);
    
    return 0;
}