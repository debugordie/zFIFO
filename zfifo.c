// zfifo: a Zero-copy AXI SG DMA driver for Zynq + Linux

#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysctl.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/scatterlist.h>
#include <linux/pagemap.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <asm/page.h>
#include <asm/byteorder.h>

#define _ZFIFO_DRIVER_
#include "zfifo.h"

MODULE_DESCRIPTION("User space zero-copy AXI SG-DMA driver");
MODULE_AUTHOR("osana");
MODULE_LICENSE("Dual BSD/GPL");

#define DRIVER_VERSION     "0.9.1"
#define DRIVER_NAME        "zfifo"
#define DEVICE_NAME_FORMAT "zfifo%d"
#define DEVICE_MAX_NUM      256

#define MM2S_DMACR       0
#define MM2S_DMASR       1
#define MM2S_CURDESC     2
#define MM2S_CURDESC_H   3
#define MM2S_TAILDESC    4
#define MM2S_TAILDESC_H  5
#define SG_CTL          11
#define S2MM_DMACR      12
#define S2MM_DMASR      13
#define S2MM_CURDESC    14
#define S2MM_CURDESC_H  15
#define S2MM_TAILDESC   16
#define S2MM_TAILDESC_H 17

#define DMACR_RS      (1u<<0)
#define DMACR_RESET   (1u<<2)
#define DMASR_HALTED  (1u<<0)
#define DMASR_IDLE    (1u<<1)
#define DMASR_IOC_Irq (1u<<12)
#define DMASR_ERR_Irq (1u<<14)

static struct class*  zfifo_sys_class = NULL;
static unsigned desc_size = 1100*1024; // descriptor space
static unsigned dma_reg_size = 128;    // AXI DMA register space size
static const int dmac_buf_bits = 20;   // # bits of DMAC buffer counter

#define LOW32(x) (x & 0xFFFFFFFF)

#ifdef __aarch64__
// ------------------------------
#define HIGH32(x) ((x>>32) & 0xFFFFFFFF)
static int dma_mask_bit = 64;
// ------------------------------
#else
// ------------------------------
#define HIGH32(x) (0)
static int dma_mask_bit = 32;
// ------------------------------
#endif

static int        info_enable = 1;
module_param(     info_enable , int, S_IRUGO);
MODULE_PARM_DESC( info_enable , "zfifo install/uninstall infomation enable");

typedef struct {
  struct device* sys_dev;
  struct device* dma_dev;
  struct cdev    cdev;
  dev_t          device_number;
  bool           is_open;
  unsigned*      dma_regs_phys;
  volatile unsigned __iomem *dma_regs;
  void          *tx_desc_base, *rx_desc_base;
  dma_addr_t     tx_phys_base,  rx_phys_base;
  unsigned      *tx_desc, *rx_desc;
  dma_addr_t     tx_phys, rx_phys;
  unsigned       dmac_buf_len;
} zfifo_device_data;

// ----------------------------------------------------------------------
// SG mapping stuff


typedef struct {
  long npages;
  struct page ** pages;
  struct scatterlist * sgl;
  enum dma_data_direction dir;
  unsigned long num_sg;
  zfifo_device_data* dev;
} sg_mapping;

static void release_pinned(struct page **pages, long npages){
  int i;
  for(i=0; i<npages; i++)
    put_page(pages[i]);
}


