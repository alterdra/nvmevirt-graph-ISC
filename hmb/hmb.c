#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/io.h>
#include "hmb.h"

#define MEM_START 7 * 1024 * 1024 * 1024UL
struct hmb_device hmb_dev = {
    .buf1 = {
        .virt_addr = NULL,
        .phys_addr = MEM_START,  /* MEM_START = 7GB, Size = 256MB */
        .size = HMB_SIZE
    },
    .buf2 = {
        .virt_addr = NULL,
        .phys_addr = MEM_START + HMB_SIZE,  /* MEM_START = 7GB + 256MB, Size = 256MB */
        .size = HMB_SIZE
    },
    .done = {
        .virt_addr = NULL,
        .phys_addr = MEM_START + HMB_SIZE * 2,  
        .size = HMB_SIZE    /* num_csd * num_partition ^ 2 * 2 (Normal, future) */ 
    }
};
EXPORT_SYMBOL(hmb_dev);

static int major_number;
static struct class *hmb_class;

/* File operations */
static struct file_operations hmb_fops = {
    .owner = THIS_MODULE,
    .mmap = hmb_mmap,
};

int hmb_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    struct hmb_buffer *buf = NULL;
    struct hmb_bitmap_buffer *done = NULL;

    /* Select which buffer based on offset */
    if (offset == 0) {
        buf = &hmb_dev.buf1;
    } else if (offset == HMB_SIZE) {
        buf = &hmb_dev.buf2;
    } else if (offset == HMB_SIZE * 2) {
        done = &hmb_dev.done;
    } else {
        return -EINVAL;
    }

    if(buf != NULL){
        if (size > buf->size)
            return -EINVAL;
        pr_info("HMB: gg float");
        return remap_pfn_range(vma, 
                            vma->vm_start,
                            buf->phys_addr >> PAGE_SHIFT,
                            size,
                            vma->vm_page_prot);
    }
    else if(done != NULL){
        if (size > done->size)
            return -EINVAL;
        pr_info("HMB: gg boolean");
        return remap_pfn_range(vma, 
                            vma->vm_start,
                            done->phys_addr >> PAGE_SHIFT,
                            size,
                            vma->vm_page_prot);
    }
    return -1;
}

int hmb_init_buffer(struct hmb_buffer *buf, phys_addr_t phys_addr)
{
    buf->phys_addr = phys_addr;
    buf->virt_addr = memremap(buf->phys_addr, buf->size, MEMREMAP_WB);
    if (!buf->virt_addr)
        return -ENOMEM;
    memset(buf->virt_addr, 0, buf->size);
    return 0;
}

void hmb_cleanup_buffer(struct hmb_buffer *buf)
{
    if (buf->virt_addr) {
        memunmap(buf->virt_addr);
        buf->virt_addr = NULL;
    }
}

int hmb_init_bitmap_buffer(struct hmb_bitmap_buffer *buf, phys_addr_t phys_addr)
{
    buf->phys_addr = phys_addr;
    buf->virt_addr = memremap(buf->phys_addr, buf->size, MEMREMAP_WB);
    if (!buf->virt_addr)
        return -ENOMEM;
    memset(buf->virt_addr, 0, buf->size);
    return 0;
}

void hmb_cleanup_bitmap_buffer(struct hmb_bitmap_buffer *buf)
{
    if (buf->virt_addr) {
        memunmap(buf->virt_addr);
        buf->virt_addr = NULL;
    }
}

int hmb_setup_device(void)
{
    major_number = register_chrdev(0, DEVICE_NAME, &hmb_fops);
    if (major_number < 0)
        return major_number;

    hmb_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(hmb_class)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(hmb_class);
    }

    if (IS_ERR(device_create(hmb_class, NULL, 
                            MKDEV(major_number, 0),
                            NULL, DEVICE_NAME))) {
        class_destroy(hmb_class);
        unregister_chrdev(major_number, DEVICE_NAME);
        return PTR_ERR(hmb_class);
    }

    return 0;
}

void hmb_cleanup_device(void)
{
    device_destroy(hmb_class, MKDEV(major_number, 0));
    class_destroy(hmb_class);
    unregister_chrdev(major_number, DEVICE_NAME);
}

static int __init hmb_init(void)
{
    int ret;

    /* Initialize both buffers */
    ret = hmb_init_buffer(&hmb_dev.buf1, hmb_dev.buf1.phys_addr);
    if (ret < 0)
        return ret;

    ret = hmb_init_buffer(&hmb_dev.buf2, hmb_dev.buf2.phys_addr);
    if (ret < 0) {
        hmb_cleanup_buffer(&hmb_dev.buf1);
        return ret;
    }

    ret = hmb_init_bitmap_buffer(&hmb_dev.done, hmb_dev.done.phys_addr);
    if (ret < 0) {
        hmb_cleanup_buffer(&hmb_dev.buf1);
        hmb_cleanup_buffer(&hmb_dev.buf2);
        return ret;
    }

    /* Setup device */
    ret = hmb_setup_device();
    if (ret < 0) {
        hmb_cleanup_buffer(&hmb_dev.buf1);
        hmb_cleanup_buffer(&hmb_dev.buf2);
        hmb_cleanup_bitmap_buffer(&hmb_dev.done);
        return ret;
    }

    pr_info("HMB: Initialized with two 256MB buffers\n");
    return 0;
}

static void __exit hmb_exit(void)
{
    hmb_cleanup_device();
    hmb_cleanup_buffer(&hmb_dev.buf1);
    hmb_cleanup_buffer(&hmb_dev.buf2);
    pr_info("HMB: Cleaned up\n");
}

module_init(hmb_init);
module_exit(hmb_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Benny");
MODULE_DESCRIPTION("Host Memory Buffer Driver starting at 7GB");