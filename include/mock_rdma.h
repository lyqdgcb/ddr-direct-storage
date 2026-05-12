#ifndef MOCK_RDMA_H
#define MOCK_RDMA_H

#include <stddef.h>
#include <stdint.h>

#include "rdma_transport.h"

struct mock_rdma_context {
    uint8_t *remote_memory;
    size_t remote_length;
    int connected;
    unsigned int connect_count;
    unsigned int submit_count;
    enum rdma_transfer_direction last_direction;

    void *registered_addr;
    size_t registered_length;
    uint64_t registered_iova;
    uint32_t registered_lkey;
};

void mock_rdma_context_init(struct mock_rdma_context *ctx,
                            uint8_t *remote_memory,
                            size_t remote_length);

struct rdma_transport_ops *mock_rdma_ops(void);

#endif