static sg_mapping *alloc_sg_buf(zfifo_device_data* this,
                                char __user *bufp, unsigned long len,
                                enum dma_data_direction dir,
                                volatile unsigned *sg_desc, dma_addr_t sg_phys){
  sg_mapping *sg_map = NULL;
  struct page **pages = NULL;
  struct scatterlist * sgl;
  struct scatterlist * sg;

  unsigned long npages_req = 0;
  unsigned long udata = (unsigned long) bufp;
  long npages = 0;
  int i;

  unsigned long len_rem = len;
  unsigned fp_offset; // first page offset
  unsigned long num_sg;

  unsigned d;
  
  npages_req = ((udata + len - 1)>>PAGE_SHIFT) - (udata>>PAGE_SHIFT) + 1;
    
  // Alloc sg_mapping
  if ((sg_map = (sg_mapping *)kmalloc(sizeof(*sg_map), GFP_KERNEL)) == NULL){
    printk(KERN_ERR "zfifo: could not allocate memory for sg_mapping struct\n");
    return NULL;
  }

  // Alloc pages array
  if ((pages = kmalloc(npages_req * sizeof(*pages), GFP_KERNEL)) == NULL){
    printk(KERN_ERR "zfifo: could not allocate memory for pages array\n");
    kfree(sg_map);
    return NULL;
  }

  // Pin pages
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
  mmap_read_lock(current->mm);
#else
  down_read(&current->mm->mmap_sem);
#endif

  npages = get_user_pages(udata, npages_req,
                          ((dir==DMA_FROM_DEVICE) ? FOLL_WRITE : 0),
                          pages, NULL);
  if (npages <= 0){
    printk(KERN_ERR "zfifo: unable to pin any pages in memory\n");
    kfree(pages);
    kfree(sg_map);
    return NULL;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,8,0)
  mmap_read_unlock(current->mm);
#else
  up_read(&current->mm->mmap_sem);
#endif

  // Create scatterlist array
  if ((sgl = kcalloc(npages, sizeof(*sgl), GFP_KERNEL)) == NULL) {
    printk(KERN_ERR "zfifo: could not allocate memory for scatterlist array\n");
    release_pinned(pages, npages);
    kfree(pages);
    kfree(sg_map);
    return NULL;
  }

  // Fill scatterlist array
  fp_offset = (udata & (~PAGE_MASK));
  sg_init_table(sgl, npages);
  for (i=0; i<npages; i++){
    unsigned page_len;
    page_len = ((fp_offset+len_rem) > PAGE_SIZE ? (PAGE_SIZE-fp_offset) :
                len_rem);
    sg_set_page(&sgl[i], pages[i], page_len, fp_offset);

    //    printk("SG List [%d], offset 0x%x, page_len %d, %ld to go\n",
    //            i, fp_offset, page_len, len_rem);

    len_rem -= page_len;
    fp_offset = 0; // no offset for 2nd and later pages
  }

  // Finalize scatterlist array and get DMA addresses
  num_sg = dma_map_sg(this->dma_dev, sgl, npages, dir);
  //  printk("dma_map_sg done, num=%ld\n", num_sg);

  d=0;
  for_each_sg(sgl, sg, num_sg, i) {
    unsigned int hw_len, prev_len;
    dma_addr_t hw_addr, prev_addr;
    
    dma_addr_t next_desc;
    const unsigned ctrl_sof = 1u << 27;
    const unsigned ctrl_eof = 1u << 26;

    int merge=0;

    next_desc = sg_phys + (0x40 * (d+1));
    
    hw_addr = sg_dma_address(sg);
    hw_len  = sg_dma_len(sg);

    if (i!=0 && i!=num_sg-1){ // decide to merge or not to
#ifdef __aarch64__
      prev_addr = ( sg_desc[(d-1)*16 +2] +
                    (((dma_addr_t)(sg_desc[(d-1)*16+3]))<<32) );
#else
      prev_addr = sg_desc[(d-1)*16 +2];
#endif

      prev_len =  sg_desc[(d-1)*16 +6] & 0x007FFFFF;

      if (hw_addr == prev_addr+prev_len &&
          (prev_len+hw_len) < this->dmac_buf_len){
        merge=1;
        
        sg_desc[(d-1)*16 +6] =
          (sg_desc[(d-1)*16 +6] & 0xFF800000) +
          ((prev_len+hw_len)    & 0x007FFFFF);

        /*
        printk("SG Merge [%d:%pad], %pad, len=%u (%u)\n",
               d-1, &sg_phys, &prev_addr, prev_len+hw_len,
               this->dmac_buf_len);   */
             
      }
    }

    if (merge==0){
      // not to merge

      /*
      printk("SG Map [%d:%pad], %pad, next=%pad, len=%u, sof=%d, eof=%d\n",
             d, &sg_phys,
             &hw_addr, &next_desc, hw_len,
             (d==0 ? 1: 0), (i==num_sg-1 ? 1:0)); 
      */
      
      sg_desc[d*16 + 0] = LOW32(next_desc);
      sg_desc[d*16 + 1] = HIGH32(next_desc);
      
      sg_desc[d*16 + 2] = LOW32(hw_addr);
      sg_desc[d*16 + 3] = HIGH32(hw_addr); 
      
      sg_desc[d*16 + 4] =  0; // Reserved
      sg_desc[d*16 + 5] =  0; // Reserved
      sg_desc[d*16 + 6] =  ((hw_len         & 0x007FFFFF)   |
                            ((i==0)        ? ctrl_sof : 0 ) |
                            ((i==num_sg-1) ? ctrl_eof : 0 )   );
      sg_desc[d*16 + 7] =  0; // Status
      d++;
    } 
  }
  
  // Store map properties (to be freed by free_sg_buf() )
  sg_map->dev    = this;
  sg_map->dir    = dir;
  sg_map->npages = npages;
  sg_map->pages  = pages;
  sg_map->sgl    = sgl;
  sg_map->num_sg = d; // with merge

  
  return sg_map;
}

