#ifndef RAMDISK_URMA_H
#define RAMDISK_URMA_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "ramdisk_backend.h"
#include "ramdisk_ctrl.h"

#ifdef HAVE_URMA
#include "urma_api.h"
#endif

#define RAMDISK_URMA_MAX_PEERS 32U
#define RAMDISK_URMA_MAX_QUEUE_DEPTH 1024U

struct ramdisk_urma_peer {
    bool active;
    uint64_t peer_id;
    uint8_t eid[16];
    uint32_t uasid;
    uint64_t seg_va;
    uint64_t seg_len;
    uint32_t seg_token_id;
    uint32_t jetty_id;
    void *mock_hbm;
#ifdef HAVE_URMA
    urma_seg_t remote_seg;
    urma_target_seg_t *import_tseg;
    urma_target_jetty_t *t_jetty;
#endif
};

struct ramdisk_urma_request {
    uint64_t peer_id;
    uint64_t request_id;
    uint64_t local_offset;
    uint64_t remote_hbm_addr;
    uint32_t length;
    uint32_t direction;
    int status;
    bool done;
    pthread_cond_t done_cond;
};

struct ramdisk_urma_mgr {
    struct ramdisk_backend *backend;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_t worker;
    bool initialized;
    bool worker_started;
    bool enabled;
    bool use_mock;
    bool stopping;
    char urma_dev[64];
    uint32_t eid_index;
    uint32_t trans_mode;
    uint32_t tp_type;
    uint32_t local_token_value;
    uint32_t queue_depth;
    struct ramdisk_urma_request **queue;
    uint32_t head;
    uint32_t tail;
    uint32_t pending;
    uint32_t inflight;
    uint64_t completed;
    uint64_t failed;
    struct ramdisk_urma_peer peers[RAMDISK_URMA_MAX_PEERS];
#ifdef HAVE_URMA
    bool real_initialized;
    urma_context_t *urma_ctx;
    urma_device_attr_t dev_attr;
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jfr_t *jfr;
    urma_jetty_t *jetty;
    urma_token_t token;
    urma_target_seg_t *local_tseg;
#endif
};

struct ramdisk_urma_config {
    bool enable;
    bool use_mock;
    uint32_t queue_depth;
    const char *urma_dev;
    uint32_t eid_index;
    uint32_t trans_mode;
    uint32_t tp_type;
    uint32_t local_token_value;
};

int ramdisk_urma_mgr_init(struct ramdisk_urma_mgr *mgr,
                          struct ramdisk_backend *backend,
                          const struct ramdisk_urma_config *config);
void ramdisk_urma_mgr_destroy(struct ramdisk_urma_mgr *mgr);

int ramdisk_urma_enable(struct ramdisk_urma_mgr *mgr);
int ramdisk_urma_disable(struct ramdisk_urma_mgr *mgr);
int ramdisk_urma_peer_connect(struct ramdisk_urma_mgr *mgr,
                              const struct ramdisk_ctrl_peer_connect_info *info);
int ramdisk_urma_peer_disconnect(struct ramdisk_urma_mgr *mgr,
                                 uint64_t peer_id);
int ramdisk_urma_transfer_sync(struct ramdisk_urma_mgr *mgr,
                               const struct ramdisk_ctrl_urma_transfer *xfer,
                               uint32_t timeout_ms);
void ramdisk_urma_fill_status(struct ramdisk_urma_mgr *mgr,
                              struct ramdisk_ctrl_status *status);

int ramdisk_urma_mock_write_peer(struct ramdisk_urma_mgr *mgr,
                                 uint64_t peer_id, uint64_t remote_addr,
                                 const void *buf, uint32_t len);
int ramdisk_urma_mock_read_peer(struct ramdisk_urma_mgr *mgr,
                                uint64_t peer_id, uint64_t remote_addr,
                                void *buf, uint32_t len);

#endif
