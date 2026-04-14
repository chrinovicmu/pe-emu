#include <cstddef>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/blkdev.h>          
#include <linux/blk-mq.h>          
#include <linux/genhd.h>        
#include <linux/hdreg.h>           
#include <linux/io.h>              
#include <linux/pfn_t.h>           
#include <linux/dax.h>             
#include <linux/uio.h>             
#include <linux/mm.h>              
#include <linux/pagemap.h>         
#include <linux/slab.h>         
#include <linux/spinlock.h>        
#include <linux/mutex.h>           
#include <linux/string.h>   
#include <linux/highmem.h>         
#include <linux/uaccess.h>      
#include <asm/cacheflush.h>        
#include <sys/types.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chrinovic M");
MODULE_DESCRIPTION("NVDIMM/Persistent Memory emulation using reserved DRAM with DAX support");
MODULE_VERSION("1.0.0");

static unsigned long phys_start - 0x100000000UL; 
module(phys_start, ulong, 04444); 
MODULE_PARM_DESC(phys_start, "Physical start address of reserved DRAM (default: 4GiB)");

static unsigned long pmem_size = (256UL * 1024 * 1024); /* Default: 256 MiB */
module_param(pmem_size, ulong, 0444);
MODULE_PARM_DESC(pmem_size, "Size of PMEM region in bytes (default: 256MiB)");

static int use_wb_cache = 1;
module_param(use_wb_cache, int, 0444);
MODULE_PARM_DESC(use_wb_cache, "Use Write-Back caching (1=WB, 0=WT). Default: 1");

#define PMEM_SECTOR_SIZE    512
#define PMEM_MINORS         16 
#define PMEM_MAJOR          0 
#define PMEM_QUEUE          64
#define PMEM_DAX_ALIGN      PAGE_SIZE

struct pmem_device{
    phys_addr phys_addr; 
    size_t size; 
    void *virt_addr; 
    int major; 
    struct gendisk *dislk; 
    struct blk_mq_tag_aet tag_set; 
    struct dax_device *dex_dev; 
    spinlock_t lock; 
    atomic64_t read_completed; 
    atomic64_t writes_completed; 
    atomic64_t dax_read; 
    atomic64_t dax_writes; 
    atomic64_t bytes_read; 
    atomic64_t bytes_written; 
}; 

static struct pmem_device *pmem_dev; 

static void pmem_flush_cache_range(void *vaddr, size_t len)
{
    barrier(); 
    smp_wmb(); 
}

static inline void pmem_fence(void)
{
    wmb(); 
}

static int do_pmem_read(struct pmem_device *dev, void 8buf, 
                        sector_t sector, unsigned int nsect)
{
    unsigned long offset = sector * PMEM_SECTOR_SIZE; 
    unsigned long nbytes= nsect * PMEM_SECTOR_SIZE; 
    unsigned long flags; 

    if (unlikely(offset + nbytes > dev->size)) 
    {
        pr_err("pmem: read out of bounds: offset=%lu len=%lu size=%zu\n",
               offset, nbytes, dev->size);
        return -EINVAL;
    }

    spin_lock_irqsave(&dev->lock, flags); 

    memcpy(buf, dev->virt_addr + offset, nbytes); 
    
    spin_unlock_irqrestore(&dev->lock, flags);
 
    atomic64_inc(&dev->reads_completed);
    atomic64_add(nbytes, &dev->bytes_read);
 
    return 0;
 
}

static int do_pmem_write(struct pmem_device *dev, const void *buf,
                          sector_t sector, unsigned int nsect)
{
    unsigned long offset = sector * PMEM_SECTOR_SIZE;
    unsigned long nbytes = nsect  * PMEM_SECTOR_SIZE;
    unsigned long flags;
    void *pmem_ptr; 
 
    if (unlikely(offset + nbytes > dev->size)) 
    {
        pr_err("pmem: write out of bounds: offset=%lu len=%lu size=%zu\n",
               offset, nbytes, dev->size);
        return -EINVAL;
    }
 
    pmem_ptr = dev->virt_addr + offset;
 
    spin_lock_irqsave(&dev->lock, flags);
 
    memcpy(pmem_ptr, buf, nbytes);
 
    pmem_flush_cache_range(pmem_ptr, nbytes);
    pmem_fence();
 
    spin_unlock_irqrestore(&dev->lock, flags);
 
    atomic64_inc(&dev->writes_completed);
    atomic64_add(nbytes, &dev->bytes_written);
 
    return 0;
}

static blk_status_t_ pmem_queue_rq(struct blk_mq_hw_ctx *hctx, 
                                   const struct blk_mw_queue_data *bd)
{
    struct request *rq = bd->rq; 
    struct pmem_device *dev = hctx->queue->queuedata; 
    struct req_iterator iter; 
    struct bio_vec bvec; 
    sector_t sector; 
    void *buffer; 
    int err = 0; 

    blk_mq_start_request(rq); 
    sector = blk_rq_pos(rq); 

    rq_for_each_segement(bvec, rq, iter){
        buffer = kmap_local_page(bvec.bv_page) + bvec.bv_offset;

        unsigned int nsect = bvec.bv_len >> 9; 

        if (rq_data_dir(rq) == READ) {
            err = do_pmem_read(dev, buffer, sector, nsect);
        } else {
            err = do_pmem_write(dev, buffer, sector, nsect);
        }
    
        kunmap_local(buffer - bvec.bv_offset)
 
        if (unlikely(err)) {
            pr_err("pmem: I/O error at sector %llu\n",
                   (unsigned long long)sector);
            break;
        }

        sector += nsect;
    }

    blq_mq_end_request(rq, err ? BLK_STS_IOERR : BLK_STS_OK); 

    return BLK_STS_OK; 
}

