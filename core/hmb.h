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

/* Structure for HMB device */
struct hmb_device {
    struct hmb_buffer buf1;  /* First 256MB buffer */
    struct hmb_buffer buf2;  /* Second 256MB buffer */
};

/* mmap operation for user space mapping */
int hmb_mmap(struct file *file, struct vm_area_struct *vma);

/* Device setup and cleanup */
int hmb_setup_device(void);
void hmb_cleanup_device(void);

/* Memory management */
int hmb_init_buffer(struct hmb_buffer *buf, phys_addr_t phys_addr);
void hmb_cleanup_buffer(struct hmb_buffer *buf);

/* Module init and exit */
int hmb_init(void);
void hmb_exit(void);

/* Global variable declaration */
extern struct hmb_device hmb_dev;

#endif /* _HMB_H_ */