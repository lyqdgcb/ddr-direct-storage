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
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <ub/urma/ubcore_types.h>
#include <ub/urma/ubcore_uapi.h>
#include <linux/urma_blkdev.h>

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

static unsigned int queue_depth = 1024;
module_param(queue_depth, uint, 0444);
MODULE_PARM_DESC(queue_depth, "URMA direct IO queue depth (default: 1024)");

struct urma_blkdev_io_req {
    struct list_head list;
    struct urma_blkdev_device *dev;
    struct urma_blkdev_peer peer;
    struct urma_blkdev_io io;

    u64 byte_offset;
    u64 local_ubva;
    size_t len;

    u64 wr_id;
    urma_blkdev_done_fn done;
    void *priv;
    refcount_t ref;
};

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

    spinlock_t pending_lock;
    struct list_head pending_list;

    struct xarray inflight_reqs;

    struct workqueue_struct *io_wq;
    struct work_struct submit_work;

    atomic_t pending;
    atomic_t inflight;
    atomic64_t next_wr_id;
    unsigned int queue_depth;
    bool stopping;
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

static ssize_t direct_io_queue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct urma_blkdev_device *bdev = dev_get_drvdata(dev);

    if (!bdev)
        return sprintf(buf, "device not ready\n");

    return sprintf(buf, "queue_depth=%u\npending=%d\ninflight=%d\nstopping=%u\nnext_wr_id=%lld\n",
                   bdev->queue_depth,
                   atomic_read(&bdev->pending),
                   atomic_read(&bdev->inflight),
                   READ_ONCE(bdev->stopping) ? 1 : 0,
                   (long long)atomic64_read(&bdev->next_wr_id));
}

static DEVICE_ATTR(seg_info, 0444, seg_info_show, NULL);
static DEVICE_ATTR(ubva, 0444, ubva_show, NULL);
static DEVICE_ATTR(token_id, 0444, token_id_show, NULL);
static DEVICE_ATTR(seg_len, 0444, seg_len_show, NULL);
static DEVICE_ATTR(ramdisk_addr, 0444, ramdisk_addr_show, NULL);
static DEVICE_ATTR(ramdisk_size, 0444, ramdisk_size_show, NULL);
static DEVICE_ATTR(ub_dev_name, 0444, ub_dev_name_show, NULL);
static DEVICE_ATTR(direct_io_queue, 0444, direct_io_queue_show, NULL);

static struct attribute *urma_blkdev_attrs[] = {
    &dev_attr_seg_info.attr,
    &dev_attr_ubva.attr,
    &dev_attr_token_id.attr,
    &dev_attr_seg_len.attr,
    &dev_attr_ramdisk_addr.attr,
    &dev_attr_ramdisk_size.attr,
    &dev_attr_ub_dev_name.attr,
    &dev_attr_direct_io_queue.attr,
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

static void urma_blkdev_req_put(struct urma_blkdev_io_req *req)
{
    if (!req)
        return;
    if (refcount_dec_and_test(&req->ref)) {
        module_put(THIS_MODULE);
        kfree(req);
    }
}

static const char *urma_blkdev_io_dir_name(enum urma_blkdev_io_dir dir)
{
    switch (dir) {
    case URMA_BLKDEV_IO_READ:
        return "READ ramdisk->HBM";
    case URMA_BLKDEV_IO_WRITE:
        return "WRITE HBM->ramdisk";
    default:
        return "UNKNOWN";
    }
}

static struct urma_blkdev_device *urma_blkdev_dev_from_bdev(struct block_device *bdev)
{
    if (bdev && bdev->bd_disk)
        return bdev->bd_disk->private_data;

