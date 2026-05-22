#ifndef RAMDISK_CTRL_CLIENT_H
#define RAMDISK_CTRL_CLIENT_H

#include <stdint.h>

#include "ramdisk_ctrl.h"

int ramdisk_ctrl_transact(const char *sock_path, uint16_t opcode,
                          const void *payload, uint32_t payload_len,
                          void *resp_payload, uint32_t *resp_payload_len,
                          uint64_t request_id);

int ramdisk_ctrl_query_status(const char *sock_path,
                              struct ramdisk_ctrl_status *status);
int ramdisk_ctrl_enable_urma(const char *sock_path);
int ramdisk_ctrl_disable_urma(const char *sock_path);
int ramdisk_ctrl_peer_connect(const char *sock_path,
                              const struct ramdisk_ctrl_peer_connect *peer);
int ramdisk_ctrl_peer_disconnect(const char *sock_path, uint64_t peer_id);
int ramdisk_ctrl_urma_transfer(const char *sock_path,
                               const struct ramdisk_ctrl_urma_transfer *xfer);

int ramdisk_ctrl_parse_eid(const char *text, uint8_t eid[16]);

#endif
