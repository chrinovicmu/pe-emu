#include <cerrno>
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

static const struct blk_mq_ops pmem_mq_ops = {
    .queue_rq = pmem_queue_rq, 
};

static int pmem_open(struct block_device *bdev, fmode_t mode)
{
    struct pmem_device *dev = bdev->bd_disk->private_data;
 
    if (!dev) {
        return -ENXIO; /* No such device — shouldn't happen */
    }
 
    pr_debug("pmem: device opened (mode=0x%x)\n", mode);
    return 0;
}

static void pmem_release(struct gendisk *disk, fmode_t mode)
{
    struct pmem_device *dev = disk->private_data;
 
    pmem_flush_cache_range(dev->virt_addr, dev->size);
    pmem_fence();
 
    pr_debug("pmem: device released\n");
}

static int pmem_ioctl(struct block_device *bdev, fmode_t mode,
                       unsigned int cmd, unsigned long arg)
{
    long size;
    struct hd_geometry geo;
 
    switch (cmd) {

        case HDIO_GETGEO:
            size = bdev->bd_disk->part0.nr_sects; /* total sectors */
            geo.cylinders = (size & ~0x3f) >> 6;  /* size / (4 heads × 16 sectors) */
            geo.heads      = 4;
            geo.sectors    = 16;
            geo.start      = 0;
 
            if (copy_to_user((void __user *)arg, &geo, sizeof(geo)))
                return -EFAULT;
            return 0;
 
        default:
            return -ENOTTY;
    }
}

static int pmem_rw_page(struct block_device *bdev, sector_t sector, 
                        struct page *page, unsigned int op)
{
    struct pmem_device *dev = bdev->bd_disk->private_data; 
    unsigned long offset = sector * PMEM_SECTOR_SIZE; 
    void *buffer; 
    int err; 

    if(unlikely(offset + PAGE_SIZE > dev->size))
        return -EINVAL; 

    buffer = kmap_atomic(page);

    if(op == REQ_OP_READ)
    {
        memcpy(buffer, dev->virt_addr + offset, PAGE_SIZE); 
        err = 0; 
    }else{
        memcpy(dev->virt_addr + offset, buffer, PAGE_SIZE); 
        pmem_flush_cache_range(dev->virt_addr + offset, PAGE_SIZE); 
        pmem_fence(); 
    }

    kunmap_atomic(buffer);

    page_endio(page, op!= REQ_OP_READ, err); 
    return err 
}

static const struct block_device_operations pmem_fops = {
    .owner    = THIS_MODULE,     
    .open     = pmem_open,
    .release  = pmem_release,
    .ioctl    = pmem_ioctl,
    .rw_page  = pmem_rw_page,    
};


static long pmem_dax_direct_access(struct dax_device *dax_dev, 
                                   pgoff_t pgoff, long nr_pages, 
                                   enum dax_access_mode mode, 
                                   void **kaddr, pfn_t *pfn)
{
    struct pmem_device *dev = dax_get_private(dax_dev); 
    resource_size_t offset; 
    long avail; 

    offset = (resource_size_t)pgoff << PAGE_SHIFT; 
    if(offset >= dev->size)
        return -EINVAL; 

    avail = (dev->size - offset) >> PAGE_SHIFT; 
    if(avail < nr_pages)
        nr_pages = avail; 

    if(kaddr)
        *kaddr = dev->virt_addr + offset; 

    if(pfn)
        *pfn = phys_to_pfn(dev->phys_addr + offset, PFN_DEV | PFN_MAP );
    
    pr_debug("pmem: DAX direct_access pgoff=%lu nr_pages=%ld kaddr=%px pfn=%lu\n",
             pgoff, nr_pages, kaddr ? *kaddr : NULL,
             pfn ? pfn_t_to_pfn(*pfn) : 0);
    
    return nr_pages; 
}

static size_t pmem_dax_copy_from_iter(struct dax_device *dax_dev, pgoff_t, 
                                     void *addr, size_t bytes, struct iov_iter *i)
{
    struct pmem_device *dev = dax_get_private(dax_dev); 
    size_t written; 

    /*MOVNTI on x86*/ 
    written = copy_from_iter_nocache(addr, bytes, i);

    /*non temporal stores have weaker odering, we use the fence*/ 
    if(written > 0)
    {
        pmem_flush_cache_range(addr, written);
        pmem_fence();
 
        atomic64_inc(&dev->dax_writes);
        atomic64_add(written, &dev->bytes_written);
    }

    return written; 
}

