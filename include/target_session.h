#ifndef TARGET_SESSION_H
#define TARGET_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "rdma_ramdisk.h"

enum target_io_direction {
    TARGET_IO_READ = 1,
    TARGET_IO_WRITE = 2,
};

struct target_connect_info {
    const char *endpoint;
};

struct target_io_request {
    enum target_io_direction direction;
    uint64_t lba;
    uint32_t block_count;
    uint64_t remote_addr;
    uint32_t rkey;
    size_t remote_length;
};

struct target_session {
    struct rdma_ramdisk *disk;
    struct rdma_transport_ops *ops;
    void *transport_ctx;
    int connected;
};

void target_session_init(struct target_session *session,
                         struct rdma_ramdisk *disk,
                         struct rdma_transport_ops *ops,
                         void *transport_ctx);

int target_session_connect(struct target_session *session,
                           const struct target_connect_info *info);

void target_session_disconnect(struct target_session *session);

int target_session_submit_io(struct target_session *session,
                             const struct target_io_request *req);

#endif
