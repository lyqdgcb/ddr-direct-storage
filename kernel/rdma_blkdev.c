/*
 * rdma_blkdev.c - RDMA Ramdisk Block Device Driver
 *
 * Creates a block device backed by RDMA-registered memory.
 * Can be used as NVMe-oF target namespace backend.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/bio.h>
#include <linux/blk-mq.h>
#include <linux/kobject.h>
#include <rdma/ib_verbs.h>

#define DRIVER_NAME    "rdma_blkdev"
#define DRIVER_VERSION "1.0"

static int rdma_blkdev_major;
static struct rdma_blkdev_device *rdma_blkdev_dev;

static unsigned long device_size_mb = 256;
module_param(device_size_mb, ulong, 0444);
MODULE_PARM_DESC(device_size_mb, "Device size in MB (default: 256)");

static unsigned int logical_block_size = 512;
module_param(logical_block_size, uint, 0444);
MODULE_PARM_DESC(logical_block_size, "Logical block size (default: 512)");

static char *ib_dev_name = NULL;
module_param(ib_dev_name, charp, 0444);
MODULE_PARM_DESC(ib_dev_name, "RDMA device name (e.g., mlx5_0)");

struct rdma_blkdev_device {
    struct request_queue *queue;
    struct gendisk *disk;
    struct blk_mq_tag_set tag_set;
    
    void *ramdisk_addr;
    size_t ramdisk_size;
    u64 ramdisk_iova;
    
    struct ib_device *ib_dev;
    struct ib_pd *pd;
    struct ib_mr *mr;
    u32 lkey;
    u32 rkey;
    
    struct ib_dma_mapping_ops dma_ops;
    
    spinlock_t lock;
};

static ssize_t mr_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "lkey=0x%x\nrkey=0x%x\niova=0x%llx\nsize=%zu\naddr=%p\n",
                   bdev->lkey, bdev->rkey, bdev->ramdisk_iova,
                   bdev->ramdisk_size, bdev->ramdisk_addr);
}

static ssize_t lkey_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "0x%x\n", bdev->lkey);
}

static ssize_t rkey_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "0x%x\n", bdev->rkey);
}

static ssize_t iova_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "0x%llx\n", bdev->ramdisk_iova);
}

static ssize_t ramdisk_addr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "%p\n", bdev->ramdisk_addr);
}

static ssize_t ramdisk_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct rdma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "%zu\n", bdev->ramdisk_size);
}

static DEVICE_ATTR(mr_info, 0444, mr_info_show, NULL);
static DEVICE_ATTR(lkey, 0444, lkey_show, NULL);
static DEVICE_ATTR(rkey, 0444, rkey_show, NULL);
static DEVICE_ATTR(iova, 0444, iova_show, NULL);
static DEVICE_ATTR(ramdisk_addr, 0444, ramdisk_addr_show, NULL);
static DEVICE_ATTR(ramdisk_size, 0444, ramdisk_size_show, NULL);

static struct attribute *rdma_blkdev_attrs[] = {
    &dev_attr_mr_info.attr,
    &dev_attr_lkey.attr,
    &dev_attr_rkey.attr,
    &dev_attr_iova.attr,
    &dev_attr_ramdisk_addr.attr,
    &dev_attr_ramdisk_size.attr,
    NULL,
};

static const struct attribute_group rdma_blkdev_attr_group = {
    .attrs = rdma_blkdev_attrs,
};

static const struct attribute_group *rdma_blkdev_attr_groups[] = {
    &rdma_blkdev_attr_group,
    NULL,
};

static int rdma_blkdev_open(struct block_device *bdev, fmode_t mode)
{
    return 0;
}

static void rdma_blkdev_release(struct gendisk *disk, fmode_t mode)
{
}

static int rdma_blkdev_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    geo->heads = 4;
    geo->sectors = 16;
    geo->cylinders = (get_capacity(bdev->bd_disk) >> 4) >> 4;
    return 0;
}

static const struct block_device_operations rdma_blkdev_fops = {
    .owner      = THIS_MODULE,
    .open       = rdma_blkdev_open,
    .release    = rdma_blkdev_release,
    .getgeo     = rdma_blkdev_getgeo,
};

static void rdma_blkdev_complete_request(struct request *rq, blk_status_t status)
{
    blk_mq_end_request(rq, status);
}

static blk_status_t rdma_blkdev_queue_rq(struct blk_mq_hw_ctx *hctx,
                                          const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct rdma_blkdev_device *dev = rq->q->queuedata;
    struct bio_vec bvec;
    struct req_iterator iter;
    sector_t sector;
    void *base_addr;
    blk_status_t status = BLK_STS_OK;

    blk_mq_start_request(rq);

    sector = blk_rq_pos(rq);
    base_addr = dev->ramdisk_addr + (sector * logical_block_size);

    if ((sector * logical_block_size) + blk_rq_bytes(rq) > dev->ramdisk_size) {
        pr_err(DRIVER_NAME ": request beyond device size\n");
        rdma_blkdev_complete_request(rq, BLK_STS_IOERR);
        return BLK_STS_IOERR;
    }

    rq_for_each_segment(bvec, rq, iter) {
        void *bio_addr = kmap_atomic(bvec.bv_page) + bvec.bv_offset;
        void *disk_addr = base_addr;

        if (rq_data_dir(rq) == WRITE) {
            memcpy(disk_addr, bio_addr, bvec.bv_len);
        } else {
            memcpy(bio_addr, disk_addr, bvec.bv_len);
        }

        kunmap_atomic(bio_addr);
        base_addr += bvec.bv_len;
    }

    rdma_blkdev_complete_request(rq, status);
    return BLK_STS_OK;
}

static const struct blk_mq_ops rdma_blkdev_mq_ops = {
    .queue_rq = rdma_blkdev_queue_rq,
};

static void rdma_blkdev_init_tag_set(struct rdma_blkdev_device *dev)
{
    memset(&dev->tag_set, 0, sizeof(dev->tag_set));
    dev->tag_set.ops = &rdma_blkdev_mq_ops;
    dev->tag_set.nr_hw_queues = 1;
    dev->tag_set.queue_depth = 128;
    dev->tag_set.numa_node = NUMA_NO_NODE;
    dev->tag_set.cmd_size = 0;
    dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
    dev->tag_set.driver_data = dev;
}

static int rdma_blkdev_setup_rdma(struct rdma_blkdev_device *dev)
{
    struct ib_device *ibdev = NULL;
    struct ib_device **dev_list = NULL;
    int ret, i, count;

    if (!ib_dev_name) {
        count = ib_device_get_by_netdev(NULL, RDMA_DRIVER_UNKNOWN);
        if (count <= 0) {
            pr_warn(DRIVER_NAME ": No RDMA device available, using memory-only mode\n");
            dev->ib_dev = NULL;
            dev->pd = NULL;
            dev->mr = NULL;
            dev->lkey = 0;
            dev->rkey = 0;
            dev->ramdisk_iova = virt_to_phys(dev->ramdisk_addr);
            return 0;
        }

        dev_list = kmalloc(sizeof(struct ib_device *) * count, GFP_KERNEL);
        if (!dev_list)
            return -ENOMEM;

        rcu_read_lock();
        ib_device_get_by_netdev(NULL, RDMA_DRIVER_UNKNOWN);
        for (i = 0; i < count; i++) {
            ibdev = ib_device_get_by_netdev(NULL, RDMA_DRIVER_UNKNOWN);
            if (ibdev) {
                dev->ib_dev = ibdev;
                break;
            }
        }
        rcu_read_unlock();

        kfree(dev_list);

        if (!ibdev) {
            pr_warn(DRIVER_NAME ": No RDMA device found\n");
            dev->ramdisk_iova = virt_to_phys(dev->ramdisk_addr);
            return 0;
        }
    } else {
        ibdev = ib_device_get_by_name(ib_dev_name);
        if (!ibdev) {
            pr_err(DRIVER_NAME ": RDMA device '%s' not found\n", ib_dev_name);
            return -ENODEV;
        }
        dev->ib_dev = ibdev;
    }

    pr_info(DRIVER_NAME ": Using RDMA device: %s\n", dev->ib_dev->name);

    dev->pd = ib_alloc_pd(dev->ib_dev, IB_PD_UNSAFE_GLOBAL_RKEY);
    if (IS_ERR(dev->pd)) {
        ret = PTR_ERR(dev->pd);
        pr_err(DRIVER_NAME ": Failed to allocate PD: %d\n", ret);
        dev->pd = NULL;
        goto err_put_dev;
    }

    dev->mr = ib_get_dma_mr(dev->pd, IB_ACCESS_LOCAL_WRITE | 
                            IB_ACCESS_REMOTE_READ | IB_ACCESS_REMOTE_WRITE);
    if (IS_ERR(dev->mr)) {
        ret = PTR_ERR(dev->mr);
        pr_err(DRIVER_NAME ": Failed to get DMA MR: %d\n", ret);
        dev->mr = NULL;
        goto err_dealloc_pd;
    }

    dev->lkey = dev->mr->lkey;
    dev->rkey = dev->mr->rkey;
    dev->ramdisk_iova = ib_dma_map_single(dev->ib_dev, dev->ramdisk_addr,
                                           dev->ramdisk_size, DMA_BIDIRECTIONAL);

    if (ib_dma_mapping_error(dev->ib_dev, dev->ramdisk_iova)) {
        pr_err(DRIVER_NAME ": DMA mapping failed\n");
        ret = -EIO;
        goto err_dereg_mr;
    }

    pr_info(DRIVER_NAME ": RDMA ready: iova=0x%llx, lkey=0x%x, rkey=0x%x\n",
            dev->ramdisk_iova, dev->lkey, dev->rkey);

    return 0;

err_dereg_mr:
    ib_dereg_mr(dev->mr);
    dev->mr = NULL;
err_dealloc_pd:
    ib_dealloc_pd(dev->pd);
    dev->pd = NULL;
err_put_dev:
    ib_device_put(dev->ib_dev);
    dev->ib_dev = NULL;
    return ret;
}

static void rdma_blkdev_cleanup_rdma(struct rdma_blkdev_device *dev)
{
    if (dev->ib_dev && dev->ramdisk_iova) {
        ib_dma_unmap_single(dev->ib_dev, dev->ramdisk_iova,
                            dev->ramdisk_size, DMA_BIDIRECTIONAL);
        dev->ramdisk_iova = 0;
    }
    if (dev->mr) {
        ib_dereg_mr(dev->mr);
        dev->mr = NULL;
    }
    if (dev->pd) {
        ib_dealloc_pd(dev->pd);
        dev->pd = NULL;
    }
    if (dev->ib_dev) {
        ib_device_put(dev->ib_dev);
        dev->ib_dev = NULL;
    }
}

static int rdma_blkdev_alloc_disk(struct rdma_blkdev_device *dev)
{
    int ret;

    rdma_blkdev_init_tag_set(dev);

    ret = blk_mq_alloc_tag_set(&dev->tag_set);
    if (ret) {
        pr_err(DRIVER_NAME ": Failed to allocate tag set: %d\n", ret);
        return ret;
    }

    dev->disk = blk_mq_alloc_disk(&dev->tag_set, dev);
    if (IS_ERR(dev->disk)) {
        pr_err(DRIVER_NAME ": Failed to allocate disk\n");
        ret = PTR_ERR(dev->disk);
        goto err_free_tag_set;
    }

    dev->queue = dev->disk->queue;
    dev->queue->queuedata = dev;

    dev->disk->major = rdma_blkdev_major;
    dev->disk->first_minor = 0;
    dev->disk->minors = 1;
    dev->disk->fops = &rdma_blkdev_fops;
    dev->disk->private_data = dev;
    dev->disk->groups = rdma_blkdev_attr_groups;
    snprintf(dev->disk->disk_name, 32, DRIVER_NAME);

    blk_queue_logical_block_size(dev->queue, logical_block_size);
    blk_queue_physical_block_size(dev->queue, logical_block_size);
    blk_queue_max_hw_sectors(dev->queue, 1024);
    blk_queue_flag_set(QUEUE_FLAG_NONROT, dev->queue);

    set_capacity(dev->disk, dev->ramdisk_size / logical_block_size);

    ret = add_disk(dev->disk);
    if (ret) {
        pr_err(DRIVER_NAME ": Failed to add disk: %d\n", ret);
        goto err_cleanup_disk;
    }

    pr_info(DRIVER_NAME ": Block device /dev/%s created\n", dev->disk->disk_name);

    return 0;

err_cleanup_disk:
    put_disk(dev->disk);
err_free_tag_set:
    blk_mq_free_tag_set(&dev->tag_set);
    return ret;
}

static void rdma_blkdev_free_disk(struct rdma_blkdev_device *dev)
{
    if (dev->disk) {
        del_gendisk(dev->disk);
        put_disk(dev->disk);
        dev->disk = NULL;
    }
    blk_mq_free_tag_set(&dev->tag_set);
}

static int rdma_blkdev_alloc_ramdisk(struct rdma_blkdev_device *dev)
{
    dev->ramdisk_size = (size_t)device_size_mb << 20;

    dev->ramdisk_addr = vmalloc_user(dev->ramdisk_size);
    if (!dev->ramdisk_addr) {
        pr_err(DRIVER_NAME ": Failed to allocate %zu bytes\n", dev->ramdisk_size);
        return -ENOMEM;
    }

    memset(dev->ramdisk_addr, 0, dev->ramdisk_size);
    pr_info(DRIVER_NAME ": Allocated ramdisk: %zu MB at %p\n",
            dev->ramdisk_size >> 20, dev->ramdisk_addr);

    return 0;
}

static void rdma_blkdev_free_ramdisk(struct rdma_blkdev_device *dev)
{
    if (dev->ramdisk_addr) {
        vfree(dev->ramdisk_addr);
        dev->ramdisk_addr = NULL;
        dev->ramdisk_size = 0;
    }
}

static int __init rdma_blkdev_init(void)
{
    int ret;

    pr_info(DRIVER_NAME ": %s loading (size=%luMB, block_size=%u)\n",
            DRIVER_VERSION, device_size_mb, logical_block_size);

    rdma_blkdev_major = register_blkdev(0, DRIVER_NAME);
    if (rdma_blkdev_major < 0) {
        pr_err(DRIVER_NAME ": Failed to register blkdev: %d\n", rdma_blkdev_major);
        return rdma_blkdev_major;
    }

    rdma_blkdev_dev = kzalloc(sizeof(*rdma_blkdev_dev), GFP_KERNEL);
    if (!rdma_blkdev_dev) {
        ret = -ENOMEM;
        goto err_unregister;
    }

    spin_lock_init(&rdma_blkdev_dev->lock);

    ret = rdma_blkdev_alloc_ramdisk(rdma_blkdev_dev);
    if (ret) {
        goto err_free_dev;
    }

    ret = rdma_blkdev_setup_rdma(rdma_blkdev_dev);
    if (ret) {
        pr_warn(DRIVER_NAME ": RDMA setup failed (%d), continuing without RDMA\n", ret);
        ret = 0;
    }

    ret = rdma_blkdev_alloc_disk(rdma_blkdev_dev);
    if (ret) {
        goto err_cleanup_rdma;
    }

    pr_info(DRIVER_NAME ": Device ready\n");
    pr_info(DRIVER_NAME ":   Device:    /dev/%s\n", rdma_blkdev_dev->disk->disk_name);
    pr_info(DRIVER_NAME ":   Size:      %zu MB\n", rdma_blkdev_dev->ramdisk_size >> 20);
    pr_info(DRIVER_NAME ":   Block:     %u bytes\n", logical_block_size);
    if (rdma_blkdev_dev->ib_dev) {
        pr_info(DRIVER_NAME ":   RDMA Dev:  %s\n", rdma_blkdev_dev->ib_dev->name);
        pr_info(DRIVER_NAME ":   IOVA:      0x%llx\n", rdma_blkdev_dev->ramdisk_iova);
        pr_info(DRIVER_NAME ":   LKey:      0x%x\n", rdma_blkdev_dev->lkey);
        pr_info(DRIVER_NAME ":   RKey:      0x%x\n", rdma_blkdev_dev->rkey);
    }

    return 0;

err_cleanup_rdma:
    rdma_blkdev_cleanup_rdma(rdma_blkdev_dev);
    rdma_blkdev_free_ramdisk(rdma_blkdev_dev);
err_free_dev:
    kfree(rdma_blkdev_dev);
    rdma_blkdev_dev = NULL;
err_unregister:
    unregister_blkdev(rdma_blkdev_major, DRIVER_NAME);
    return ret;
}

static void __exit rdma_blkdev_exit(void)
{
    if (rdma_blkdev_dev) {
        rdma_blkdev_free_disk(rdma_blkdev_dev);
        rdma_blkdev_cleanup_rdma(rdma_blkdev_dev);
        rdma_blkdev_free_ramdisk(rdma_blkdev_dev);
        kfree(rdma_blkdev_dev);
        rdma_blkdev_dev = NULL;
    }

    unregister_blkdev(rdma_blkdev_major, DRIVER_NAME);
    pr_info(DRIVER_NAME ": Unloaded\n");
}

module_init(rdma_blkdev_init);
module_exit(rdma_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DDR Direct Storage");
MODULE_DESCRIPTION("RDMA-enabled Ramdisk Block Device");
MODULE_VERSION(DRIVER_VERSION);

/*
 * Usage:
 * 
 * 1. Load module:
 *    sudo insmod rdma_blkdev.ko device_size_mb=1024 ib_dev_name=mlx5_0
 * 
 * 2. Check device:
 *    lsblk
 *    ls -l /dev/rdma_blkdev
 * 
 * 3. Read MR info from sysfs:
 *    cat /sys/block/rdma_blkdev/mr_info
 *    cat /sys/block/rdma_blkdev/lkey
 *    cat /sys/block/rdma_blkdev/iova
 * 
 * 4. Configure as NVMe-oF namespace (nvmet):
 *    cd /sys/kernel/config/nvmet/subsystems
 *    mkdir mytarget
 *    mkdir mytarget/namespaces/1
 *    echo -n /dev/rdma_blkdev > mytarget/namespaces/1/device_path
 *    echo 1 > mytarget/namespaces/1/enable
 *    echo 1 > mytarget/attr_allow_any_port
 *    cd /sys/kernel/config/nvmet/ports
 *    mkdir 1
 *    echo rdma > 1/addr_trtype
 *    echo mlx5_0 > 1/addr_traddr   # RDMA device IP or name
 *    echo 4420 > 1/addr_trsvcid
 *    ln -s ../subsystems/mytarget 1/subsystems/mytarget
 * 
 * 5. Test from host:
 *    nvme connect -t rdma -a <target_ip> -s 4420 -n mytarget
 *    nvme list
 */