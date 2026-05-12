#ifndef RDMA_TRANSPORT_H
#define RDMA_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

enum rdma_transfer_direction {
    RDMA_TRANSFER_WRITE_TO_REMOTE = 1,
    RDMA_TRANSFER_READ_FROM_REMOTE = 2,
};

struct rdma_registered_mr {
    void *addr;
    size_t length;
    uint64_t iova;
    uint32_t lkey;
    void *priv;
};

struct rdma_remote_region {
    uint64_t addr;
    uint32_t rkey;
    size_t length;
};

struct rdma_local_sge {
    uint64_t addr;
    uint32_t lkey;
    size_t length;
};

struct rdma_transport_ops {
    int (*connect)(void *ctx, const char *endpoint);
    void (*disconnect)(void *ctx);

    int (*register_mr)(void *ctx, void *addr, size_t length,
                       struct rdma_registered_mr *out);
    void (*deregister_mr)(void *ctx, struct rdma_registered_mr *mr);

    int (*submit)(void *ctx,
                  enum rdma_transfer_direction direction,
                  const struct rdma_local_sge *local,
                  const struct rdma_remote_region *remote);
};

#endif