static size_t pmem_dax_copy_to_iter(struct dax_device *dax_dev, pgoff_t pgoff,
                                     void *addr, size_t bytes, struct iov_iter *i)
{
    struct pmem_device *dev = dax_get_private(dax_dev);
    size_t read;
 
    read = copy_to_iter(addr, bytes, i);
 
    if (read > 0) 
    {
        atomic64_inc(&dev->dax_reads);
        atomic64_add(read, &dev->bytes_read);
    }
 
    return read;
}

/*clear a range of PMEM pages */ 
static int pmem_dax_zero_page_range(struct dax_device *dax_dev, pgoff_t pgoff,
                                     long nr_pages)
{
    struct pmem_device *dev    = dax_get_private(dax_dev);
    resource_size_t    offset  = (resource_size_t)pgoff << PAGE_SHIFT;
    size_t             nbytes  = nr_pages * PAGE_SIZE;
 
    if (offset + nbytes > dev->size)
        return -EINVAL;
 
    memset(dev->virt_addr + offset, 0, nbytes);
    pmem_flush_cache_range(dev->virt_addr + offset, nbytes);
    pmem_fence();
 
    return 0;
}

static const struct dax_operations pmem_dax_ops = {
    .direct_access   = pmem_dax_direct_access,   
    .copy_from_iter  = pmem_dax_copy_from_iter,   
    .copy_to_iter    = pmem_dax_copy_to_iter,     
    .zero_page_range = pmem_dax_zero_page_range,
};


static itn __init pmem_init(void)
{
    struct pmem_device *dev; 
    int err = 0; 
    unsigned long memremap_flags; 

    pr_info("pmem: Initializing PMEM emulation driver\n");
    pr_info("pmem: Physical address: 0x%lx, Size: %lu MiB\n",
            phys_start, pmem_size / (1024 * 1024));

    
    /*we check for alignment 
    * memremap() requires pager-aligned address and sizes*/ 
     
    if (phys_start & (PAGE_SIZE - 1)) 
    {
        pr_warn("pmem: phys_start not page-aligned; aligning down\n");
        phys_start &= PAGE_MASK; /* PAGE_MASK = ~(PAGE_SIZE - 1) */
    }
 
    if (pmem_size & (PAGE_SIZE - 1))
    {
        pr_warn("pmem: pmem_size not page-aligned; aligning down\n");
        pmem_size &= PAGE_MASK;
    }
 
    if (pmem_size == 0) 
    {
        pr_err("pmem: pmem_size must be > 0\n");
        return -EINVAL;
    }

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
    {
        pr_err("pmem: Failed to allocate device structure\n");
        return -ENOMEM;
    }
 
    dev->phys_addr = (phys_addr_t)phys_start;
    dev->size      = pmem_size;
    spin_lock_init(&dev->lock); 
 
    atomic64_set(&dev->reads_completed, 0);
    atomic64_set(&dev->writes_completed, 0);
    atomic64_set(&dev->dax_reads, 0);
    atomic64_set(&dev->dax_writes, 0);
    atomic64_set(&dev->bytes_read, 0);
    atomic64_set(&dev->bytes_written, 0);

    memremap_flags = use_wb_cache ? MEMREMAP_WB : MEMREMAP_WT; 

    pr_info("pmem: Mapping 0x%llx..0x%llx with %s caching\n",
            (unsigned long long)dev->phys_addr,
            (unsigned long long)(dev->phys_addr + dev->size - 1),
            use_wb_cache ? "Write-Back" : "Write-Through");
    
    dev->virt_addr = memremap(dev->phys_addr, dev->size, memremap_flags);
    if (!dev->virt_addr) 
    {
        pr_err("pmem: memremap() failed for phys=0x%llx size=%zu\n",
               (unsigned long long)dev->phys_addr, dev->size);
        err = -ENOMEM;
        goto err_free_dev;
    }
 
    pr_info("pmem: Physical 0x%llx mapped to virtual %px (%zu MiB)\n",
            (unsigned long long)dev->phys_addr, dev->virt_addr,
            dev->size / (1024 * 1024));
    
    memset(dev->virt_addr, 0, dev->size);
    pr_info("pmem: PMEM region zero-initialized\n");


}

