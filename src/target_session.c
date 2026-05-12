#include "target_session.h"

#include <errno.h>
#include <string.h>

void target_session_init(struct target_session *session,
                         struct rdma_ramdisk *disk,
                         struct rdma_transport_ops *ops,
                         void *transport_ctx)
{
    if (!session) {
        return;
    }

    memset(session, 0, sizeof(*session));
    session->disk = disk;
    session->ops = ops;
    session->transport_ctx = transport_ctx;
}

int target_session_connect(struct target_session *session,
                           const struct target_connect_info *info)
{
    int rc;

    if (!session || !session->ops || !session->ops->connect || !info ||
        !info->endpoint) {
        return -EINVAL;
    }

    rc = session->ops->connect(session->transport_ctx, info->endpoint);
    if (rc == 0) {
        session->connected = 1;
    }
    return rc;
}

void target_session_disconnect(struct target_session *session)
{
    if (!session || !session->connected) {
        return;
    }

    if (session->ops && session->ops->disconnect) {
        session->ops->disconnect(session->transport_ctx);
    }
    session->connected = 0;
}

int target_session_submit_io(struct target_session *session,
                             const struct target_io_request *req)
{
    struct rdma_ramdisk_mapping mapping;
    struct rdma_remote_region remote;
    enum rdma_transfer_direction rdma_direction;
    int rc;

    if (!session || !session->connected || !session->disk || !session->ops ||
        !req) {
        return -EINVAL;
    }

    rc = rdma_ramdisk_map_lba(session->disk, req->lba, req->block_count,
                              &mapping);
    if (rc != 0) {
        return rc;
    }

    if (req->remote_length < mapping.length) {
        return -ERANGE;
    }

    remote.addr = req->remote_addr;
    remote.rkey = req->rkey;
    remote.length = mapping.length;

    switch (req->direction) {
    case TARGET_IO_READ:
        rdma_direction = RDMA_TRANSFER_WRITE_TO_REMOTE;
        break;
    case TARGET_IO_WRITE:
        rdma_direction = RDMA_TRANSFER_READ_FROM_REMOTE;
        break;
    default:
        return -EINVAL;
    }

    return session->ops->submit(session->transport_ctx, rdma_direction,
                                &mapping.sge, &remote);
}
