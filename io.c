// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>
#include <linux/preempt.h>

// SSE
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <asm/fpu/api.h>
#include <asm/processor.h>

#include <linux/jiffies.h>

#include "nvmev.h"
#include "dma.h"

#include "core/queue.h"
#include "core/csd_edge_buffer.h"
#include "core/csd_vertex_buffer.h"
#include "core/params.h"
#include <hmb.h>

#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern bool io_using_dma;

static inline unsigned int __get_io_worker(int sqid)
{
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
	return (sqid - 1) % nvmev_vdev->config.nr_io_workers;
#else
	return nvmev_vdev->io_worker_turn;
#endif
}

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static inline size_t __cmd_io_offset(struct nvme_rw_command *cmd)
{
	return (cmd->slba) << LBA_BITS;
}

static inline size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	return (cmd->length + 1) << LBA_BITS;
}

// Printing the float values in kernel
void get_integer_and_fraction(float x, int* integer_part, int* fraction_part)
{
	*integer_part = (int)x;
	*fraction_part   = (int)((x - *integer_part) * 1000000);
}

void print_vertex_info(int csd_id, int* outdegree, int u, int v)
{
	unsigned int i_src, f_src, i_dst, f_dst;
	get_integer_and_fraction(hmb_dev.buf0.virt_addr[u], &i_src, &f_src);
	get_integer_and_fraction(hmb_dev.buf1.virt_addr[v], &i_dst, &f_dst);
	NVMEV_INFO("[CSD %d] src_vtx[%d]: %u.%06u, outdegree[%d]: %d, dst_vtx[%d]: %u.%06u\n", csd_id, u, i_src, f_src, u, outdegree[u], v, i_dst, f_dst);
}

void __proc_edge(struct PROC_EDGE task, float* dst, float* src, bool* done)
{
	int csd_id = task.csd_id;
	int num_vertices = task.num_vertices;

	int* storage = nvmev_vdev->ns[task.nsid].mapped;
	int* outdegree = storage + task.outdegree_slba / VERTEX_SIZE;
	int* e = storage + task.edge_block_slba / VERTEX_SIZE;
	int* e_end = e + task.edge_block_len / VERTEX_SIZE;
	
	long long start_time, end_time;
	long long hmb_offset, max_partition_offset;
	int u = -1, v = -1, id;

	// Update the maximum partition for hmb window
	max_partition_offset = (long long)(task.num_csds + 2) * num_vertices + csd_id;
	dst[max_partition_offset] = task.c;

	// Process the edges
	hmb_offset = (long long)(csd_id + 1) * num_vertices;
	start_time = ktime_get_ns();
	for(; e < e_end; e += EDGE_SIZE / VERTEX_SIZE) {	
		u = *e, v = *(e + 1);
		dst[v + hmb_offset] += src[u] / outdegree[u];
	}
	end_time = ktime_get_ns();

	// NVMEV_INFO("gg1: %x %x %x %lld", storage, outdegree, e_end, (long long)(csd_id + 1) * num_vertices);

	// Compensation for MCU lower frequency
	end_time = end_time + (end_time - start_time) * (CPU_MCU_SPEED_RATIO - 1);
	while(ktime_get_ns() < end_time){
		if (kthread_should_stop())
			return;
	}
	
	// For task.csd_id, Edge task.r, task.c is finished
	id = task.csd_id * task.num_partitions * task.num_partitions + task.r * task.num_partitions + task.c;
	done[id] = 1;
}