    /*
     * The current module only creates one ramdisk. Allow NULL bdev for early
     * bring-up tests, but keep this path explicit in logs at call sites.
     */
    return urma_blkdev_dev;
}

static int urma_blkdev_map_io(struct urma_blkdev_device *dev,
                              const struct urma_blkdev_io *io,
                              u64 *byte_offset,
                              size_t *len,
                              u64 *local_ubva)
{
    u64 offset;
    u64 bytes;

    if (!dev || !io || !byte_offset || !len || !local_ubva)
        return -EINVAL;
    if (!logical_block_size || !io->block_count)
        return -EINVAL;
    if (io->lba > U64_MAX / logical_block_size)
        return -EOVERFLOW;
    if ((u64)io->block_count > U64_MAX / logical_block_size)
        return -EOVERFLOW;

    offset = io->lba * logical_block_size;
    bytes = (u64)io->block_count * logical_block_size;

    if (offset > dev->ramdisk_size || bytes > dev->ramdisk_size - offset)
        return -ERANGE;
    if (bytes > SIZE_MAX)
        return -EOVERFLOW;
    if (dev->ubva_addr > U64_MAX - offset)
        return -EOVERFLOW;

    *byte_offset = offset;
    *len = (size_t)bytes;
    *local_ubva = dev->ubva_addr + offset;
    return 0;
}

static void urma_blkdev_done_req(struct urma_blkdev_io_req *req, int status)
{
    if (!req)
        return;

    pr_debug(DRIVER_NAME ": complete wr_id=%llu status=%d lba=%llu blocks=%u len=%zu\n",
             (unsigned long long)req->wr_id, status,
             (unsigned long long)req->io.lba, req->io.block_count, req->len);

    if (req->done)
        req->done(req->priv, status);

    urma_blkdev_req_put(req);
}

static int urma_blkdev_post_jetty_wr(struct urma_blkdev_io_req *req)
{
    /*
     * This is the only place that should touch ubcore jetty WR details.
     *
     * The repository currently has no target-environment ubcore headers that
     * define the exact kernel WR layout for peer HBM access. Keep this
     * function fail-safe until those headers are available: validate all state,
     * emit detailed diagnostics, and return an error instead of dereferencing
     * guessed fields and risking a kernel crash.
     */
    if (!req || !req->dev || !req->dev->seg || !req->peer.jetty)
        return -EINVAL;

    pr_err_ratelimited(DRIVER_NAME
        ": URMA jetty post not wired yet: wr_id=%llu dir=%s lba=%llu blocks=%u len=%zu local_ubva=0x%llx hbm_addr=0x%llx hbm_token=0x%x peer_eid=%16phN jetty=%p\n",
        (unsigned long long)req->wr_id,
        urma_blkdev_io_dir_name(req->io.dir),
        (unsigned long long)req->io.lba,
        req->io.block_count,
        req->len,
        (unsigned long long)req->local_ubva,
        (unsigned long long)req->io.hbm_addr,
        req->io.hbm_token_id,
        req->peer.eid.raw,
        req->peer.jetty);

    return -EOPNOTSUPP;
}

static struct urma_blkdev_io_req *urma_blkdev_pop_pending(struct urma_blkdev_device *dev)
{
    struct urma_blkdev_io_req *req = NULL;
    unsigned long flags;