static void free_sg_buf(sg_mapping *sg_map){
  dma_unmap_sg(sg_map->dev->dma_dev, sg_map->sgl, sg_map->npages, sg_map->dir);
  release_pinned(sg_map->pages, sg_map->npages);

  kfree(sg_map->sgl);
  kfree(sg_map->pages);
  kfree(sg_map);
}



// ----------------------------------------------------------------------
// Send/Recv

static int zfifo_recv(zfifo_device_data* this,
                      char __user *bufp, unsigned long len){
  sg_mapping *sg_map;
  dma_addr_t head, tail;

  sg_map = alloc_sg_buf(this, bufp, len, DMA_FROM_DEVICE,
                        this->rx_desc, this->rx_phys);

  head = this->rx_phys;
  tail = this->rx_phys + (0x40 * (sg_map->num_sg-1));


  this->dma_regs[S2MM_CURDESC   ] = LOW32 (head);
  this->dma_regs[S2MM_CURDESC_H ] = HIGH32(head);
  this->dma_regs[S2MM_DMACR     ] = DMACR_RS;
  this->dma_regs[S2MM_TAILDESC  ] = LOW32 (tail);
  this->dma_regs[S2MM_TAILDESC_H] = HIGH32(tail);

  dev_dbg(this->sys_dev,
          "Recv DMA regs=%pa user=%pa, len=%ld, head=%pad, tail=%pad\n",
          &this->dma_regs_phys, &bufp, len, &head, &tail);

  // wait 
  while( ~this->dma_regs[S2MM_DMASR] & DMASR_IOC_Irq ){};
  this->dma_regs[S2MM_DMASR] = (DMASR_IOC_Irq | DMASR_ERR_Irq);

  this->dma_regs[S2MM_DMACR] = 0;
  
  free_sg_buf(sg_map);
  return 0;
}


static int zfifo_send(zfifo_device_data* this,
                      char __user *bufp, unsigned long len){
  sg_mapping *sg_map;
  dma_addr_t head, tail;

  sg_map = alloc_sg_buf(this, bufp, len, DMA_TO_DEVICE,
                        this->tx_desc, this->tx_phys);

  head = this->tx_phys;
  tail = this->tx_phys + (0x40 * (sg_map->num_sg-1));


  this->dma_regs[MM2S_CURDESC   ] = LOW32 (head);
  this->dma_regs[MM2S_CURDESC_H ] = HIGH32(head);
  this->dma_regs[MM2S_DMACR     ] = DMACR_RS;
  this->dma_regs[MM2S_TAILDESC  ] = LOW32 (tail);
  this->dma_regs[MM2S_TAILDESC_H] = HIGH32(tail);

  dev_dbg(this->sys_dev,
          "Send DMA regs=%pa user=%pa, len=%ld, head=%pad, tail=%pad\n",
          &this->dma_regs_phys, &bufp, len, &head, &tail);
  
  // wait 
  while( ~this->dma_regs[MM2S_DMASR] & DMASR_IOC_Irq ){};
  this->dma_regs[MM2S_DMASR] = (DMASR_IOC_Irq | DMASR_ERR_Irq);

  // stop 
  this->dma_regs[MM2S_DMACR] = 0;

  
  free_sg_buf(sg_map);
  return 0;
} 