void prefetch_edge_block(struct edge_buffer *edge_buf, struct PROC_EDGE task_prefetch, long long* edge_proc_time, int is_prefetch)
{
	unsigned long long vacent_edge_block_size, size_in_cache;
	long long edge_io_time;
	double ratio;

	if(task_prefetch.edge_block_len == 0 || *edge_proc_time <= 0)
		return;

	size_in_cache = get_edge_block_size(edge_buf, task_prefetch.r, task_prefetch.c);
	if(size_in_cache == task_prefetch.edge_block_len)
		return;
	if(size_in_cache == -1)
		size_in_cache = 0;
	
	edge_io_time = task_prefetch.nsecs_target;
	ratio = 1.0 * (*edge_proc_time) / edge_io_time;
	if(ratio > 1.0) ratio = 1.0;
	// NVMEV_INFO("Prefetching edge block %d-%d, size_in_cache: %lld, edge_block_len: %lld, edge_proc_time: %lld, edge_io_time: %lld",
	// 	task_prefetch.r, task_prefetch.c, size_in_cache, task_prefetch.edge_block_len, *edge_proc_time, edge_io_time);
	*edge_proc_time -= (long long) (edge_io_time * (1.0 * (task_prefetch.edge_block_len - size_in_cache) / task_prefetch.edge_block_len));

	size_in_cache = min(size_in_cache + (long long) (task_prefetch.edge_block_len * ratio), task_prefetch.edge_block_len);
	// NVMEV_INFO("After Prefetching edge block %d-%d, size_in_cache: %lld, edge_block_len: %lld, edge_proc_time: %lld, edge_io_time: %lld",
	// 	task_prefetch.r, task_prefetch.c, size_in_cache, task_prefetch.edge_block_len, *edge_proc_time, edge_io_time);
	access_edge_block(edge_buf, hmb_dev.done_partition.virt_addr, task_prefetch.r, task_prefetch.c, size_in_cache, is_prefetch);
}

bool find_next_future_task(struct queue *future_task_queue, bool* aggregated, struct PROC_EDGE *task, bool is_dequeue)
{
	struct queue_node *node;
	bool found = false;
	if(get_queue_size(future_task_queue) == 0)
		return false;

	mutex_lock(&future_task_queue->lock);

	if(task->row_overlap == 0)
	{
		node = list_first_entry(&future_task_queue->head, struct queue_node, list);
		*task = node->proc_edge_struct;
		found = true;
	}
	else if(task->row_overlap == 1)
	{
		list_for_each_entry(node, &future_task_queue->head, list) {
			if (aggregated[node->proc_edge_struct.r]) {
				*task = node->proc_edge_struct;
				found = true;
				break;
			}
		}
	}
	else if(task->row_overlap == 2)
	{
		list_for_each_entry_reverse(node, &future_task_queue->head, list) {
			if (aggregated[node->proc_edge_struct.r]) {
				*task = node->proc_edge_struct;
				found = true;
				break;
			}
		}
	}
	else{
		// default: dequeue the first task
		node = list_first_entry(&future_task_queue->head, struct queue_node, list);
		*task = node->proc_edge_struct;
		found = true;
	}

	if(found && node && is_dequeue)
	{
		list_del(&node->list);
		future_task_queue->size--;
		kfree(node);
	}

	mutex_unlock(&future_task_queue->lock);

	return found;
}

