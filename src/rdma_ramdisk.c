#include "rdma_ramdisk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct rdma_ramdisk {
    uint64_t size_bytes;
    uint32_t logical_block_size;
    void *base;
    struct rdma_transport_ops *ops;
    void *transport_ctx;
    struct rdma_registered_mr mr;
};

static int checked_io_range(const struct rdma_ramdisk *disk,
                            uint64_t lba,
                            uint32_t block_count,
                            uint64_t *byte_offset,
                            size_t *length)
{
    uint64_t offset;
    uint64_t bytes;

    if (!disk || !byte_offset || !length || block_count == 0) {
        return -EINVAL;
    }

    if (lba > UINT64_MAX / disk->logical_block_size) {
        return -EOVERFLOW;
    }
    offset = lba * disk->logical_block_size;

    if (block_count > UINT64_MAX / disk->logical_block_size) {
        return -EOVERFLOW;
    }
    bytes = (uint64_t)block_count * disk->logical_block_size;

    if (offset > disk->size_bytes || bytes > disk->size_bytes - offset) {
        return -ERANGE;
    }
    if (bytes > SIZE_MAX) {
        return -EOVERFLOW;
    }

    *byte_offset = offset;
    *length = (size_t)bytes;
    return 0;
}

struct rdma_ramdisk *rdma_ramdisk_create(const struct rdma_ramdisk_config *cfg,
                                         struct rdma_transport_ops *ops,
                                         void *transport_ctx)
{
    struct rdma_ramdisk *disk;
    void *base = NULL;
    int rc;

    if (!cfg || !ops || !ops->register_mr || !ops->deregister_mr ||
        !ops->submit || cfg->size_bytes == 0 ||
        cfg->logical_block_size == 0 ||
        cfg->size_bytes % cfg->logical_block_size != 0 ||
        cfg->size_bytes > SIZE_MAX) {
        errno = EINVAL;
        return NULL;
    }

    disk = calloc(1, sizeof(*disk));
    if (!disk) {
        return NULL;
    }

    rc = posix_memalign(&base, 4096, (size_t)cfg->size_bytes);
    if (rc != 0) {
        free(disk);
        errno = rc;
        return NULL;
    }
    memset(base, 0, (size_t)cfg->size_bytes);

    disk->size_bytes = cfg->size_bytes;
    disk->logical_block_size = cfg->logical_block_size;
    disk->base = base;
    disk->ops = ops;
    disk->transport_ctx = transport_ctx;

    rc = ops->register_mr(transport_ctx, base, (size_t)cfg->size_bytes,
                          &disk->mr);
    if (rc != 0) {
        free(base);
        free(disk);
        errno = -rc;
        return NULL;
    }

    return disk;
}

void rdma_ramdisk_destroy(struct rdma_ramdisk *disk)
{
    if (!disk) {
        return;
    }
    disk->ops->deregister_mr(disk->transport_ctx, &disk->mr);
    free(disk->base);
    free(disk);
}

int rdma_ramdisk_map_lba(struct rdma_ramdisk *disk,
                         uint64_t lba,
                         uint32_t block_count,
                         struct rdma_ramdisk_mapping *out)
{
    uint64_t byte_offset;
    size_t length;
    int rc;

    if (!out) {
        return -EINVAL;
    }

    rc = checked_io_range(disk, lba, block_count, &byte_offset, &length);
    if (rc != 0) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->byte_offset = byte_offset;
    out->length = length;
    out->host_addr = (uint8_t *)disk->base + byte_offset;
    out->sge.addr = disk->mr.iova + byte_offset;
    out->sge.lkey = disk->mr.lkey;
    out->sge.length = length;
    return 0;
}

int rdma_ramdisk_fill(struct rdma_ramdisk *disk,
                      uint64_t lba,
                      uint32_t block_count,
                      uint8_t value)
{
    struct rdma_ramdisk_mapping mapping;
    int rc;

    rc = rdma_ramdisk_map_lba(disk, lba, block_count, &mapping);
    if (rc != 0) {
        return rc;
    }

    memset(mapping.host_addr, value, mapping.length);
    return 0;
}

int rdma_ramdisk_verify(struct rdma_ramdisk *disk,
                        uint64_t lba,
                        uint32_t block_count,
                        uint8_t value)
{
    struct rdma_ramdisk_mapping mapping;
    const uint8_t *ptr;
    int rc;

    rc = rdma_ramdisk_map_lba(disk, lba, block_count, &mapping);
    if (rc != 0) {
        return rc;
    }

    ptr = mapping.host_addr;
    for (size_t i = 0; i < mapping.length; i++) {
        if (ptr[i] != value) {
            return -EIO;
        }
    }
    return 0;
}

uint64_t rdma_ramdisk_size(const struct rdma_ramdisk *disk)
{
    return disk ? disk->size_bytes : 0;
}

uint32_t rdma_ramdisk_block_size(const struct rdma_ramdisk *disk)
{
    return disk ? disk->logical_block_size : 0;
}

const struct rdma_registered_mr *rdma_ramdisk_mr(const struct rdma_ramdisk *disk)
{
    return disk ? &disk->mr : NULL;
}
