// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <linux/highmem.h>

#include <linux/sched.h>     // For task_struct and current
#include <linux/sched/signal.h> // For accessing process structures
#include <linux/preempt.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "simple_ftl.h"
#include "core/queue.h"
#include "core/csd_edge_buffer.h"
#include "core/csd_vertex_buffer.h"
#include "core/params.h"
#include <hmb.h>

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher);
}

static size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	NVMEV_DEBUG_VERBOSE("[%c] %llu + %d, prp %llx %llx\n",
			cmd->opcode == nvme_cmd_write ? 'W' : 'R', cmd->slba, cmd->length,
		    cmd->prp1, cmd->prp2);
	// NVMEV_INFO("[%c] %llu + %d, prp %llx %llx\n",
	// 		cmd->opcode == nvme_cmd_write ? 'W' : 'R', cmd->slba, cmd->length,
	// 	    cmd->prp1, cmd->prp2);

	return (cmd->length + 1) << LBA_BITS;
}

/* Return the time to complete */
static unsigned long long __schedule_io_units(int opcode, unsigned long lba, unsigned int length,
					      unsigned long long nsecs_start)
{    
	unsigned int io_unit_size = 1 << nvmev_vdev->config.io_unit_shift;
	unsigned int io_unit =
		(lba >> (nvmev_vdev->config.io_unit_shift - LBA_BITS)) % nvmev_vdev->config.nr_io_units;
	int nr_io_units = min(nvmev_vdev->config.nr_io_units, DIV_ROUND_UP(length, io_unit_size));

	unsigned long long latest; /* Time of completion */
	unsigned int delay = 0;
	unsigned int latency = 0;
	unsigned int trailing = 0;

	if (opcode == nvme_cmd_write) {
		delay = nvmev_vdev->config.write_delay;
		latency = nvmev_vdev->config.write_time;
		trailing = nvmev_vdev->config.write_trailing;
	} else if (opcode == nvme_cmd_read) {
		delay = nvmev_vdev->config.read_delay;
		latency = nvmev_vdev->config.read_time;
		trailing = nvmev_vdev->config.read_trailing;
	} else if (opcode == nvme_cmd_csd_process_edge) {
		delay = nvmev_vdev->config.read_delay;
		latency = nvmev_vdev->config.read_time;
		trailing = nvmev_vdev->config.read_trailing;
	}


	latest = max(nsecs_start, nvmev_vdev->io_unit_stat[io_unit]) + delay;

	// Seperate nvme_cmd_csd_process_edge
	// For synchronous ioctl
	if(opcode == nvme_cmd_csd_process_edge){
		do {
			latest += latency;
			length -= min(length, io_unit_size);
		} while (length > 0);
	}
	return latest;

	do {
		latest += latency;
		nvmev_vdev->io_unit_stat[io_unit] = latest;

		if (nr_io_units-- > 0) {
			nvmev_vdev->io_unit_stat[io_unit] += trailing;
		}

		length -= min(length, io_unit_size);
		if (++io_unit >= nvmev_vdev->config.nr_io_units)
			io_unit = 0;
	} while (length > 0);

	return latest;
}

static unsigned long long __schedule_flush(struct nvmev_request *req)
{
	unsigned long long latest = 0;
	int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_units; i++) {
		latest = max(latest, nvmev_vdev->io_unit_stat[i]);
	}

	return latest;
}