void __do_perform_edge_proc(void)
{
	struct queue *normal_task_queue = &(nvmev_vdev->normal_task_queue);
	struct queue *future_task_queue = &(nvmev_vdev->future_task_queue);
	struct edge_buffer *edge_buf = &nvmev_vdev->edge_buf;
	struct vertex_buffer *vertex_buf = &nvmev_vdev->vertex_buf;
	extern int invalidation_at_future_value;

	// Execution composition
	long long EXEC_START_TIME, EXEC_END_TIME;
	long long edge_proc_time, edge_io_time;

	while(get_queue_size(normal_task_queue) || get_queue_size(future_task_queue))
	{
		struct PROC_EDGE task;
		bool future_aggr_ready = false;
		
		if (kthread_should_stop())
			return;

		if(get_queue_size(future_task_queue))
		{
			get_queue_front(future_task_queue, &task);
			
			// End of the iteration update
			// Fake task for even and last iter
			if(task.iter == task.num_iters
			|| ((task.iter - 1) % 2 == 0 && task.r == task.num_partitions - 1)
			|| ((task.iter - 1) % 2 == 1 && task.r == 0))
			{
				int csd_id, num_vertices;
				long long offset, v;
				unsigned long timeout;

				// Waiting for last column aggregation end
				timeout = jiffies + msecs_to_jiffies(10000); // 10 second timeout
				while(!hmb_dev.done_partition.virt_addr[task.r]){
					if (kthread_should_stop())
						return;
					// Check if we've timed out
					if (time_after(jiffies, timeout)) {
						pr_warn("Timeout waiting for aggregation completion\n");
						break;
					}
					cpu_relax();
				}
				
				queue_dequeue(future_task_queue, &task);

				// All normal task must be done --> swap normal queue and future queue
				queue_swap(normal_task_queue, future_task_queue);
				// NVMEV_INFO("CSD %d, %s, Swap queues, Queue sizes: %d, %d", task.csd_id, __func__, get_queue_size(normal_task_queue), get_queue_size(future_task_queue));
				
				// Ensuring all CSDs are ready for end-of-iter update to avoid race condition
				hmb_dev.done_partition.virt_addr[task.num_partitions + task.csd_id + 1] = true;
				
				timeout = jiffies + msecs_to_jiffies(10000); // 10 second timeout
				while(hmb_dev.done_partition.virt_addr[task.num_partitions + task.csd_id + 1]) {
					if (kthread_should_stop())
						return;
					// Check if we've timed out
					if (time_after(jiffies, timeout)) {
						pr_warn("Timeout waiting for partition completion\n");
						break;
					}
					cpu_relax();
				}

				// End of iter vertices value update
				csd_id = task.csd_id;
				num_vertices = task.num_vertices;
				offset = (task.csd_id + 1) * task.num_vertices;
				for(v = 0; v < num_vertices; v++){
				    hmb_dev.buf1.virt_addr[v + offset] = hmb_dev.buf2.virt_addr[v + offset];
				    hmb_dev.buf2.virt_addr[v + offset] = 0.0;
				}
			}

			// To use a row of edges that are aggregated to overlap
			// If the row_overlap == 1 or 2
			if(task.row_overlap && get_queue_size(future_task_queue)){
				struct queue_node *node;
				bool found = false;
				mutex_lock(&future_task_queue->lock);
				list_for_each_entry(node, &future_task_queue->head, list) {
					if (hmb_dev.done_partition.virt_addr[node->proc_edge_struct.r]) {
						found = true;
						break;
					}
				}
				if(found && node){
					task = node->proc_edge_struct;
				}
				mutex_unlock(&future_task_queue->lock);
			}
			// Future task ready
			future_aggr_ready = hmb_dev.done_partition.virt_addr[task.r];
		}
		
		if(future_aggr_ready && get_queue_size(future_task_queue))
		{
			long long end_time, size_not_in_cache;
			unsigned long long size_in_cache;
			double ratio;
			long long partition_size;
			int num_vertices;

			// We must find the next future task, because future_aggr_ready
			find_next_future_task(future_task_queue, hmb_dev.done_partition.virt_addr, &task, true);
			NVMEV_INFO("(CSD %d Future Queue) Prefetched-%d-%d-iter-%d, Processing-%d-%d-iter-%d",
				task.csd_id,
				edge_buf->prefetched_r, edge_buf->prefetched_c, edge_buf->prefetched_iter,
				task.r, task.c, task.iter);
			
			num_vertices = task.num_vertices;
		
		EXEC_START_TIME = ktime_get_ns();
			__proc_edge(task, hmb_dev.buf2.virt_addr, hmb_dev.buf1.virt_addr, hmb_dev.done2.virt_addr);
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_proc_time += (EXEC_END_TIME - EXEC_START_TIME);	
		edge_proc_time = EXEC_END_TIME - EXEC_START_TIME;
			
		EXEC_START_TIME = ktime_get_ns();
			// Edge I/O
			size_not_in_cache = access_edge_block(edge_buf, hmb_dev.done_partition.virt_addr, task.r, task.c, task.edge_block_len, false);
			if(invalidation_at_future_value){
        		invalidate_edge_block(edge_buf, task.r, task.c);
			}
			if(task.edge_block_len == 0)
				ratio = 1.0;
			else
				ratio = (1.0 * size_not_in_cache / task.edge_block_len);
			
			// Prefetch current edge block (Pipelining)
			if(task.is_prefetching >= 1)
			{
				double pipeline_ratio, prefetch_ratio;
				if(task.nsecs_target == 0)
					pipeline_ratio = 0;
				else
					pipeline_ratio = 1.0 * edge_proc_time / task.nsecs_target;

				prefetch_ratio = pipeline_ratio - ratio;
				if(prefetch_ratio < 0) prefetch_ratio = 0;

				// edge_processing_time * ratio is the time that we can prefetch the current edge block
				edge_buf->hit_cnt += min((long long) (pipeline_ratio * ratio * task.edge_block_len), size_not_in_cache) / PAGE_SIZE;
				ratio -= pipeline_ratio;
				if(ratio < 0) ratio = 0;
					
				// If the remaining edge_proc_time is not zero, we can prefetch the next edge block
				if(task.is_prefetching >= 2){
					long long tmp_edge_proc_time = (long long) (prefetch_ratio * task.nsecs_target);
					edge_buf->prefetched_r = -1;
					edge_buf->prefetched_c = -1;
					edge_buf->prefetched_iter = -1;
					if(tmp_edge_proc_time > 0){
						struct PROC_EDGE next_task;
						if(get_queue_size(future_task_queue) 
						&& find_next_future_task(future_task_queue, hmb_dev.done_partition.virt_addr, &next_task, false))
						{	
							NVMEV_INFO("Prefetch future");
							prefetch_edge_block(edge_buf, next_task, &tmp_edge_proc_time, 1);
							edge_buf->prefetched_r = next_task.r;
							edge_buf->prefetched_c = next_task.c;
							edge_buf->prefetched_iter = next_task.iter;
						}
						else if(get_queue_size(normal_task_queue))
						{
							NVMEV_INFO("Prefetch Normal");
							get_queue_front(normal_task_queue, &next_task);
							int is_prefetch = 1;
							if(task.is_fvc && !next_task.is_fvc)
								is_prefetch = 2;
							prefetch_edge_block(edge_buf, next_task, &tmp_edge_proc_time, is_prefetch);
							edge_buf->prefetched_r = next_task.r;
							edge_buf->prefetched_c = next_task.c;
							edge_buf->prefetched_iter = next_task.iter;
						}
					}
				}
			}
			
			end_time = ktime_get_ns() + (long long) (task.nsecs_target * ratio);
			// NVMEV_INFO("[CSD %d, %s(), iter: %d]: Processing edge-block-%u-%u with time span %lld, Future", task.csd_id, __func__, task.iter, task.r, task.c, (long long) (task.nsecs_target * ratio));
			while(ktime_get_ns() < end_time){
				if (kthread_should_stop())
					return;
			}
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_internal_io_time += (EXEC_END_TIME - EXEC_START_TIME);
		
		EXEC_START_TIME = ktime_get_ns();
			// Vertex parition aggregate to CSD vertex buffer
			if(task.num_partitions == 0){
				partition_size = 0;
				NVMEV_INFO("Error: partition size is zero");
			}
			else
				partition_size = (long long) num_vertices * VERTEX_SIZE / task.num_partitions;
			size_not_in_cache = access_partition(vertex_buf, task.r, task.iter, partition_size);
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_external_io_time += (EXEC_END_TIME - EXEC_START_TIME);
			
			// Fake E_00 task: for even iter end of iteration
			if(task.iter % 2 == 0 && task.r == task.num_partitions - 1 && task.c == 0){
				task.r = task.c = 0;
				queue_enqueue(future_task_queue, task);
			}
		}
		else if(get_queue_size(normal_task_queue))
		{
			long long end_time, size_not_in_cache;
			double ratio;
			long long partition_size;
			int num_vertices;

			queue_dequeue(normal_task_queue, &task);
			NVMEV_INFO("(CSD %d Normal Queue) Prefetched-%d-%d-iter-%d, Processing-%d-%d-iter-%d",
				task.csd_id,
				edge_buf->prefetched_r, edge_buf->prefetched_c, edge_buf->prefetched_iter,
				task.r, task.c, task.iter);
			num_vertices = task.num_vertices;

		EXEC_START_TIME = ktime_get_ns();
			__proc_edge(task, hmb_dev.buf1.virt_addr, hmb_dev.buf0.virt_addr, hmb_dev.done1.virt_addr);
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_proc_time += (EXEC_END_TIME - EXEC_START_TIME);	
		edge_proc_time = EXEC_END_TIME - EXEC_START_TIME;
		
		EXEC_START_TIME = ktime_get_ns();
			// Edge read I/O
			size_not_in_cache = access_edge_block(edge_buf, hmb_dev.done_partition.virt_addr, task.r, task.c, task.edge_block_len, false);
			if(invalidation_at_future_value){
        		if(task.iter == 0 && task.r > task.c || task.is_fvc)	// lower triangle
        			invalidate_edge_block(edge_buf, task.r, task.c);
			}
			if(task.edge_block_len == 0)
				ratio = 1.0;
			else
				ratio = (1.0 * size_not_in_cache / task.edge_block_len);

			// Prefetch current edge block (Pipelining)
			if(task.is_prefetching >= 1)
			{
				double pipeline_ratio, prefetch_ratio;
				if(task.nsecs_target == 0)
					pipeline_ratio = 0;
				else
					pipeline_ratio = 1.0 * edge_proc_time / task.nsecs_target;

				prefetch_ratio = pipeline_ratio - ratio;
				if(prefetch_ratio < 0) prefetch_ratio = 0;

				edge_buf->hit_cnt += min((long long) (pipeline_ratio * ratio * task.edge_block_len), size_not_in_cache) / PAGE_SIZE;
				ratio -= pipeline_ratio;
				if(ratio < 0) ratio = 0;
					
				// If the remaining edge_proc_time is not zero, we can prefetch the next edge block
				if(task.is_prefetching >= 2){
					long long tmp_edge_proc_time = (long long) prefetch_ratio * task.nsecs_target;
					edge_buf->prefetched_r = -1;
					edge_buf->prefetched_c = -1;
					edge_buf->prefetched_iter = -1;
					if(tmp_edge_proc_time > 0){
						struct PROC_EDGE next_task;
						if(get_queue_size(future_task_queue) 
						&& find_next_future_task(future_task_queue, hmb_dev.done_partition.virt_addr, &next_task, false))
						{	
							NVMEV_INFO("Prefetch future");
							prefetch_edge_block(edge_buf, next_task, &tmp_edge_proc_time, 1);
							edge_buf->prefetched_r = next_task.r;
							edge_buf->prefetched_c = next_task.c;
							edge_buf->prefetched_iter = next_task.iter;
						}
						else if(get_queue_size(normal_task_queue))
						{
							NVMEV_INFO("Prefetch Normal");
							get_queue_front(normal_task_queue, &next_task);
							int is_prefetch = 1;
							if(task.is_fvc && !next_task.is_fvc)
								is_prefetch = 2;
							prefetch_edge_block(edge_buf, next_task, &tmp_edge_proc_time, is_prefetch);
							edge_buf->prefetched_r = next_task.r;
							edge_buf->prefetched_c = next_task.c;
							edge_buf->prefetched_iter = next_task.iter;
						}
					}
				}
			}
		
			end_time = ktime_get_ns() + (long long) (task.nsecs_target * ratio);

			// NVMEV_INFO("[CSD %d, %s(), iter: %d]: Processing edge-block-%u-%u with time span %lld, size: %lld, Normal", task.csd_id, __func__, task.iter, task.r, task.c, (long long) (task.nsecs_target * ratio));
			while(ktime_get_ns() < end_time){
				if (kthread_should_stop())
					return;
			}
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_internal_io_time += (EXEC_END_TIME - EXEC_START_TIME);
		
		EXEC_START_TIME = ktime_get_ns();
			// Vertex parition DMA read
			if(task.num_partitions == 0){
				partition_size = 0;
				NVMEV_INFO("Error: partition size is zero");
			}
			else
				partition_size = (long long) num_vertices * VERTEX_SIZE / task.num_partitions;
			size_not_in_cache = access_partition(vertex_buf, task.r, task.iter, partition_size);
			end_time = ktime_get_ns() + (long long) DMA_READ_LATENCY * size_not_in_cache / PAGE_SIZE;
			// NVMEV_INFO("Partition-%d I/O time: %lld", task.c, (long long) DMA_READ_LATENCY * size_not_in_cache / PAGE_SIZE);
			while(ktime_get_ns() < end_time){
				if (kthread_should_stop())
					return;
			}
		EXEC_END_TIME = ktime_get_ns();
		edge_buf->edge_external_io_time += (EXEC_END_TIME - EXEC_START_TIME);
			
			// Insert to future task queue
			if(task.iter != task.num_iters - 1 &&
			((task.iter % 2 == 0 && task.r <= task.c) || (task.iter % 2 == 1 && task.r > task.c))){
				task.iter++;
				queue_enqueue(future_task_queue, task);
			}

			// Fake E_00 task: for last iter end of iteration
			if(task.iter == task.num_iters - 1){
				// To wait for the aggregation of P[num_partitions - 1]
				if(task.iter % 2 == 0 && task.r == task.num_partitions - 1 && task.c == task.num_partitions - 1){
					task.r = task.c = task.num_partitions - 1;
					task.iter++;
					queue_enqueue(future_task_queue, task);
				}
				// To wait for the aggregation of P[0]
				else if(task.iter % 2 == 1 && task.r == task.num_partitions - 1 && task.c == 0){
					task.r = task.c = 0;
					task.iter++;
					queue_enqueue(future_task_queue, task);
				}
			}
		}
	}
}

