#include "mock_rdma.h"

#include <errno.h>
#include <string.h>

#define MOCK_IOVA_BASE 0x100000000ULL
#define MOCK_LKEY 0x51A70001U

static int mock_connect(void *opaque, const char *endpoint)
{
    struct mock_rdma_context *ctx = opaque;

    if (!ctx || !endpoint || endpoint[0] == '\0') {
        return -EINVAL;
    }

    ctx->connected = 1;
    ctx->connect_count++;
    return 0;
}

static void mock_disconnect(void *opaque)
{
    struct mock_rdma_context *ctx = opaque;

    if (ctx) {
        ctx->connected = 0;
    }
}

static int mock_register_mr(void *opaque, void *addr, size_t length,
                            struct rdma_registered_mr *out)
{
    struct mock_rdma_context *ctx = opaque;

    if (!ctx || !addr || length == 0 || !out) {
        return -EINVAL;
    }

    ctx->registered_addr = addr;
    ctx->registered_length = length;
    ctx->registered_iova = MOCK_IOVA_BASE;
    ctx->registered_lkey = MOCK_LKEY;

    out->addr = addr;
    out->length = length;
    out->iova = ctx->registered_iova;
    out->lkey = ctx->registered_lkey;
    out->priv = ctx;
    return 0;
}

static void mock_deregister_mr(void *opaque, struct rdma_registered_mr *mr)
{
    struct mock_rdma_context *ctx = opaque;

    if (!ctx || !mr) {
        return;
    }

    ctx->registered_addr = NULL;
    ctx->registered_length = 0;
    ctx->registered_iova = 0;
    ctx->registered_lkey = 0;
    memset(mr, 0, sizeof(*mr));
}

static int local_sge_to_ptr(struct mock_rdma_context *ctx,
                            const struct rdma_local_sge *local,
                            uint8_t **ptr)
{
    uint64_t offset;

    if (!ctx || !local || !ptr || local->lkey != ctx->registered_lkey ||
        local->addr < ctx->registered_iova) {
        return -EINVAL;
    }

    offset = local->addr - ctx->registered_iova;
    if (offset > ctx->registered_length ||
        local->length > ctx->registered_length - (size_t)offset) {
        return -ERANGE;
    }

    *ptr = (uint8_t *)ctx->registered_addr + offset;
    return 0;
}

static int remote_region_to_ptr(struct mock_rdma_context *ctx,
                                const struct rdma_remote_region *remote,
                                uint8_t **ptr)
{
    if (!ctx || !remote || !ptr || remote->addr > ctx->remote_length ||
        remote->length > ctx->remote_length - (size_t)remote->addr) {
        return -ERANGE;
    }

    *ptr = ctx->remote_memory + remote->addr;
    return 0;
}

static int mock_submit(void *opaque,
                       enum rdma_transfer_direction direction,
                       const struct rdma_local_sge *local,
                       const struct rdma_remote_region *remote)
{
    struct mock_rdma_context *ctx = opaque;
    uint8_t *local_ptr;
    uint8_t *remote_ptr;
    int rc;

    if (!ctx || !ctx->connected || !local || !remote ||
        local->length != remote->length) {
        return -EINVAL;
    }

    rc = local_sge_to_ptr(ctx, local, &local_ptr);
    if (rc != 0) {
        return rc;
    }

    rc = remote_region_to_ptr(ctx, remote, &remote_ptr);
    if (rc != 0) {
        return rc;
    }

    switch (direction) {
    case RDMA_TRANSFER_WRITE_TO_REMOTE:
        memcpy(remote_ptr, local_ptr, local->length);
        break;
    case RDMA_TRANSFER_READ_FROM_REMOTE:
        memcpy(local_ptr, remote_ptr, local->length);
        break;
    default:
        return -EINVAL;
    }

    ctx->submit_count++;
    ctx->last_direction = direction;
    return 0;
}

void mock_rdma_context_init(struct mock_rdma_context *ctx,
                            uint8_t *remote_memory,
                            size_t remote_length)
{
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->remote_memory = remote_memory;
    ctx->remote_length = remote_length;
}

struct rdma_transport_ops *mock_rdma_ops(void)
{
    static struct rdma_transport_ops ops = {
        .connect = mock_connect,
        .disconnect = mock_disconnect,
        .register_mr = mock_register_mr,
        .deregister_mr = mock_deregister_mr,
        .submit = mock_submit,
    };

    return &ops;
}
