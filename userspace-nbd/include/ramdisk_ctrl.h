#ifndef RAMDISK_CTRL_H
#define RAMDISK_CTRL_H

#include <stdint.h>

#define RAMDISK_CTRL_MAGIC 0x52444354U
#define RAMDISK_CTRL_VERSION 1U
#define RAMDISK_CTRL_MAX_PAYLOAD 4096U

enum ramdisk_ctrl_opcode {
    RAMDISK_CTRL_QUERY_STATUS = 1,
    RAMDISK_CTRL_URMA_ENABLE = 2,
    RAMDISK_CTRL_URMA_DISABLE = 3,
    RAMDISK_CTRL_PEER_CONNECT = 4,
    RAMDISK_CTRL_PEER_DISCONNECT = 5,
    RAMDISK_CTRL_URMA_TRANSFER = 6,
};

enum ramdisk_urma_direction {
    RAMDISK_TO_HBM = 1,
    HBM_TO_RAMDISK = 2,
};

struct ramdisk_ctrl_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t payload_len;
    uint32_t flags;
    uint64_t request_id;
};

struct ramdisk_ctrl_resp {
    struct ramdisk_ctrl_hdr hdr;
    int32_t status;
    uint32_t payload_len;
};

struct ramdisk_ctrl_status {
    uint64_t size;
    uint32_t block_size;
    uint32_t urma_enabled;
    uint32_t peer_count;
    uint32_t queue_depth;
    uint32_t inflight;
    uint32_t pending;
    uint64_t completed;
    uint64_t failed;
};

struct ramdisk_ctrl_peer_connect {
    uint64_t peer_id;
    uint8_t eid[16];
    uint32_t uasid;
    uint32_t reserved0;
    uint64_t seg_va;
    uint64_t seg_len;
    uint32_t seg_token_id;
    uint32_t jetty_id;
};

struct ramdisk_ctrl_peer_disconnect {
    uint64_t peer_id;
};

struct ramdisk_ctrl_urma_transfer {
    uint64_t peer_id;
    uint64_t request_id;
    uint64_t local_offset;
    uint64_t remote_hbm_addr;
    uint32_t length;
    uint32_t direction;
};

#endif
