// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/highmem.h>

#include <linux/sched.h>     // For task_struct and current
#include <linux/sched/signal.h> // For accessing process structures
#include <linux/preempt.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "simple_ftl.h"
#include "core/queue.h"
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
	const int vertex_size = 4;
	const int edge_size = 8;    //Unweighted

	// Initialize the edge starting addresses
	int* storage = nvmev_vdev->ns[task.nsid].mapped;
	int* outdegree = storage + task.outdegree_slba / vertex_size;
	int* e = storage + task.edge_block_slba / vertex_size;
	int* e_end = e + task.edge_block_len / vertex_size;
	NVMEV_INFO("[Grafu CSD %d, %s()]: Processing edge-block-%u-%u, version %d:, io_time: %d", task.csd_id, __func__, task.r, task.c, task.iter, task.nsecs_target);
	
	// Edge block read I/O
	if(task.iter == 0){
		long long end_time = ktime_get_ns() + task.nsecs_target;
		while(ktime_get_ns() < end_time);
	}
	
	// Process normal values or future values according to iter in the command
	int u = -1, v = -1;
	for(; e < e_end; e += edge_size / vertex_size) {	
		u = *e, v = *(e + 1);
		preempt_disable();
		if(task.iter == 0)
			hmb_dev.buf1.virt_addr[v] += hmb_dev.buf0.virt_addr[u] / outdegree[u];
		else
			hmb_dev.buf2.virt_addr[v] += hmb_dev.buf1.virt_addr[u] / outdegree[u];
		preempt_enable();
	}
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
			// NVMEV_INFO("prp1: %llx\n, vaddr: %llx", cmd->rw.prp1, vaddr);
			if (vaddr == NULL || !virt_addr_valid(vaddr)) {
				NVMEV_ERROR("Invalid vaddr: %llx\n", vaddr);
				return -EFAULT;
			}

			// Dispatcher
			struct PROC_EDGE proc_edge_struct;
			memcpy(&proc_edge_struct, vaddr, sizeof(struct PROC_EDGE));
			proc_edge_struct.nsid = cmd->rw.nsid - 1;	// For io worker (do_perform_edge_proc) to know the namespace id
			
			NVMEV_INFO("[CSD %d, %s()] [nvme_cmd_csd_proc_edge]\n", proc_edge_struct.csd_id, __func__);

			// Schedule the I/O, get the target I/O complete time
			__u64 current_time = __get_wallclock();
			ret->nsecs_target = __schedule_io_units(
			cmd->common.opcode, proc_edge_struct.edge_block_slba, proc_edge_struct.edge_block_len, current_time);
			__u64 finished_time = ret->nsecs_target - current_time;
			proc_edge_struct.nsecs_target = finished_time;
			
			// Synchronously process the edge processing command
			int csd_flag = cmd->rw.apptag;
			if(csd_flag == SYNC){
				NVMEV_INFO("io_finished_time: %llu(ns), io_time_span: %llu(us)\n", ret->nsecs_target, finished_time / 1000);
				__do_perform_edge_proc_grafu(proc_edge_struct);
			}

			// Fill in the CQ entry
			// NVMEV_INFO("%s: Fill in CSD_PROC_EDGE CQ Result", __func__);
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

			// Process CQ entries to support asynchronous
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

			if(csd_flag == ASYNC){
				// Insert proc edge command into task queues
				// In case of duplicate task (aggregation for future task not done)
				struct queue *normal_task_queue = &(nvmev_vdev->normal_task_queue);
				if(!queue_find(normal_task_queue, proc_edge_struct))
					queue_enqueue(normal_task_queue, proc_edge_struct);
				// NVMEV_INFO("%s: Enqueue Normal_task_queue: Queue size: %d, edge_slba: %llu, edge_len: %llu, io_time_span: %llu(us)", 
				// 	__func__, get_queue_size(normal_task_queue), proc_edge_struct.edge_block_slba, proc_edge_struct.edge_block_len, finished_time / 1000);
				NVMEV_INFO("%s: Enqueue Normal_task_queue: Queue size: %d, Edge-%u-%u, io_time_span: %llu(us)", 
					__func__, get_queue_size(normal_task_queue), proc_edge_struct.r, proc_edge_struct.c, finished_time / 1000);
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