static void zfifo_dmac_reset(zfifo_device_data* this){
  this->dma_regs[MM2S_DMACR] = DMACR_RESET;
  while (this->dma_regs[MM2S_DMACR] & DMACR_RESET);

  this->dma_regs[S2MM_DMACR] = DMACR_RESET;
  while (this->dma_regs[S2MM_DMACR] & DMACR_RESET);

  //   printk("Done AXI DMA Reset\n");
}

// ----------------------------------------------------------------------
// Device file operations

static int zfifo_open(struct inode *inode, struct file *file){
  zfifo_device_data* this;
  int status = 0;

  this = container_of(inode->i_cdev, zfifo_device_data, cdev);
  file->private_data = this;
  this->is_open = 1;
  dev_dbg(this->sys_dev, "open: DMA regs at %pa\n", &this->dma_regs_phys);
  
  return status;
}

static int zfifo_release(struct inode *inode, struct file *file){
  zfifo_device_data* this = file->private_data;

  dev_dbg(this->sys_dev, "close: DMA regs at %pa\n", &this->dma_regs_phys);
  this->is_open = 0;

  return 0;
}

static long zfifo_ioctl(struct file *file, unsigned int ioctlnum,
                       unsigned long param){

  zfifo_device_data* this = file->private_data;
  zfifo_io zio;

  // Get user parameters and check them
  if (ioctlnum != IOCTL_RESET){
    int rc;
    if ((rc = copy_from_user(&zio, (void *)param, sizeof(zfifo_io)))) {
      printk(KERN_ERR "zfifo: cannot read ioctl user parameter.\n");
      return rc;
    }

    // Check parameters
    if ((dma_addr_t)zio.data & 0x3){
      printk(KERN_ERR "zfifo: user buffer must be 32bit word aligned.\n");
      return -EINVAL;
    }

    if (zio.len & 0x3){
      printk(KERN_ERR "zfifo: transfer length must be 4n bytes.\n");
      return -EINVAL;
    }

    // no len=0 transger
    if (zio.len == 0) return 0;
  }

  // IOCTLs
  switch(ioctlnum){
  case IOCTL_SEND:
    zfifo_send(this, zio.data, zio.len);
    break;
      
  case IOCTL_RECV:
    zfifo_recv(this, zio.data, zio.len);
    break;

  case IOCTL_RESET:
    dev_dbg(this->sys_dev, "Reset!!\n");
    
    break;
    
  default:
    return -ENOTTY;
  }
  return 0;
}

static const struct file_operations zfifo_file_ops =
  {
   .owner   = THIS_MODULE,
   .open    = zfifo_open,
   .release = zfifo_release,
   .unlocked_ioctl = zfifo_ioctl
};

// ------------------------------------------------------------
// Device Data Operations

static DEFINE_IDA(zfifo_device_ida);
static dev_t      zfifo_device_number  = 0;