void __do_perform_edge_proc_grafu(struct PROC_EDGE task)
{
	int csd_id = task.csd_id;
	int num_vertices = task.num_vertices;
	struct edge_buffer *edge_buf = &nvmev_vdev->edge_buf;
	struct vertex_buffer *vertex_buf = &nvmev_vdev->vertex_buf;
	long long EXEC_START_TIME, EXEC_END_TIME;

	// Initialize the edge starting addresses
	int* storage = nvmev_vdev->ns[task.nsid].mapped;
	int* outdegree = storage + task.outdegree_slba / VERTEX_SIZE;
	int* e = storage + task.edge_block_slba / VERTEX_SIZE;
	int* e_end = e + task.edge_block_len / VERTEX_SIZE;

	// Initialize vertex source and destination addresses
	int u = -1, v = -1, id;
	float *dst, *src;
	long long hmb_offset = (long long)(csd_id + 1) * num_vertices;

	long long start_time, end_time, size_not_in_cache;
	double ratio;
	long long partition_size;

EXEC_START_TIME = ktime_get_ns();
	// Edge block read I/O
	size_not_in_cache = access_edge_block(edge_buf, hmb_dev.done_partition.virt_addr, task.r, task.c, task.edge_block_len, false);
	ratio = task.edge_block_len == 0 ? 1 : (1.0 * size_not_in_cache / task.edge_block_len);
	end_time = ktime_get_ns() + (long long) (task.nsecs_target * ratio);
	// NVMEV_INFO("Edge-%d-%d I/O time: %lld", task.r, task.c, (long long) (task.nsecs_target * ratio));
	while(ktime_get_ns() < end_time){
		if (kthread_should_stop())
			return;
	}
EXEC_END_TIME = ktime_get_ns();
edge_buf->edge_internal_io_time += (EXEC_END_TIME - EXEC_START_TIME);

EXEC_START_TIME = ktime_get_ns();
	// Vertex parition read I/O
	partition_size = (long long) num_vertices * VERTEX_SIZE / task.num_partitions;
	size_not_in_cache = access_partition(vertex_buf, task.r, task.iter, partition_size);
	end_time = ktime_get_ns() + (long long) DMA_READ_LATENCY * size_not_in_cache / PAGE_SIZE;
	// NVMEV_INFO("Partition-%d I/O time: %lld", task.c, (long long) DMA_READ_LATENCY * size_not_in_cache / PAGE_SIZE);
	while(ktime_get_ns() < end_time){
		if (kthread_should_stop())
			return;
		cpu_relax();
		cond_resched();
	}
EXEC_END_TIME = ktime_get_ns();
edge_buf->edge_external_io_time += (EXEC_END_TIME - EXEC_START_TIME);

	// Initialize vertex source and destination addresses
	if(task.is_fvc == 0){
		dst = hmb_dev.buf1.virt_addr;
		src = hmb_dev.buf0.virt_addr;
	}
	else{
		dst = hmb_dev.buf2.virt_addr;
		src = hmb_dev.buf1.virt_addr;
	}

EXEC_START_TIME = ktime_get_ns();
	// Process normal values or future values according to iter in the command
	start_time = ktime_get_ns();
	for(; e < e_end; e += EDGE_SIZE / VERTEX_SIZE) {	
		u = *e, v = *(e + 1);
		dst[v + hmb_offset] += src[u] / outdegree[u];
	}
	end_time = ktime_get_ns();

	// Compensation for MCU lower frequency
	end_time = end_time + (end_time - start_time) * (CPU_MCU_SPEED_RATIO - 1);
	while(ktime_get_ns() < end_time){
		if (kthread_should_stop())
			return;
		cpu_relax();
		cond_resched();
	}
EXEC_END_TIME = ktime_get_ns();
edge_buf->edge_proc_time += (EXEC_END_TIME - EXEC_START_TIME);	

	id = task.csd_id * task.num_partitions * task.num_partitions + task.r * task.num_partitions + task.c;
	if(task.is_fvc == 0)
		hmb_dev.done1.virt_addr[id] = true;
	else
		hmb_dev.done2.virt_addr[id] = true;
}

bool simple_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req,
			     struct nvmev_result *ret, int sqid, int sq_entry)
{
	struct nvme_command *cmd = req->cmd;

	BUG_ON(ns->csi != NVME_CSI_NVM);
	BUG_ON(BASE_SSD != INTEL_OPTANE);

