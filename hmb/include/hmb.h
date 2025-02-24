#ifndef _HMB_H_
#define _HMB_H_

#include <linux/types.h>
#include <linux/mm.h>

#define DEVICE_NAME "hmb_mem"
#define MB_256 (256 * 1024 * 1024UL)
#define HMB_SIZE MB_256

/* Structure for a single HMB buffer */
struct hmb_buffer {
    float *virt_addr;         /* Kernel virtual address of mapped memory */
    phys_addr_t phys_addr;  /* Physical address of memory region */
    size_t size;           /* Size of memory region */
};

struct hmb_bitmap_buffer {
    bool *virt_addr;         /* Kernel virtual address of mapped memory */
    phys_addr_t phys_addr;  /* Physical address of memory region */
    size_t size;           /* Size of memory region */
};

/* Structure for HMB device */
struct hmb_device {
    struct hmb_buffer buf0;  /* First 256MB buffer (v_t) */
    struct hmb_buffer buf1;  /* Second 256MB buffer (v_t+1) */
    struct hmb_buffer buf2;  /* Third 256MB buffer (v_t+2)*/
    struct hmb_bitmap_buffer done1, done2;  /* Aggregation from CSDs: num_csd * num_partition ^ 2 * 2 (Normal, future) */ 
    struct hmb_bitmap_buffer done_partition;    /* Aggregation done to notify CSDs*/
};

/* mmap operation for user space mapping */
int hmb_mmap(struct file *file, struct vm_area_struct *vma);

/* Device setup and cleanup */
int hmb_setup_device(void);
void hmb_cleanup_device(void);

/* Memory management */
int hmb_init_buffer(struct hmb_buffer *buf, phys_addr_t phys_addr);
void hmb_cleanup_buffer(struct hmb_buffer *buf);
int hmb_init_bitmap_buffer(struct hmb_bitmap_buffer *buf, phys_addr_t phys_addr);
void hmb_cleanup_bitmap_buffer(struct hmb_bitmap_buffer *buf);

/* Module init and exit */
static int hmb_init(void);
static void hmb_exit(void);

/* Global variable declaration */
extern struct hmb_device hmb_dev;

#endif /* _HMB_H_ */