static zfifo_device_data*
zfifo_device_create(const char* name, struct device* parent, int minor){
  zfifo_device_data* this     = NULL;
  unsigned int                done     = 0;
  const unsigned int          DONE_ALLOC_MINOR   = (1 << 0);
  const unsigned int          DONE_CHRDEV_ADD    = (1 << 1);
  const unsigned int          DONE_DEVICE_CREATE = (1 << 3);

  int retval;

  // Allocate minor #
  if ((0 <= minor) && (minor < DEVICE_MAX_NUM)) {
    if (ida_simple_get(&zfifo_device_ida, minor, minor+1, GFP_KERNEL) < 0) {
      printk(KERN_ERR "couldn't allocate minor number(=%d).\n", minor);
      goto failed;
    }
  } else if(minor == -1) {
    if ((minor = ida_simple_get(&zfifo_device_ida, 0, DEVICE_MAX_NUM, GFP_KERNEL)) < 0) {
      printk(KERN_ERR "couldn't allocate new minor number. return=%d.\n", minor);
      goto failed;
    }
  } else {
    printk(KERN_ERR "invalid minor number(=%d), valid range is 0 to %d\n", minor, DEVICE_MAX_NUM-1);
    goto failed;
  }
  done |= DONE_ALLOC_MINOR;

  // create device_data
  this = kzalloc(sizeof(*this), GFP_KERNEL);
  if (IS_ERR_OR_NULL(this)) {
    retval = PTR_ERR(this);
    this = NULL;
    printk(KERN_ERR "kzalloc() failed. return=%d\n", retval);
    goto failed;
  }

  // set device #
  this->device_number = MKDEV(MAJOR(zfifo_device_number ), minor);

  // sysfs registration: good to get sys_dev
  if (name == NULL) {
    this->sys_dev = device_create(zfifo_sys_class,
                                  parent,
                                  this->device_number,
                                  (void *)this,
                                  DEVICE_NAME_FORMAT, MINOR(this->device_number));
  } else {
    this->sys_dev = device_create(zfifo_sys_class,
                                  parent,
                                  this->device_number,
                                  (void *)this,
                                  "%s", name);
  }
  if (IS_ERR_OR_NULL(this->sys_dev)) {
    int retval = PTR_ERR(this->sys_dev);
    this->sys_dev = NULL;
    printk(KERN_ERR "device_create() failed. return=%d\n", retval);
    goto failed;
  }
  done |= DONE_DEVICE_CREATE;
    
  // add chdev
  cdev_init(&this->cdev, &zfifo_file_ops);
  this->cdev.owner = THIS_MODULE;
  if ((retval = cdev_add(&this->cdev, this->device_number, 1)) != 0) {
    printk(KERN_ERR "cdev_add() failed. return=%d\n", retval);
    goto failed;
  }
  done |= DONE_CHRDEV_ADD;
        
  // set dma_dev
  if (parent != NULL)
    this->dma_dev = parent;
  else
    this->dma_dev = this->sys_dev;

  return this;

 failed:
  if (done & DONE_CHRDEV_ADD   ) { cdev_del(&this->cdev); }
  if (done & DONE_DEVICE_CREATE) { device_destroy(zfifo_sys_class, this->device_number);}
  if (done & DONE_ALLOC_MINOR  ) { ida_simple_remove(&zfifo_device_ida, minor);}
  if (this != NULL)              { kfree(this); }
  return NULL;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static int zfifo_device_setup(zfifo_device_data* this){
  unsigned tx_offset, rx_offset;
  void *tx, *rx;

  if (!this) return -ENODEV;

  if (this->dma_dev->dma_mask == NULL)
    this->dma_dev->dma_mask = &this->dma_dev->coherent_dma_mask;

  if (*this->dma_dev->dma_mask == 0){
    if (dma_set_mask(this->dma_dev, DMA_BIT_MASK(dma_mask_bit)) == 0) {
      dma_set_coherent_mask(this->dma_dev, DMA_BIT_MASK(dma_mask_bit));
    } else {
      printk(KERN_WARNING "dma_set_mask(DMA_BIT_MASK(%d)) failed\n", dma_mask_bit);
      dma_set_mask(this->dma_dev, DMA_BIT_MASK(32));
      dma_set_coherent_mask(this->dma_dev, DMA_BIT_MASK(32));
    }
  }
    
  this->tx_desc_base = dma_alloc_coherent(this->dma_dev,  desc_size,
                                          &this->tx_phys_base, GFP_KERNEL);
  this->rx_desc_base = dma_alloc_coherent(this->dma_dev,  desc_size,
                                          &this->rx_phys_base, GFP_KERNEL);

  if ( IS_ERR_OR_NULL(this->tx_desc_base) ||
       IS_ERR_OR_NULL(this->rx_desc_base) ){
    printk(KERN_ERR "zfifo: couldn't alloc TX/RX descriptor buffer\n");
    return -ENOMEM;
  }

  tx_offset = ((unsigned long)this->tx_desc_base & 0x3f);
  rx_offset = ((unsigned long)this->rx_desc_base & 0x3f);

  if (tx_offset != 0) tx_offset = 0x40-tx_offset;
  if (rx_offset != 0) rx_offset = 0x40-rx_offset;

  tx = this->tx_desc_base + tx_offset;
  rx = this->rx_desc_base + rx_offset;

  this->tx_desc = (unsigned*)tx;
  this->rx_desc = (unsigned*)rx;

  this->tx_phys = this->tx_phys_base + tx_offset;
  this->rx_phys = this->rx_phys_base + rx_offset;
  
  return 0;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void zfifo_device_info(zfifo_device_data* this){
 #ifdef __aarch64__
  dev_info(this->sys_dev, "zfifo in 64bit mode\n");
#endif
  dev_info(this->sys_dev, "driver version = %s\n"  , DRIVER_VERSION);
  dev_info(this->sys_dev, "major number   = %d\n"  , MAJOR(this->device_number));
  dev_info(this->sys_dev, "minor number   = %d\n"  , MINOR(this->device_number));
  dev_info(this->sys_dev, "DMA regs       = %pa\n", &this->dma_regs_phys);
  dev_info(this->sys_dev, "Tx descriptors = %pa (phys %pad)",
           &this->tx_desc, &this->tx_phys);
  dev_info(this->sys_dev, "Rx descriptors = %pa (phys %pad)",
           &this->rx_desc, &this->rx_phys);
  
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static int zfifo_device_destroy(zfifo_device_data* this){
  if (!this)
    return -ENODEV;

  iounmap((void*)this->dma_regs);
  release_mem_region((resource_size_t)this->dma_regs_phys, dma_reg_size);
  
  dma_free_coherent(this->dma_dev, desc_size,
                    this->tx_desc_base, this->tx_phys_base);
  dma_free_coherent(this->dma_dev, desc_size,
                    this->rx_desc_base, this->rx_phys_base);

  cdev_del(&this->cdev);
  device_destroy(zfifo_sys_class, this->device_number);
  ida_simple_remove(&zfifo_device_ida, MINOR(this->device_number));

  kfree(this);
  return 0;
}

#define STATIC_DEVICE_NUM   8

// ----------------------------------------------------------------------
// static device list: pdev and DMAC regs

struct zfifo_static_device {
  struct platform_device* pdev;
  dma_addr_t             dmac;  // for 32bit ARM
};

struct zfifo_static_device zfifo_static_device_list[STATIC_DEVICE_NUM] = {};

// ----------------------------------------------------------------------
// Create & remove device

static void zfifo_static_device_create(int id, dma_addr_t dmac){
  struct platform_device* pdev;
  int                     retval = 0;

  printk("create %d %pa\n", id, &dmac);
    
  if ((id < 0) || (id >= STATIC_DEVICE_NUM))
    return;
    
  if (dmac == 0) {
    zfifo_static_device_list[id].pdev = NULL;
    zfifo_static_device_list[id].dmac = 0;
    return;
  }

  printk("alloc\n");
  pdev = platform_device_alloc(DRIVER_NAME, id);
  if (IS_ERR_OR_NULL(pdev)) {
    retval = PTR_ERR(pdev);
    pdev   = NULL;
    printk(KERN_ERR "platform_device_alloc(%s,%d) failed. return=%d\n", DRIVER_NAME, id, retval);
    goto failed;
  }

  retval = platform_device_add(pdev);
  if (retval != 0) {
    dev_err(&pdev->dev, "platform_device_add failed. return=%d\n", retval);
    goto failed;
  }

  zfifo_static_device_list[id].pdev = pdev;
  zfifo_static_device_list[id].dmac = dmac;
  return;

 failed:
  if (pdev != NULL) {
    platform_device_put(pdev);
  }
  zfifo_static_device_list[id].pdev = NULL;
  zfifo_static_device_list[id].dmac = 0;
  return;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void zfifo_static_device_remove(int id){
  if (zfifo_static_device_list[id].pdev != NULL) {
    platform_device_del(zfifo_static_device_list[id].pdev);
    platform_device_put(zfifo_static_device_list[id].pdev);
    zfifo_static_device_list[id].pdev = NULL;
    zfifo_static_device_list[id].dmac = 0;
  }
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

// Find in static device list
static int zfifo_static_device_search(struct platform_device *pdev,
                                      int* pid, unsigned int* pdmac){
  int id;
  int found = 0;

  for (id = 0; id < STATIC_DEVICE_NUM; id++) {
    if ((zfifo_static_device_list[id].pdev != NULL) &&
        (zfifo_static_device_list[id].pdev == pdev)) {
      *pid   = id;
      *pdmac = zfifo_static_device_list[id].dmac;
      found  = 1;
      break;
    }
  }
  return found;
}

#define DEFINE_ZFIFO_STATIC_DEVICE_PARAM(__num)                         \
  static int       zfifo ## __num = 0;                                  \
  module_param(    zfifo ## __num, uint, S_IRUGO);                      \
  MODULE_PARM_DESC(zfifo ## __num, DRIVER_NAME #__num " DMA regs");

#define CALL_ZFIFO_STATIC_DEVICE_CREATE(__num)          \
  zfifo_static_device_create(__num, zfifo ## __num);

DEFINE_ZFIFO_STATIC_DEVICE_PARAM(0);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(1);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(2);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(3);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(4);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(5);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(6);
DEFINE_ZFIFO_STATIC_DEVICE_PARAM(7);

static void zfifo_static_device_create_all(void){
  CALL_ZFIFO_STATIC_DEVICE_CREATE(0);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(1);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(2);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(3);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(4);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(5);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(6);
  CALL_ZFIFO_STATIC_DEVICE_CREATE(7);
}

static void zfifo_static_device_remove_all(void){
  int id;
  for (id = 0; id < STATIC_DEVICE_NUM; id++) {
    zfifo_static_device_remove(id);
  }
}

// ----------------------------------------------------------------------
// Platform driver cleanup, probe and remove

static int zfifo_platform_driver_cleanup(struct platform_device *pdev,
                                         zfifo_device_data *this){
  int retval = 0;

  if (this != NULL) {
    retval = zfifo_device_destroy(this);
    dev_set_drvdata(&pdev->dev, NULL);
    of_reserved_mem_device_release(&pdev->dev);
  } else {
    retval = -ENODEV;
  }
  return retval;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static int zfifo_platform_driver_probe(struct platform_device *pdev){
  int                         retval       = 0;
  int                         of_status    = 0;
  unsigned int                of_u32_value = 0;
  unsigned int                dmac         = 0;
  int                         minor_number = -1;
  zfifo_device_data*          this         = NULL;
  const char*                 device_name  = NULL;

  dev_dbg(&pdev->dev, "driver probe start.\n");

  if (zfifo_static_device_search(pdev, &minor_number, &dmac) == 0) {
    /* 
    // still not 64bit compatible 

    // Not initialized by insmod args, saerch Open Firmware
    of_status = of_property_read_u32(pdev->dev.of_node, "size", &dmac);
    if (of_status != 0) {
      dev_err(&pdev->dev, "invalid property size. status=%d\n", of_status);
      retval = -ENODEV;
      goto failed;
    }

    of_status = of_property_read_u32(pdev->dev.of_node, "minor-number", &of_u32_value);
    minor_number = (of_status == 0) ? of_u32_value : -1;

    device_name = of_get_property(pdev->dev.of_node, "device-name", NULL);
    if (IS_ERR_OR_NULL(device_name)) {
      if (minor_number < 0)
        device_name = dev_name(&pdev->dev);
      else
        device_name = NULL;
    }
    */
  }

  // Dev create
  this = zfifo_device_create(device_name, &pdev->dev, minor_number);
  if (IS_ERR_OR_NULL(this)) {
    retval = PTR_ERR(this);
    dev_err(&pdev->dev, "driver create failed. return=%d.\n", retval);
    this = NULL;
    retval = (retval == 0) ? -EINVAL : retval;
    goto failed;
  }
  dev_set_drvdata(&pdev->dev, this);

  // TODO: this->hogehoge = foobar

  this->dmac_buf_len = (2u << (dmac_buf_bits-1)) - 1;

  // AXI DMA registers
  this->dma_regs_phys = (unsigned*)dmac;
  if (!request_mem_region(dmac, dma_reg_size, "AXI DMA REGS")){
    dev_err(&pdev->dev, "couldn't map AXI DMA registers.\n");
    goto failed;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
  this->dma_regs = ioremap(dmac, dma_reg_size);
#else
  this->dma_regs = ioremap_nocache(dmac, dma_reg_size);
#endif
  
  printk("MM2S_DMASR: 0x%x\n", this->dma_regs[MM2S_DMASR]);
  printk("S2MM_DMASR: 0x%x\n", this->dma_regs[S2MM_DMASR]);
  zfifo_dmac_reset(this);

  // DMA setup
  if (pdev->dev.of_node != NULL) {
    if ((retval=of_reserved_mem_device_init(&pdev->dev)) != 0){
      dev_err(&pdev->dev, "of_reserved_mem_device_init failed. return=%d\n",
              retval);
      goto failed;
    }
  }

  if((retval=of_dma_configure(&pdev->dev, pdev->dev.of_node, true)) !=0){
    dev_err(&pdev->dev, "of_dma_configure failed. return=%d\n", retval);
    goto failed;
  }
  
  // Dev setup
  retval = zfifo_device_setup(this);
  if (retval) {
    dev_err(&pdev->dev, "driver setup failed. return=%d\n", retval);
    goto failed;
  }
    
  if (info_enable) {
    zfifo_device_info(this);
    dev_info(&pdev->dev, "driver installed.\n");
  }
  return 0;

 failed:
  zfifo_platform_driver_cleanup(pdev, this);

  return retval;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static int zfifo_platform_driver_remove(struct platform_device *pdev){
  zfifo_device_data* this   = dev_get_drvdata(&pdev->dev);
  int                         retval = 0;

  dev_dbg(&pdev->dev, "driver remove start.\n");

  retval = zfifo_platform_driver_cleanup(pdev, this);

  if (info_enable) {
    dev_info(&pdev->dev, "driver removed.\n");
  }
  return retval;
}

// Open Firmware device matching table
static struct of_device_id zfifo_of_match[] =
  {
   { .compatible = "osana,zfifo-0.99.0", },
   { /* end of table */}
  };
MODULE_DEVICE_TABLE(of, zfifo_of_match);

static struct platform_driver zfifo_platform_driver =
  {
   .probe  = zfifo_platform_driver_probe,
   .remove = zfifo_platform_driver_remove,
   .driver = {
              .owner = THIS_MODULE,
              .name  = DRIVER_NAME,
              .of_match_table = zfifo_of_match,
              },
  };

// ----------------------------------------------------------------------
// Module load, cleanup

static bool zfifo_platform_driver_registerd = 0;

static void zfifo_module_cleanup(void){
  zfifo_static_device_remove_all();
  if (zfifo_platform_driver_registerd)
    platform_driver_unregister(&zfifo_platform_driver);

  if (zfifo_sys_class != NULL)
    class_destroy(zfifo_sys_class);
  
  if (zfifo_device_number != 0)
    unregister_chrdev_region(zfifo_device_number , 0);
  ida_destroy(&zfifo_device_ida);
}

static int __init zfifo_module_init(void){
  int retval = 0;

  ida_init(&zfifo_device_ida);
      
  retval = alloc_chrdev_region(&zfifo_device_number , 0, 0, DRIVER_NAME);
  if (retval != 0) {
    printk(KERN_ERR "%s: couldn't allocate device major number. return=%d\n", DRIVER_NAME, retval);
    zfifo_device_number = 0;
    goto failed;
  }

  zfifo_sys_class = class_create(THIS_MODULE, DRIVER_NAME);
  if (IS_ERR_OR_NULL(zfifo_sys_class)) {
    retval = PTR_ERR(zfifo_sys_class);
    zfifo_sys_class = NULL;
    printk(KERN_ERR "%s: couldn't create sys class. return=%d\n", DRIVER_NAME, retval);
    retval = (retval == 0) ? -ENOMEM : retval;
    goto failed;
  }

  zfifo_static_device_create_all();

  retval = platform_driver_register(&zfifo_platform_driver);
  if (retval) {
    printk(KERN_ERR "%s: couldn't register platform driver. return=%d\n", DRIVER_NAME, retval);
    zfifo_platform_driver_registerd = 0;
    goto failed;
  } else {
    zfifo_platform_driver_registerd = 1;
  }

  return 0;

 failed:
  zfifo_module_cleanup();
  return retval;
}

static void __exit zfifo_module_exit(void){
  zfifo_module_cleanup();
}

module_init(zfifo_module_init);
module_exit(zfifo_module_exit);