static unsigned int __do_perform_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t nsid = cmd->nsid - 1; // 0-based

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) +
					     (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) 
		{
			memcpy(nvmev_vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
			// NVMEV_INFO("[__do_perform_io()] [nvme_cmd_write] Prp1 address: %llx, Virt address: %llx\n", paddr, vaddr);
			// NVMEV_INFO("[%s][write] NSID: %d, Storage virt addr: %llu, IO size: %d\n", __func__, nsid, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		} 
		else if (cmd->opcode == nvme_cmd_read) 
		{
			memcpy(vaddr + mem_offs, nvmev_vdev->ns[nsid].mapped + offset, io_size);
			// NVMEV_INFO("[%s][read] NSID: %d, Storage virt addr: %llu, IO size: %d\n", __func__, nsid, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		}
		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

static u64 paddr_list[513] = {
	0,
}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	memset(paddr_list, 0, sizeof(paddr_list));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[prp_offs] = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				tmp_paddr_list = kmap_atomic_pfn(PRP_PFN(paddr_list[prp_offs])) +
						 (paddr_list[prp_offs] & PAGE_OFFSET_MASK);
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL)
		kunmap_atomic(tmp_paddr_list);

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			ioat_dma_submit(paddr, nvmev_vdev->config.storage_start + offset, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			ioat_dma_submit(nvmev_vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

static void __insert_req_sorted(unsigned int entry, struct nvmev_io_worker *worker,
				unsigned long nsecs_target)
{
	/**
	 * Requests are placed in @work_queue sorted by their target time.
	 * @work_queue is statically allocated and the ordered list is
	 * implemented by chaining the indexes of entries with @prev and @next.
	 * This implementation is nasty but we do this way over dynamically
	 * allocated linked list to minimize the influence of dynamic memory allocation.
	 * Also, this O(n) implementation can be improved to O(logn) scheme with
	 * e.g., red-black tree but....
	 */
	if (worker->io_seq == -1) {
		worker->io_seq = entry;
		worker->io_seq_end = entry;
	} else {
		unsigned int curr = worker->io_seq_end;

		while (curr != -1) {
			if (worker->work_queue[curr].nsecs_target <= worker->latest_nsecs)
				break;

			if (worker->work_queue[curr].nsecs_target <= nsecs_target)
				break;

			curr = worker->work_queue[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			worker->work_queue[worker->io_seq].prev = entry;
			worker->work_queue[entry].next = worker->io_seq;
			worker->io_seq = entry;
		} else if (worker->work_queue[curr].next == -1) { /* Tail */
			worker->work_queue[entry].prev = curr;
			worker->io_seq_end = entry;
			worker->work_queue[curr].next = entry;
		} else { /* In between */
			worker->work_queue[entry].prev = curr;
			worker->work_queue[entry].next = worker->work_queue[curr].next;

			worker->work_queue[worker->work_queue[entry].next].prev = entry;
			worker->work_queue[curr].next = entry;
		}
	}
}

static struct nvmev_io_worker *__allocate_work_queue_entry(int sqid, unsigned int *entry)
{
	unsigned int io_worker_turn = __get_io_worker(sqid);
	struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[io_worker_turn];
	unsigned int e = worker->free_seq;
	struct nvmev_io_work *w = worker->work_queue + e;

	if (w->next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		return NULL;
	}

	if (++io_worker_turn == nvmev_vdev->config.nr_io_workers)
		io_worker_turn = 0;
	nvmev_vdev->io_worker_turn = io_worker_turn;

	worker->free_seq = w->next;
	BUG_ON(worker->free_seq >= NR_MAX_PARALLEL_IO);
	*entry = e;

	return worker;
}

static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned long long nsecs_start,
			     struct nvmev_result *ret)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u[%d], sq %d cq %d, entry %d, %llu + %llu\n", worker->thread_name, entry,
		    sq_entry(sq_entry).rw.opcode, sqid, cqid, sq_entry, nsecs_start,
		    ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	w->sqid = sqid;
	w->cqid = cqid;
	w->sq_entry = sq_entry;
	w->command_id = sq_entry(sq_entry).common.command_id;
	w->nsecs_start = nsecs_start;
	w->nsecs_enqueue = local_clock();
	w->nsecs_target = ret->nsecs_target;
	w->status = ret->status;
	w->is_completed = false;
	w->is_copied = false;
	w->prev = -1;
	w->next = -1;

	w->is_internal = false;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, ret->nsecs_target);
}

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, size_t buffs_to_release)
{
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u, internal sq %d, %llu + %llu\n", worker->thread_name, entry, sqid,
		    local_clock(), nsecs_target - local_clock());

	/////////////////////////////////
	w->sqid = sqid;
	w->nsecs_start = w->nsecs_enqueue = local_clock();
	w->nsecs_target = nsecs_target;
	w->is_completed = false;
	w->is_copied = true;
	w->prev = -1;
	w->next = -1;

	w->is_internal = true;
	w->write_buffer = write_buffer;
	w->buffs_to_release = buffs_to_release;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, nsecs_target);
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < nvmev_vdev->config.nr_io_workers; turn++) {
		struct nvmev_io_worker *worker;
		struct nvmev_io_work *w;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		worker = &nvmev_vdev->io_workers[turn];

		first_entry = worker->io_seq;
		curr = first_entry;

		while (curr != -1) {
			w = &worker->work_queue[curr];
			if (w->is_completed == true && w->is_copied == true &&
			    w->nsecs_target <= worker->latest_nsecs) {
				last_entry = curr;
				curr = w->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			w = &worker->work_queue[last_entry];
			worker->io_seq = w->next;
			if (w->next != -1) {
				worker->work_queue[w->next].prev = -1;
			}
			w->next = -1;

			w = &worker->work_queue[first_entry];
			w->prev = worker->free_seq_end;

			w = &worker->work_queue[worker->free_seq_end];
			w->next = first_entry;

			worker->free_seq_end = last_entry;
			NVMEV_DEBUG_VERBOSE("%s: %u -- %u, %d\n", __func__,
					first_entry, last_entry, nr_reclaimed);
		}
	}
}