    spin_lock_irqsave(&dev->pending_lock, flags);
    if (!list_empty(&dev->pending_list)) {
        req = list_first_entry(&dev->pending_list,
                               struct urma_blkdev_io_req, list);
        list_del_init(&req->list);
        atomic_dec(&dev->pending);
    }
    spin_unlock_irqrestore(&dev->pending_lock, flags);
    return req;
}

static void urma_blkdev_submit_workfn(struct work_struct *work)
{
    struct urma_blkdev_device *dev;
    struct urma_blkdev_io_req *req;
    int ret;

    dev = container_of(work, struct urma_blkdev_device, submit_work);

    while (!READ_ONCE(dev->stopping) &&
           atomic_read(&dev->inflight) < dev->queue_depth) {
        req = urma_blkdev_pop_pending(dev);
        if (!req)
            break;

        req->wr_id = (u64)atomic64_inc_return(&dev->next_wr_id);

        ret = xa_err(xa_store(&dev->inflight_reqs,
                              (unsigned long)req->wr_id, req, GFP_KERNEL));
        if (ret) {
            pr_err(DRIVER_NAME ": failed to track inflight req lba=%llu blocks=%u ret=%d\n",
                   (unsigned long long)req->io.lba, req->io.block_count, ret);
            urma_blkdev_done_req(req, ret);
            continue;
        }

        atomic_inc(&dev->inflight);
        pr_debug(DRIVER_NAME ": posting wr_id=%llu dir=%s lba=%llu blocks=%u len=%zu local_ubva=0x%llx hbm=0x%llx\n",
                 (unsigned long long)req->wr_id,
                 urma_blkdev_io_dir_name(req->io.dir),
                 (unsigned long long)req->io.lba, req->io.block_count,
                 req->len, (unsigned long long)req->local_ubva,
                 (unsigned long long)req->io.hbm_addr);

        ret = urma_blkdev_post_jetty_wr(req);
        if (ret) {
            xa_erase(&dev->inflight_reqs, (unsigned long)req->wr_id);
            atomic_dec(&dev->inflight);
            pr_err(DRIVER_NAME ": post failed wr_id=%llu ret=%d\n",
                   (unsigned long long)req->wr_id, ret);
            urma_blkdev_done_req(req, ret);
            continue;
        }

        /*
         * Real URMA completion handling must look up req by wr_id, remove it
         * from inflight_reqs, decrement inflight, and call urma_blkdev_done_req.
         */
    }
}

static void urma_blkdev_fail_queued(struct urma_blkdev_device *dev, int status)
{
    LIST_HEAD(to_complete);
    struct urma_blkdev_io_req *req;
    struct urma_blkdev_io_req *tmp;
    unsigned long flags;
    unsigned long index;
    void *entry;

    if (!dev)
        return;

    spin_lock_irqsave(&dev->pending_lock, flags);
    list_splice_init(&dev->pending_list, &to_complete);
    atomic_set(&dev->pending, 0);
    spin_unlock_irqrestore(&dev->pending_lock, flags);

    index = 0;
    while ((entry = xa_find(&dev->inflight_reqs, &index, ULONG_MAX, XA_PRESENT))) {
        req = entry;
        xa_erase(&dev->inflight_reqs, index);
        list_add_tail(&req->list, &to_complete);
        atomic_dec(&dev->inflight);
        index++;
    }

    list_for_each_entry_safe(req, tmp, &to_complete, list) {
        list_del_init(&req->list);
        pr_warn(DRIVER_NAME ": failing queued req wr_id=%llu lba=%llu blocks=%u status=%d\n",
                (unsigned long long)req->wr_id,
                (unsigned long long)req->io.lba,
                req->io.block_count, status);
        urma_blkdev_done_req(req, status);
    }
}

int urma_blkdev_submit_nvmet_io(struct block_device *bdev,
                                const struct urma_blkdev_peer *peer,
                                const struct urma_blkdev_io *io,
                                urma_blkdev_done_fn done,
                                void *priv)
{
    struct urma_blkdev_device *dev;
    struct urma_blkdev_io_req *req;
    unsigned long flags;
    u64 byte_offset;
    u64 local_ubva;
    size_t len;
    int ret;

    if (!peer || !io || !done) {
        pr_err(DRIVER_NAME ": submit reject: peer=%p io=%p done=%p\n",
               peer, io, done);
        return -EINVAL;
    }

    dev = urma_blkdev_dev_from_bdev(bdev);
    if (!dev) {
        pr_err(DRIVER_NAME ": submit reject: no device for bdev=%p\n", bdev);
        return -ENODEV;
    }

    if (READ_ONCE(dev->stopping)) {
        pr_warn(DRIVER_NAME ": submit reject: device stopping lba=%llu blocks=%u\n",
                (unsigned long long)io->lba, io->block_count);
        return -ESHUTDOWN;
    }

    if (!dev->seg || !dev->ub_dev) {
        pr_err(DRIVER_NAME ": submit reject: URMA not ready ub_dev=%p seg=%p lba=%llu blocks=%u\n",
               dev->ub_dev, dev->seg,
               (unsigned long long)io->lba, io->block_count);
        return -ENODEV;
    }

    if (!dev->io_wq) {
        pr_err(DRIVER_NAME ": submit reject: direct IO workqueue not ready\n");
        return -ENODEV;
    }

    if (!peer->jetty) {
        pr_err(DRIVER_NAME ": submit reject: missing peer jetty lba=%llu blocks=%u hbm=0x%llx token=0x%x\n",
               (unsigned long long)io->lba, io->block_count,
               (unsigned long long)io->hbm_addr, io->hbm_token_id);
        return -EINVAL;
    }

    if (io->dir != URMA_BLKDEV_IO_READ && io->dir != URMA_BLKDEV_IO_WRITE) {
        pr_err(DRIVER_NAME ": submit reject: invalid dir=%d lba=%llu blocks=%u\n",
               io->dir, (unsigned long long)io->lba, io->block_count);
        return -EINVAL;
    }

    ret = urma_blkdev_map_io(dev, io, &byte_offset, &len, &local_ubva);
    if (ret) {
        pr_err(DRIVER_NAME ": submit reject: map failed ret=%d dir=%s lba=%llu blocks=%u ramdisk_size=%zu block_size=%u\n",
               ret, urma_blkdev_io_dir_name(io->dir),
               (unsigned long long)io->lba, io->block_count,
               dev->ramdisk_size, logical_block_size);
        return ret;
    }

    if (atomic_read(&dev->pending) + atomic_read(&dev->inflight) >= dev->queue_depth) {
        pr_warn_ratelimited(DRIVER_NAME ": submit reject: queue full pending=%d inflight=%d depth=%u lba=%llu blocks=%u\n",
                            atomic_read(&dev->pending),
                            atomic_read(&dev->inflight),
                            dev->queue_depth,
                            (unsigned long long)io->lba,
                            io->block_count);
        return -EBUSY;
    }

    if (!try_module_get(THIS_MODULE)) {
        pr_warn(DRIVER_NAME ": submit reject: module ref unavailable\n");
        return -ENODEV;
    }

    req = kzalloc(sizeof(*req), GFP_KERNEL);
    if (!req) {
        module_put(THIS_MODULE);
        pr_err(DRIVER_NAME ": submit reject: request allocation failed\n");
        return -ENOMEM;
    }

    INIT_LIST_HEAD(&req->list);
    refcount_set(&req->ref, 1);
    req->dev = dev;
    req->peer = *peer;
    req->io = *io;
    req->byte_offset = byte_offset;
    req->local_ubva = local_ubva;
    req->len = len;
    req->done = done;
    req->priv = priv;

    spin_lock_irqsave(&dev->pending_lock, flags);
    if (dev->stopping) {
        spin_unlock_irqrestore(&dev->pending_lock, flags);
        pr_warn(DRIVER_NAME ": submit reject after alloc: device stopping lba=%llu blocks=%u\n",
                (unsigned long long)io->lba, io->block_count);
        urma_blkdev_req_put(req);
        return -ESHUTDOWN;
    }
    if (atomic_read(&dev->pending) + atomic_read(&dev->inflight) >= dev->queue_depth) {
        spin_unlock_irqrestore(&dev->pending_lock, flags);
        pr_warn_ratelimited(DRIVER_NAME ": submit reject after alloc: queue full pending=%d inflight=%d depth=%u lba=%llu blocks=%u\n",
                            atomic_read(&dev->pending),
                            atomic_read(&dev->inflight),
                            dev->queue_depth,
                            (unsigned long long)io->lba,
                            io->block_count);
        urma_blkdev_req_put(req);
        return -EBUSY;
    }
    list_add_tail(&req->list, &dev->pending_list);
    atomic_inc(&dev->pending);
    spin_unlock_irqrestore(&dev->pending_lock, flags);

    pr_debug(DRIVER_NAME ": accepted nvmet IO dir=%s lba=%llu blocks=%u len=%zu local_ubva=0x%llx hbm=0x%llx token=0x%x pending=%d inflight=%d\n",
             urma_blkdev_io_dir_name(io->dir),
             (unsigned long long)io->lba, io->block_count, len,
             (unsigned long long)local_ubva,
             (unsigned long long)io->hbm_addr, io->hbm_token_id,
             atomic_read(&dev->pending), atomic_read(&dev->inflight));

    queue_work(dev->io_wq, &dev->submit_work);
    return 0;
}
EXPORT_SYMBOL_GPL(urma_blkdev_submit_nvmet_io);

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
    struct ubcore_seg_cfg seg_cfg;
    union ubcore_token_id_flag token_flag;

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

static int urma_blkdev_init_io_queue(struct urma_blkdev_device *dev)
{
    if (!dev)
        return -EINVAL;

    spin_lock_init(&dev->pending_lock);
    INIT_LIST_HEAD(&dev->pending_list);
    xa_init(&dev->inflight_reqs);
    INIT_WORK(&dev->submit_work, urma_blkdev_submit_workfn);
    atomic_set(&dev->pending, 0);
    atomic_set(&dev->inflight, 0);
    atomic64_set(&dev->next_wr_id, 0);
    dev->queue_depth = queue_depth ? queue_depth : 1;
    WRITE_ONCE(dev->stopping, false);

    dev->io_wq = alloc_workqueue(DRIVER_NAME "_io",
                                 WQ_UNBOUND | WQ_MEM_RECLAIM,
                                 dev->queue_depth);
    if (!dev->io_wq) {
        pr_err(DRIVER_NAME ": failed to allocate direct IO workqueue depth=%u\n",
               dev->queue_depth);
        xa_destroy(&dev->inflight_reqs);
        return -ENOMEM;
    }

    pr_info(DRIVER_NAME ": direct IO queue initialized depth=%u\n",
            dev->queue_depth);
    return 0;
}

static void urma_blkdev_cleanup_io_queue(struct urma_blkdev_device *dev)
{
    if (!dev)
        return;

    WRITE_ONCE(dev->stopping, true);

    if (dev->io_wq) {
        flush_workqueue(dev->io_wq);
        urma_blkdev_fail_queued(dev, -ESHUTDOWN);
        destroy_workqueue(dev->io_wq);
        dev->io_wq = NULL;
    } else {
        urma_blkdev_fail_queued(dev, -ESHUTDOWN);
    }

    xa_destroy(&dev->inflight_reqs);
    pr_info(DRIVER_NAME ": direct IO queue stopped pending=%d inflight=%d\n",
            atomic_read(&dev->pending), atomic_read(&dev->inflight));
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
    dev_set_drvdata(disk_to_dev(dev->disk), dev);
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

    ret = urma_blkdev_init_io_queue(urma_blkdev_dev);
    if (ret) {
        goto err_free_dev;
    }

    ret = urma_blkdev_alloc_ramdisk(urma_blkdev_dev);
    if (ret) {
        goto err_cleanup_queue;
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
err_cleanup_queue:
    urma_blkdev_cleanup_io_queue(urma_blkdev_dev);
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
        urma_blkdev_cleanup_io_queue(urma_blkdev_dev);
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
