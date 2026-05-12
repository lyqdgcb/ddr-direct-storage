#ifndef RDMA_RAMDISK_H
#define RDMA_RAMDISK_H

#include <stddef.h>
#include <stdint.h>

#include "rdma_transport.h"

struct rdma_ramdisk;

struct rdma_ramdisk_config {
    uint64_t size_bytes;
    uint32_t logical_block_size;
};

struct rdma_ramdisk_mapping {
    uint64_t byte_offset;
    size_t length;
    void *host_addr;
    struct rdma_local_sge sge;
};

struct rdma_ramdisk *rdma_ramdisk_create(const struct rdma_ramdisk_config *cfg,
                                         struct rdma_transport_ops *ops,
                                         void *transport_ctx);

void rdma_ramdisk_destroy(struct rdma_ramdisk *disk);

int rdma_ramdisk_map_lba(struct rdma_ramdisk *disk,
                         uint64_t lba,
                         uint32_t block_count,
                         struct rdma_ramdisk_mapping *out);

int rdma_ramdisk_fill(struct rdma_ramdisk *disk,
                      uint64_t lba,
                      uint32_t block_count,
                      uint8_t value);

int rdma_ramdisk_verify(struct rdma_ramdisk *disk,
                        uint64_t lba,
                        uint32_t block_count,
                        uint8_t value);

uint64_t rdma_ramdisk_size(const struct rdma_ramdisk *disk);
uint32_t rdma_ramdisk_block_size(const struct rdma_ramdisk *disk);
const struct rdma_registered_mr *rdma_ramdisk_mr(const struct rdma_ramdisk *disk);

#endif