	// NVMEV_INFO("%s: Opcode: %x", __func__, cmd->common.opcode);

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
		ret->nsecs_target = __schedule_io_units(
			cmd->common.opcode, cmd->rw.slba,
			__cmd_io_size((struct nvme_rw_command *)cmd), __get_wallclock());
		// if(cmd->common.opcode == nvme_cmd_write)
		// 	NVMEV_INFO("%s: Response time: %d", __func__, ret->nsecs_target);
		break;
	case nvme_cmd_flush:
		ret->nsecs_target = __schedule_flush(req);
		break;
	case nvme_cmd_csd_process_edge:
		{
			void *vaddr = phys_to_virt(cmd->rw.prp1);
			struct PROC_EDGE proc_edge_struct;
			__u64 current_time, finished_time;
			int csd_flag;

			// NVMEV_INFO("prp1: %llx\n, vaddr: %llx", cmd->rw.prp1, vaddr);
			if (vaddr == NULL || !virt_addr_valid(vaddr)) {
				NVMEV_ERROR("Invalid vaddr: %llx\n", (long long unsigned int)vaddr);
				return -EFAULT;
			}

			// Dispatcher
			memcpy(&proc_edge_struct, vaddr, sizeof(struct PROC_EDGE));
			proc_edge_struct.nsid = cmd->rw.nsid - 1;	// For io worker (do_perform_edge_proc) to know the namespace id
			
			// NVMEV_INFO("[CSD %d, %s()] [nvme_cmd_csd_proc_edge]\n", proc_edge_struct.csd_id, __func__);

			// Schedule the I/O, get the target I/O complete time
			current_time = __get_wallclock();
			ret->nsecs_target = __schedule_io_units(cmd->common.opcode, proc_edge_struct.edge_block_slba, proc_edge_struct.edge_block_len, current_time);
			finished_time = ret->nsecs_target - current_time;
			proc_edge_struct.nsecs_target = finished_time;
			
			// Fill in the CQ entry
			// NVMEV_INFO("%s: Fill in CSD_PROC_EDGE CQ Result", __func__);
			{
				struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
				int cqid = sq->cqid;
				unsigned int command_id = sq_entry(sq_entry).common.command_id;
				unsigned int status = ret->status;
				struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
				int cq_head = cq->cq_head;
				struct nvme_completion *cqe = &cq_entry(cq_head);

				spin_lock(&cq->entry_lock);
				cqe->command_id = command_id;
				cqe->sq_id = sqid;
				cqe->sq_head = sq_entry;
				cqe->status = cq->phase | (status << 1);
				// cqe->result0 = result0;
				// cqe->result1 = result1;
				if (++cq_head == cq->queue_size) {
					cq_head = 0;
					cq->phase = !cq->phase;
				}
				cq->cq_head = cq_head;
				cq->interrupt_ready = true;
				spin_unlock(&cq->entry_lock);
			}

			// Process CQ entries to support asynchronous
			{
				int qidx;
				for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
					struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];
					if (cq == NULL || !cq->irq_enabled)
						continue;
					// NVMEV_INFO("IRQ for CQ entry");
					if (mutex_trylock(&cq->irq_lock)) {
						if (cq->interrupt_ready == true) {
							cq->interrupt_ready = false;
							nvmev_signal_irq(cq->irq_vector);
						}
						mutex_unlock(&cq->irq_lock);
					}
				}
			}

			// Synchronously process the edge processing command
			csd_flag = cmd->rw.apptag;
			if(csd_flag == SYNC){
				__do_perform_edge_proc_grafu(proc_edge_struct);
			}
			else if(csd_flag == ASYNC){
				// Insert proc edge command into task queues; in case of duplicate task (aggregation for future task not done)
				struct queue *normal_task_queue = &(nvmev_vdev->normal_task_queue);
				if(!queue_find(normal_task_queue, proc_edge_struct))
					queue_enqueue(normal_task_queue, proc_edge_struct);
			}
			else if(csd_flag == FLUSH_CSD_DRAM){

				int csd_id = proc_edge_struct.csd_id;
				int num_csds = proc_edge_struct.num_csds;
				int ms_ns_ratio = 1000000;
				int i;

				NVMEV_INFO("Hit/Total (Edge buffer): %lld/%lld", nvmev_vdev->edge_buf.hit_cnt, nvmev_vdev->edge_buf.total_access_cnt);
				NVMEV_INFO("Hit/Total (Vertex buffer): %lld/%lld", nvmev_vdev->vertex_buf.hit_cnt, nvmev_vdev->vertex_buf.total_access_cnt);
				NVMEV_INFO("Edge Processing time: %lld ms, Internal IO time: %lld ms, External IO time: %lld ms", nvmev_vdev->edge_buf.edge_proc_time / ms_ns_ratio, 
					nvmev_vdev->edge_buf.edge_internal_io_time / ms_ns_ratio, nvmev_vdev->edge_buf.edge_external_io_time / ms_ns_ratio);
				NVMEV_INFO("Prefetch Hit/Total Pages (Edge buffer): %lld/%lld", nvmev_vdev->edge_buf.prefetch_hit_cnt, nvmev_vdev->edge_buf.total_prefetch_cnt);
				NVMEV_INFO("Prefetch Hit/Total Edge blocks (Edge buffer): %lld/%lld", nvmev_vdev->edge_buf.prefetch_block_hit_cnt, nvmev_vdev->edge_buf.total_prefetch_block_cnt);
				NVMEV_INFO("Prefetch Priority: 2: %lld, 3: %lld, 4: %lld, 5: %lld", nvmev_vdev->edge_buf.prefetch_priority_cnt[2], nvmev_vdev->edge_buf.prefetch_priority_cnt[3], nvmev_vdev->edge_buf.prefetch_priority_cnt[4], nvmev_vdev->edge_buf.prefetch_priority_cnt[5]);
				NVMEV_INFO("Prefetch Priority Accuracy: 2: %lld/%lld, 3: %lld/%lld, 4: %lld/%lld, 5: %lld/%lld",
					nvmev_vdev->edge_buf.prefetch_block_hit_cnt_arr[2], nvmev_vdev->edge_buf.prefetch_block_cnt_arr[2],
					nvmev_vdev->edge_buf.prefetch_block_hit_cnt_arr[3], nvmev_vdev->edge_buf.prefetch_block_cnt_arr[3],
					nvmev_vdev->edge_buf.prefetch_block_hit_cnt_arr[4], nvmev_vdev->edge_buf.prefetch_block_cnt_arr[4],
					nvmev_vdev->edge_buf.prefetch_block_hit_cnt_arr[5], nvmev_vdev->edge_buf.prefetch_block_cnt_arr[5]);
				
				hmb_dev.buf2.virt_addr[csd_id] = 1.0f * nvmev_vdev->edge_buf.hit_cnt / nvmev_vdev->edge_buf.total_access_cnt;
				hmb_dev.buf2.virt_addr[csd_id + num_csds] = nvmev_vdev->edge_buf.edge_proc_time / ms_ns_ratio;
				hmb_dev.buf2.virt_addr[csd_id + num_csds * 2] = nvmev_vdev->edge_buf.edge_internal_io_time / ms_ns_ratio;
				hmb_dev.buf2.virt_addr[csd_id + num_csds * 3] = nvmev_vdev->edge_buf.edge_external_io_time / ms_ns_ratio;
				for(i = 4; i <= 7; i++){
					hmb_dev.buf2.virt_addr[csd_id + num_csds * i] = 1.0 * nvmev_vdev->edge_buf.prefetch_priority_cnt[i - 2];
				}
				for(i = 8; i <= 11; i++){
					hmb_dev.buf2.virt_addr[csd_id + num_csds * i] = 1.0 * nvmev_vdev->edge_buf.prefetch_block_hit_cnt_arr[i - 6];
				}
				for(i = 12; i <= 15; i++){
					hmb_dev.buf2.virt_addr[csd_id + num_csds * i] = 1.0 * nvmev_vdev->edge_buf.prefetch_block_cnt_arr[i - 10];
				}

				edge_buffer_destroy(&(nvmev_vdev->edge_buf));
				vertex_buffer_destroy(&(nvmev_vdev->vertex_buf));
				hmb_dev.done2.virt_addr[proc_edge_struct.csd_id] = true;
			}

		}
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
			    nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}

	return true;
}

void simple_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			   uint32_t cpu_nr_dispatcher)
{
	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->size = size;
	ns->mapped = mapped_addr;
	ns->proc_io_cmd = simple_proc_nvme_io_cmd;

	return;
}

void simple_remove_namespace(struct nvmev_ns *ns)
{
	// Nothing to do here
}
