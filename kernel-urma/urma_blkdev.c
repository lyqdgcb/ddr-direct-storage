/*
 * urma_blkdev.c - URMA Ramdisk Block Device Driver
 *
 * Creates a block device backed by URMA-registered memory.
 * Can be used as NVMe-oF target namespace backend.
 *
 * URMA (Unified Bus) is Huawei's unified bus protocol for chip-to-chip communication.
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
#include <ub/urma/ubcore_types.h>
#include <ub/urma/ubcore_uapi.h>

#define DRIVER_NAME    "urma_blkdev"
#define DRIVER_VERSION "1.0"

static int urma_blkdev_major;
static struct urma_blkdev_device *urma_blkdev_dev;

static unsigned long device_size_mb = 256;
module_param(device_size_mb, ulong, 0444);
MODULE_PARM_DESC(device_size_mb, "Device size in MB (default: 256)");

static unsigned int logical_block_size = 512;
module_param(logical_block_size, uint, 0444);
MODULE_PARM_DESC(logical_block_size, "Logical block size (default: 512)");

static char *ub_dev_name = NULL;
module_param(ub_dev_name, charp, 0444);
MODULE_PARM_DESC(ub_dev_name, "UB device name (e.g., ub0)");

static uint32_t eid_index = 0;
module_param(eid_index, uint, 0444);
MODULE_PARM_DESC(eid_index, "EID index (default: 0)");

struct urma_blkdev_device {
    struct request_queue *queue;
    struct gendisk *disk;
    struct blk_mq_tag_set tag_set;
    
    void *ramdisk_addr;
    size_t ramdisk_size;
    
    struct ubcore_device *ub_dev;
    struct ubcore_token_id *token_id;
    struct ubcore_target_seg *seg;
    struct ubcore_ucontext *uctx;
    
    uint64_t ubva_addr;
    uint32_t token_id_val;
    
    spinlock_t lock;
};

static ssize_t seg_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    if (!bdev || !bdev->seg)
        return sprintf(buf, "segment not registered\n");
    
    return sprintf(buf, "ubva_eid=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x\n"
                   "ubva_va=0x%llx\n"
                   "len=%zu\n"
                   "token_id=0x%x\n"
                   "addr=%p\n",
                   bdev->seg->seg.ubva.eid.raw[0], bdev->seg->seg.ubva.eid.raw[1],
                   bdev->seg->seg.ubva.eid.raw[2], bdev->seg->seg.ubva.eid.raw[3],
                   bdev->seg->seg.ubva.eid.raw[4], bdev->seg->seg.ubva.eid.raw[5],
                   bdev->seg->seg.ubva.eid.raw[6], bdev->seg->seg.ubva.eid.raw[7],
                   bdev->seg->seg.ubva.eid.raw[8], bdev->seg->seg.ubva.eid.raw[9],
                   bdev->seg->seg.ubva.eid.raw[10], bdev->seg->seg.ubva.eid.raw[11],
                   bdev->seg->seg.ubva.eid.raw[12], bdev->seg->seg.ubva.eid.raw[13],
                   bdev->seg->seg.ubva.eid.raw[14], bdev->seg->seg.ubva.eid.raw[15],
                   (unsigned long long)bdev->seg->seg.ubva.va,
                   bdev->seg->seg.len,
                   bdev->seg->seg.token_id,
                   bdev->ramdisk_addr);
}

static ssize_t ubva_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    if (!bdev || !bdev->seg)
        return sprintf(buf, "0x0\n");
    return sprintf(buf, "0x%llx\n", (unsigned long long)bdev->seg->seg.ubva.va);
}

static ssize_t token_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    if (!bdev || !bdev->seg)
        return sprintf(buf, "0x0\n");
    return sprintf(buf, "0x%x\n", bdev->seg->seg.token_id);
}

static ssize_t seg_len_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    if (!bdev || !bdev->seg)
        return sprintf(buf, "0\n");
    return sprintf(buf, "%zu\n", bdev->seg->seg.len);
}

static ssize_t ramdisk_addr_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "%p\n", bdev ? bdev->ramdisk_addr : NULL);
}

static ssize_t ramdisk_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    return sprintf(buf, "%zu\n", bdev ? bdev->ramdisk_size : 0);
}

static ssize_t ub_dev_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);
    if (!bdev || !bdev->ub_dev)
        return sprintf(buf, "none\n");
    return sprintf(buf, "%s\n", bdev->ub_dev->dev_name);
}

static DEVICE_ATTR(seg_info, 0444, seg_info_show, NULL);
static DEVICE_ATTR(ubva, 0444, ubva_show, NULL);
static DEVICE_ATTR(token_id, 0444, token_id_show, NULL);
static DEVICE_ATTR(seg_len, 0444, seg_len_show, NULL);
static DEVICE_ATTR(ramdisk_addr, 0444, ramdisk_addr_show, NULL);
static DEVICE_ATTR(ramdisk_size, 0444, ramdisk_size_show, NULL);
static DEVICE_ATTR(ub_dev_name, 0444, ub_dev_name_show, NULL);

static struct attribute *urma_blkdev_attrs[] = {
    &dev_attr_seg_info.attr,
    &dev_attr_ubva.attr,
    &dev_attr_token_id.attr,
    &dev_attr_seg_len.attr,
    &dev_attr_ramdisk_addr.attr,
    &dev_attr_ramdisk_size.attr,
    &dev_attr_ub_dev_name.attr,
    NULL,
};

static const struct attribute_group urma_blkdev_attr_group = {
    .attrs = urma_blkdev_attrs,
};

static const struct attribute_group *urma_blkdev_attr_groups[] = {
    &urma_blkdev_attr_group,
    NULL,
};

static int urma_blkdev_open(struct block_device *bdev, fmode_t mode)
{
    return 0;
}

static void urma_blkdev_release(struct gendisk *disk, fmode_t mode)
{
}

static int urma_blkdev_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
    geo->heads = 4;
    geo->sectors = 16;
    geo->cylinders = (get_capacity(bdev->bd_disk) >> 4) >> 4;
    return 0;
}

static const struct block_device_operations urma_blkdev_fops = {
    .owner      = THIS_MODULE,
    .open       = urma_blkdev_open,
    .release    = urma_blkdev_release,
    .getgeo     = urma_blkdev_getgeo,
};

static void urma_blkdev_complete_request(struct request *rq, blk_status_t status)
{
    blk_mq_end_request(rq, status);
}

static blk_status_t urma_blkdev_queue_rq(struct blk_mq_hw_ctx *hctx,
                                          const struct blk_mq_queue_data *bd)
{
    struct request *rq = bd->rq;
    struct urma_blkdev_device *dev = rq->q->queuedata;
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
        urma_blkdev_complete_request(rq, BLK_STS_IOERR);
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

    urma_blkdev_complete_request(rq, status);
    return BLK_STS_OK;
}

static const struct blk_mq_ops urma_blkdev_mq_ops = {
    .queue_rq = urma_blkdev_queue_rq,
};

static void urma_blkdev_init_tag_set(struct urma_blkdev_device *dev)
{
    memset(&dev->tag_set, 0, sizeof(dev->tag_set));
    dev->tag_set.ops = &urma_blkdev_mq_ops;
    dev->tag_set.nr_hw_queues = 1;
    dev->tag_set.queue_depth = 128;
    dev->tag_set.numa_node = NUMA_NO_NODE;
    dev->tag_set.cmd_size = 0;
    dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
    dev->tag_set.driver_data = dev;
}

static int urma_blkdev_setup_urma(struct urma_blkdev_device *dev)
{
    union ubcore_eid eid;
    struct ubcore_seg_cfg seg_cfg;
    union ubcore_token_id_flag token_flag;
    int ret;

    if (!dev->ub_dev) {
        pr_warn(DRIVER_NAME ": No URMA device, using memory-only mode\n");
        dev->ubva_addr = virt_to_phys(dev->ramdisk_addr);
        dev->token_id_val = 0;
        return 0;
    }

    pr_info(DRIVER_NAME ": Using URMA device: %s\n", dev->ub_dev->dev_name);

    token_flag.value = 0;
    token_flag.bs.pa = 0;
    token_flag.bs.multi_seg = 0;
    
    dev->token_id = ubcore_alloc_token_id(dev->ub_dev, token_flag, NULL);
    if (!dev->token_id) {
        pr_err(DRIVER_NAME ": Failed to allocate token_id\n");
        return -ENOMEM;
    }
    dev->token_id_val = dev->token_id->token_id;
    pr_info(DRIVER_NAME ": Allocated token_id: 0x%x\n", dev->token_id_val);

    memset(&seg_cfg, 0, sizeof(seg_cfg));
    seg_cfg.va = (uint64_t)dev->ramdisk_addr;
    seg_cfg.len = dev->ramdisk_size;
    seg_cfg.eid_index = eid_index;
    seg_cfg.token_id = dev->token_id;
    seg_cfg.flag.value = 0;
    seg_cfg.flag.bs.access = UBCORE_ACCESS_LOCAL_WRITE | UBCORE_ACCESS_READ | UBCORE_ACCESS_WRITE;
    seg_cfg.flag.bs.cacheable = 0;
    seg_cfg.flag.bs.non_pin = 0;
    seg_cfg.flag.bs.pa = 0;
    seg_cfg.iova = 0;

    dev->seg = ubcore_register_seg(dev->ub_dev, &seg_cfg, NULL);
    if (!dev->seg) {
        pr_err(DRIVER_NAME ": Failed to register segment\n");
        ubcore_free_token_id(dev->token_id);
        dev->token_id = NULL;
        return -EINVAL;
    }

    dev->ubva_addr = dev->seg->seg.ubva.va;
    
    pr_info(DRIVER_NAME ": Segment registered successfully\n");
    pr_info(DRIVER_NAME ":   UBVA: 0x%llx\n", (unsigned long long)dev->ubva_addr);
    pr_info(DRIVER_NAME ":   Token ID: 0x%x\n", dev->seg->seg.token_id);
    pr_info(DRIVER_NAME ":   Length: %zu bytes\n", dev->seg->seg.len);

    return 0;
}

static void urma_blkdev_cleanup_urma(struct urma_blkdev_device *dev)
{
    if (dev->seg) {
        ubcore_unregister_seg(dev->seg);
        dev->seg = NULL;
    }
    if (dev->token_id) {
        ubcore_free_token_id(dev->token_id);
        dev->token_id = NULL;
    }
    if (dev->ub_dev) {
        ubcore_put_device(dev->ub_dev);
        dev->ub_dev = NULL;
    }
}

static struct ubcore_device *urma_blkdev_find_device(void)
{
    struct ubcore_device *ub_dev = NULL;
    struct net_device *netdev;
    
    if (ub_dev_name) {
        ub_dev = ubcore_get_device_by_name(ub_dev_name);
        if (!ub_dev) {
            pr_err(DRIVER_NAME ": URMA device '%s' not found\n", ub_dev_name);
            return NULL;
        }
        return ub_dev;
    }

    rcu_read_lock();
    for_each_netdev_rcu(&init_net, netdev) {
        ub_dev = ubcore_get_device_by_netdev(netdev, UBCORE_TRANSPORT_UB);
        if (ub_dev) {
            rcu_read_unlock();
            pr_info(DRIVER_NAME ": Found URMA device via netdev %s\n", netdev->name);
            return ub_dev;
        }
    }
    rcu_read_unlock();

    pr_warn(DRIVER_NAME ": No URMA device found\n");
    return NULL;
}

static int urma_blkdev_alloc_disk(struct urma_blkdev_device *dev)
{
    int ret;

    urma_blkdev_init_tag_set(dev);

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

    dev->disk->major = urma_blkdev_major;
    dev->disk->first_minor = 0;
    dev->disk->minors = 1;
    dev->disk->fops = &urma_blkdev_fops;
    dev->disk->private_data = dev;
    dev->disk->groups = urma_blkdev_attr_groups;
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

static void urma_blkdev_free_disk(struct urma_blkdev_device *dev)
{
    if (dev->disk) {
        del_gendisk(dev->disk);
        put_disk(dev->disk);
        dev->disk = NULL;
    }
    blk_mq_free_tag_set(&dev->tag_set);
}

static int urma_blkdev_alloc_ramdisk(struct urma_blkdev_device *dev)
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

static void urma_blkdev_free_ramdisk(struct urma_blkdev_device *dev)
{
    if (dev->ramdisk_addr) {
        vfree(dev->ramdisk_addr);
        dev->ramdisk_addr = NULL;
        dev->ramdisk_size = 0;
    }
}

static int __init urma_blkdev_init(void)
{
    int ret;

    pr_info(DRIVER_NAME ": %s loading (size=%luMB, block_size=%u)\n",
            DRIVER_VERSION, device_size_mb, logical_block_size);

    urma_blkdev_major = register_blkdev(0, DRIVER_NAME);
    if (urma_blkdev_major < 0) {
        pr_err(DRIVER_NAME ": Failed to register blkdev: %d\n", urma_blkdev_major);
        return urma_blkdev_major;
    }

    urma_blkdev_dev = kzalloc(sizeof(*urma_blkdev_dev), GFP_KERNEL);
    if (!urma_blkdev_dev) {
        ret = -ENOMEM;
        goto err_unregister;
    }

    spin_lock_init(&urma_blkdev_dev->lock);

    ret = urma_blkdev_alloc_ramdisk(urma_blkdev_dev);
    if (ret) {
        goto err_free_dev;
    }

    urma_blkdev_dev->ub_dev = urma_blkdev_find_device();

    ret = urma_blkdev_setup_urma(urma_blkdev_dev);
    if (ret) {
        pr_warn(DRIVER_NAME ": URMA setup failed (%d), continuing without URMA\n", ret);
        ret = 0;
    }

    ret = urma_blkdev_alloc_disk(urma_blkdev_dev);
    if (ret) {
        goto err_cleanup_urma;
    }

    pr_info(DRIVER_NAME ": Device ready\n");
    pr_info(DRIVER_NAME ":   Device:    /dev/%s\n", urma_blkdev_dev->disk->disk_name);
    pr_info(DRIVER_NAME ":   Size:      %zu MB\n", urma_blkdev_dev->ramdisk_size >> 20);
    pr_info(DRIVER_NAME ":   Block:     %u bytes\n", logical_block_size);
    if (urma_blkdev_dev->ub_dev) {
        pr_info(DRIVER_NAME ":   URMA Dev:  %s\n", urma_blkdev_dev->ub_dev->dev_name);
        pr_info(DRIVER_NAME ":   UBVA:      0x%llx\n", urma_blkdev_dev->ubva_addr);
        pr_info(DRIVER_NAME ":   Token ID:  0x%x\n", urma_blkdev_dev->token_id_val);
    }

    return 0;

err_cleanup_urma:
    urma_blkdev_cleanup_urma(urma_blkdev_dev);
    urma_blkdev_free_ramdisk(urma_blkdev_dev);
err_free_dev:
    kfree(urma_blkdev_dev);
    urma_blkdev_dev = NULL;
err_unregister:
    unregister_blkdev(urma_blkdev_major, DRIVER_NAME);
    return ret;
}

static void __exit urma_blkdev_exit(void)
{
    if (urma_blkdev_dev) {
        urma_blkdev_free_disk(urma_blkdev_dev);
        urma_blkdev_cleanup_urma(urma_blkdev_dev);
        urma_blkdev_free_ramdisk(urma_blkdev_dev);
        kfree(urma_blkdev_dev);
        urma_blkdev_dev = NULL;
    }

    unregister_blkdev(urma_blkdev_major, DRIVER_NAME);
    pr_info(DRIVER_NAME ": Unloaded\n");
}

module_init(urma_blkdev_init);
module_exit(urma_blkdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("DDR Direct Storage");
MODULE_DESCRIPTION("URMA-enabled Ramdisk Block Device");
MODULE_VERSION(DRIVER_VERSION);

/*
 * Usage:
 * 
 * 1. Load module:
 *    sudo insmod urma_blkdev.ko device_size_mb=1024 ub_dev_name=ub0 eid_index=0
 * 
 * 2. Check device:
 *    lsblk
 *    ls -l /dev/urma_blkdev
 * 
 * 3. Read segment info from sysfs:
 *    cat /sys/block/urma_blkdev/seg_info
 *    cat /sys/block/urma_blkdev/ubva
 *    cat /sys/block/urma_blkdev/token_id
 * 
 * 4. Configure as NVMe-oF namespace (nvmet):
 *    cd /sys/kernel/config/nvmet/subsystems
 *    mkdir mytarget
 *    mkdir mytarget/namespaces/1
 *    echo -n /dev/urma_blkdev > mytarget/namespaces/1/device_path
 *    echo 1 > mytarget/namespaces/1/enable
 * 
 * 5. For URMA direct access:
 *    - Remote side uses ubva + token_id to access memory
 *    - Similar to RDMA rkey + remote_addr pattern
 * 
 * URMA vs RDMA comparison:
 *    RDMA              URMA
 *    ----------------------------------------
 *    ib_mr             ubcore_target_seg
 *    lkey/rkey         token_id
 *    iova              ubva.va
 *    ib_qp             ubcore_jetty
 *    ib_cq             ubcore_jfc
 *    ib_post_send      ubcore_post_jetty_send_wr
 *    ib_poll_cq        ubcore_poll_jfc
 */