static size_t __nvmev_proc_io(int sqid, int sq_entry, size_t *io_size)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock();
	struct nvme_command *cmd = &sq_entry(sq_entry);
#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif
	struct nvmev_ns *ns = &nvmev_vdev->ns[nsid];

	struct nvmev_request req = {
		.cmd = cmd,
		.sq_id = sqid,
		.nsecs_start = nsecs_start,
	};
	struct nvmev_result ret = {
		.nsecs_target = nsecs_start,
		.status = NVME_SC_SUCCESS,
	};

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	// Graph processing task queue
	if (!ns->proc_io_cmd(ns, &req, &ret, sqid, sq_entry))
		return false;
	*io_size = __cmd_io_size(&sq_entry(sq_entry).rw);

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	// For graph processing asynchronous ioctl
	if(cmd->common.opcode != nvme_cmd_csd_process_edge){
		__enqueue_io_req(sqid, sq->cqid, sq_entry, nsecs_start, &ret);
	}

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter,
			    clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}

int nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;

	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__nvmev_proc_io(sqid, sq_entry, &io_size))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		sq->stat.total_io += io_size;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		int sqid = cq_entry(i).sq_id;
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}

		/* Should check the validity here since SPDK deletes SQ immediately
		 * before processing associated CQes */
		if (!nvmev_vdev->sqes[sqid]) continue;

		nvmev_vdev->sqes[sqid]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

