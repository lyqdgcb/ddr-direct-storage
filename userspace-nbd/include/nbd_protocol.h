#ifndef RAMDISK_NBD_PROTOCOL_H
#define RAMDISK_NBD_PROTOCOL_H

#include <stdint.h>

#define RAMDISK_NBD_REQUEST_MAGIC 0x25609513U
#define RAMDISK_NBD_REPLY_MAGIC 0x67446698U

#define RAMDISK_NBD_CMD_READ 0U
#define RAMDISK_NBD_CMD_WRITE 1U
#define RAMDISK_NBD_CMD_DISC 2U
#define RAMDISK_NBD_CMD_FLUSH 3U
#define RAMDISK_NBD_CMD_TRIM 4U


struct ramdisk_nbd_request {
    uint32_t magic;
    uint32_t type;
    uint8_t handle[8];
    uint64_t from;
    uint32_t len;
} __attribute__((packed));

struct ramdisk_nbd_reply {
    uint32_t magic;
    uint32_t error;
    uint8_t handle[8];
} __attribute__((packed));

#endif
