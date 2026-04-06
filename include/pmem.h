#ifndef _PMEM_DRIVER_H
#define _PMEM_DRIVER_H
 
#include <linux/types.h>     
#include <linux/dax.h>      
#include <linux/blkdev.h>    


/*
 * Driver version following semantic versioning (MAJOR.MINOR.PATCH):
 *   MAJOR: incompatible API changes
 *   MINOR: new backwards-compatible features
 *   PATCH: backwards-compatible bug fixes
 */
#define PMEM_DRV_VERSION_MAJOR  1
#define PMEM_DRV_VERSION_MINOR  0
#define PMEM_DRV_VERSION_PATCH  0
#define PMEM_DRV_VERSION_STR    "1.0.0"


#define PMEM_CACHE_LINE_SIZE    64
#define PMEM_MIN_SIZE           (16UL * 1024 * 1024)  /* 16 MiB */
#define PMEM_MAX_SIZE           (64UL * 1024 * 1024 * 1024) /* 64 GiB */
#define PMEM_ALIGN              PAGE_SIZE

#define PMEM_MAGIC              0x504D454DUL
#define PMEM_VERSION_MAGIC      0x00010000UL /* version 1.0.0 in packed form */

struct pmem_super
{
    __le32 magic; 
    __le32 version; 
    __le64 size;
    __le64 phys_addr;
    __le64 c_time;
    __le32  checksum;
     u8      _reserved[512 - (4 + 4 + 8 + 8 + 8 + 4)];
}__packed; 

struct pmem_error_inject 
{
    int enabled;
    sector_t error_sector;
    int error_type;
    int persist;
};


static inline unsigned long pmem_sector_to_offset(sector_t sector)
{
    return (unsigned long)sector << 9; /* sector × 512 */
}

static inline sector_t pmem_offset_to_sector(unsigned long offset)
{
    return (sector_t)(offset >> 9); /* offset ÷ 512 */
}

static inline unsigned long pmem_offset_to_pfn(phys_addr_t phys_base,
                                                 unsigned long offset)
{
    return (unsigned long)((phys_base + offset) >> PAGE_SHIFT);
}

static inline unsigned long pmem_pfn_to_offset(phys_addr_t phys_base,
                                                unsigned long pfn)
{
    return (unsigned long)((pfn << PAGE_SHIFT) - phys_base);
}


#define PMEM_STORE_FENCE()  wmb()
#define PMEM_LOAD_FENCE()   rmb()

 
#define PMEM_SYSFS_DIR          "pmem"
#define PMEM_SYSFS_PHYS_ADDR    "phys_addr"     /* physical base address */
#define PMEM_SYSFS_SIZE         "size"           /* region size in bytes */
#define PMEM_SYSFS_READS        "reads"          /* total read operations */
#define PMEM_SYSFS_WRITES       "writes"         /* total write operations */
#define PMEM_SYSFS_DAX_READS    "dax_reads"      /* DAX read operations */
#define PMEM_SYSFS_DAX_WRITES   "dax_writes"     /* DAX write operations */
#define PMEM_SYSFS_BYTES_READ   "bytes_read"     /* cumulative bytes read */
#define PMEM_SYSFS_BYTES_WRITTEN "bytes_written" /* cumulative bytes written */
#define PMEM_SYSFS_CACHE_POLICY "cache_policy"   /* WB or WT */

#define PMEM_IOCTL_MAGIC    'P'
#define PMEM_IOCTL_FLUSH    _IO(PMEM_IOCTL_MAGIC, 1)

struct pmem_info 
{
    __u64   phys_addr;      
    __u64   size;           
    __u32   cache_policy;   
    __u32   dax_enabled;    
    __u64   reads;          
    __u64   writes;         
    __u64   bytes_read;     
    __u64   bytes_written;
};

#define PMEM_IOCTL_GET_INFO _IOR(PMEM_IOCTL_MAGIC, 2, struct pmem_info)
#define PMEM_IOCTL_INJECT_ERROR _IOW(PMEM_IOCTL_MAGIC, 3, struct pmem_error_inject)
#define PMEM_IOCTL_CLEAR_ERROR  _IO(PMEM_IOCTL_MAGIC, 4)

#endif /* _PMEM_DRIVER_H */