static void __fill_cq_result(struct nvmev_io_work *w)
{
	int sqid = w->sqid;
	int cqid = w->cqid;
	int sq_entry = w->sq_entry;
	unsigned int command_id = w->command_id;
	unsigned int status = w->status;
	unsigned int result0 = w->result0;
	unsigned int result1 = w->result1;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int cq_head = cq->cq_head;
	struct nvme_completion *cqe = &cq_entry(cq_head);

	spin_lock(&cq->entry_lock);
	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	cqe->sq_head = sq_entry;
	cqe->status = cq->phase | (status << 1);
	cqe->result0 = result0;
	cqe->result1 = result1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_io_worker(void *data)
{
	struct nvmev_io_worker *worker = (struct nvmev_io_worker *)data;
	struct nvmev_ns *ns;
	static unsigned long last_io_time = 0;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d)\n", worker->thread_name, smp_processor_id(),
		   cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = worker->io_seq;
		int qidx;

		// Edge Processing: normal and future queue
		__do_perform_edge_proc();

		while (curr != -1) {
			struct nvmev_io_work *w = &worker->work_queue[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			worker->latest_nsecs = curr_nsecs;

			if (w->is_completed == true) {
				curr = w->next;
				continue;
			}

			if (w->is_copied == false) {
#ifdef PERF_DEBUG
				w->nsecs_copy_start = local_clock() + delta;
#endif
				if (w->is_internal) {
					;
				} else if (io_using_dma) {
					__do_perform_io_using_dma(w->sqid, w->sq_entry);
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					struct nvmev_submission_queue *sq =
						nvmev_vdev->sqes[w->sqid];
					ns = &nvmev_vdev->ns[0];
					if (ns->identify_io_cmd(ns, sq_entry(w->sq_entry))) {
						w->result0 = ns->perform_io_cmd(
							ns, &sq_entry(w->sq_entry), &(w->status));
					} else {
						__do_perform_io(w->sqid, w->sq_entry);
					}
#else 
					__do_perform_io(w->sqid, w->sq_entry);
#endif
				}

#ifdef PERF_DEBUG
				w->nsecs_copy_done = local_clock() + delta;
#endif
				w->is_copied = true;
				last_io_time = jiffies;

				NVMEV_DEBUG_VERBOSE("%s: copied %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);
			}

			if (w->nsecs_target <= curr_nsecs) {
				if (w->is_internal) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
					buffer_release((struct buffer *)w->write_buffer,
						       w->buffs_to_release);
#endif
				} else {
					__fill_cq_result(w);
				}

				NVMEV_DEBUG_VERBOSE("%s: completed %u, %d %d %d\n", worker->thread_name, curr,
					    w->sqid, w->cqid, w->sq_entry);

#ifdef PERF_DEBUG
				w->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n", w->nsecs_start,
					     w->nsecs_enqueue - w->nsecs_start,
					     w->nsecs_copy_start - w->nsecs_start,
					     w->nsecs_copy_done - w->nsecs_start,
					     w->nsecs_cq_filled - w->nsecs_start,
					     w->nsecs_target - w->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				w->is_completed = true;
			}

			curr = w->next;
		}

		for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];

#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
			if ((worker->id) != __get_io_worker(qidx))
				continue;
#endif
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						NVMEV_DEBUG("Intr %d: %llu\n", qidx,
							    intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			}
		}
		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_io_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i, worker_id;

	nvmev_vdev->io_workers =
		kcalloc(sizeof(struct nvmev_io_worker), nvmev_vdev->config.nr_io_workers, GFP_KERNEL);
	nvmev_vdev->io_worker_turn = 0;

	for (worker_id = 0; worker_id < nvmev_vdev->config.nr_io_workers; worker_id++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		worker->work_queue =
			kzalloc(sizeof(struct nvmev_io_work) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			worker->work_queue[i].next = i + 1;
			worker->work_queue[i].prev = i - 1;
		}
		worker->work_queue[NR_MAX_PARALLEL_IO - 1].next = -1;
		worker->id = worker_id;
		worker->free_seq = 0;
		worker->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		worker->io_seq = -1;
		worker->io_seq_end = -1;

		snprintf(worker->thread_name, sizeof(worker->thread_name), "nvmev_io_worker_%d", worker_id);

		worker->task_struct = kthread_create(nvmev_io_worker, worker, "%s", worker->thread_name);

		kthread_bind(worker->task_struct, nvmev_vdev->config.cpu_nr_io_workers[worker_id]);
		wake_up_process(worker->task_struct);
	}
}

void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
}
