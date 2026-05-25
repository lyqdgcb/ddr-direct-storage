#include "ramdisk_urma.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ramdisk_log.h"

#define RAMDISK_URMA_DEFAULT_TIMEOUT_MS 30000U
#define RAMDISK_URMA_DEFAULT_TOKEN 0xACFEU
#define RAMDISK_URMA_POLL_SLEEP_US 1000U

#ifdef HAVE_URMA
static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int parse_eid_text(const char *text, urma_eid_t *eid)
{
    uint8_t raw[16];
    uint32_t nibbles = 0;
    int high = -1;

    if (text == NULL || eid == NULL)
        return -EINVAL;

    memset(raw, 0, sizeof(raw));
    for (; *text != '\0'; text++) {
        int v;

        if (*text == ':' || *text == '-' || isspace((unsigned char)*text))
            continue;

        v = hex_value((unsigned char)*text);
        if (v < 0)
            return -EINVAL;
        if (nibbles >= 32U)
            return -EINVAL;

        if (high < 0) {
            high = v;
        } else {
            raw[nibbles / 2U] = (uint8_t)((high << 4) | v);
            high = -1;
        }
        nibbles++;
    }

    if (nibbles != 32U || high >= 0)
        return -EINVAL;

    memset(eid, 0, sizeof(*eid));
    memcpy(eid->raw, raw, sizeof(raw));
    return 0;
}

static urma_transport_mode_t real_trans_mode(uint32_t mode)
{
    switch (mode) {
    case 1:
        return URMA_TM_RC;
    case 2:
        return URMA_TM_UM;
    case 0:
    default:
        return URMA_TM_RM;
    }
}

static urma_tp_type_t real_tp_type(uint32_t type)
{
    switch (type) {
    case 1:
        return URMA_CTP;
    case 2:
        return URMA_UTP;
    case 0:
    default:
        return URMA_RTP;
    }
}

static int real_select_eid_index(urma_device_t *dev, uint32_t requested,
                                 const urma_eid_t *local_eid)
{
    urma_eid_info_t *eid_list;
    uint32_t eid_cnt = 0;
    uint32_t i;
    int selected = -1;

    eid_list = urma_get_eid_list(dev, &eid_cnt);
    if (eid_list == NULL || eid_cnt == 0)
        return -ENODEV;

    for (i = 0; i < eid_cnt; i++) {
        RD_LOG_INFO("URMA device=%s eid_index=%u", dev->name,
                    eid_list[i].eid_index);
        if (local_eid != NULL &&
            memcmp(eid_list[i].eid.raw, local_eid->raw,
                   sizeof(local_eid->raw)) == 0) {
            if (requested != UINT32_MAX && requested != eid_list[i].eid_index) {
                urma_free_eid_list(eid_list);
                return -EINVAL;
            }
            selected = (int)eid_list[i].eid_index;
            break;
        }
        if (local_eid == NULL && eid_list[i].eid_index == requested)
            selected = (int)requested;
    }
    if (selected < 0 && local_eid == NULL && requested == UINT32_MAX)
        selected = (int)eid_list[0].eid_index;
    urma_free_eid_list(eid_list);
    return selected < 0 ? -ENOENT : selected;
}

static int real_init_provider(struct ramdisk_urma_mgr *mgr)
{
    urma_init_attr_t init_attr = {0};
    urma_eid_t local_eid;
    urma_device_t *dev;
    int eid_index;
    uint32_t depth;

    init_attr.uasid = 0;
    if (urma_init(&init_attr) != URMA_SUCCESS) {
        RD_LOG_ERR("urma_init failed");
        return -EIO;
    }

    if (mgr->urma_dev[0] == '\0') {
        RD_LOG_ERR("real URMA requires --urma-eid EID_HEX");
        urma_uninit();
        return -EINVAL;
    }

    if (parse_eid_text(mgr->urma_dev, &local_eid) != 0) {
        RD_LOG_ERR("invalid --urma-eid format: expected 16-byte hex EID, got '%s'",
                   mgr->urma_dev);
        urma_uninit();
        return -EINVAL;
    }

    dev = urma_get_device_by_eid(local_eid, URMA_TRANSPORT_UB);
    if (dev == NULL) {
        RD_LOG_ERR("urma_get_device_by_eid failed eid=%s", mgr->urma_dev);
        urma_uninit();
        return -ENODEV;
    }
    if (urma_query_device(dev, &mgr->dev_attr) != URMA_SUCCESS) {
        RD_LOG_ERR("urma_query_device failed eid=%s dev=%s", mgr->urma_dev,
                   dev->name);
        urma_uninit();
        return -EIO;
    }

    eid_index = real_select_eid_index(dev, mgr->eid_index, &local_eid);
    if (eid_index < 0) {
        RD_LOG_ERR("no usable URMA EID dev=%s eid=%s requested=%u rc=%d",
                   dev->name, mgr->urma_dev, mgr->eid_index, eid_index);
        urma_uninit();
        return eid_index;
    }
    RD_LOG_INFO("$$$$$ URMA EID dev=%s eid=%s requested=%u rc=%d",
                   dev->name, mgr->urma_dev, mgr->eid_index, eid_index);
    mgr->urma_ctx = urma_create_context(dev, (uint32_t)eid_index);
    if (mgr->urma_ctx == NULL) {
        RD_LOG_ERR("urma_create_context failed dev=%s eid=%s eid_index=%d",
                   dev->name, mgr->urma_dev, eid_index);
        urma_uninit();
        return -EIO;
    }
    mgr->token.token = mgr->local_token_value;

    mgr->jfce = urma_create_jfce(mgr->urma_ctx);
    if (mgr->jfce == NULL) {
        RD_LOG_ERR("urma_create_jfce failed");
        goto fail_ctx;
    }

    depth = mgr->queue_depth * 2U;
    if (depth < 16U)
        depth = 16U;
    if (mgr->dev_attr.dev_cap.max_jfc_depth != 0 &&
        depth > mgr->dev_attr.dev_cap.max_jfc_depth)
        depth = mgr->dev_attr.dev_cap.max_jfc_depth;

    urma_jfc_cfg_t jfc_cfg = {
        .depth = depth,
        .flag = {.value = 0},
        .jfce = mgr->jfce,
        .user_ctx = (uintptr_t)NULL,
    };
    mgr->jfc = urma_create_jfc(mgr->urma_ctx, &jfc_cfg);
    if (mgr->jfc == NULL) {
        RD_LOG_ERR("urma_create_jfc failed depth=%u", depth);
        goto fail_jfce;
    }

    urma_jfr_cfg_t jfr_cfg = {
        .depth = depth,
        .flag.bs.tag_matching = URMA_NO_TAG_MATCHING,
        .trans_mode = URMA_TM_RM,
        .min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER,
        .jfc = mgr->jfc,
        .token_value = mgr->token,
        .id = 0,
        .max_sge = 1,
    };
    mgr->jfr = urma_create_jfr(mgr->urma_ctx, &jfr_cfg);
    if (mgr->jfr == NULL) {
        RD_LOG_ERR("urma_create_jfr failed depth=%u", depth);
        goto fail_jfc;
    }

    urma_jfs_cfg_t jfs_cfg = {
        .depth = depth,
        .trans_mode = URMA_TM_RM,
        .priority = URMA_MAX_PRIORITY,
        .max_sge = 1,
        .max_inline_data = 0,
        .rnr_retry = URMA_TYPICAL_RNR_RETRY,
        .err_timeout = URMA_TYPICAL_ERR_TIMEOUT,
        .jfc = mgr->jfc,
        .user_ctx = (uintptr_t)NULL,
    };
    urma_jetty_cfg_t jetty_cfg = {
        .flag.bs.share_jfr = 1,
        .jfs_cfg = jfs_cfg,
        .shared.jfr = mgr->jfr,
    };
    mgr->jetty = urma_create_jetty(mgr->urma_ctx, &jetty_cfg);
    if (mgr->jetty == NULL) {
        RD_LOG_ERR("urma_create_jetty failed");
        goto fail_jfr;
    }

    urma_reg_seg_flag_t reg_flag = {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.token_id_valid = 0,
    };

    // mgr->va = memalign((0x1 << 12), 0x2000);
    // (void)memset(mgr->va, 1, 0x2000);
    
    urma_seg_cfg_t seg_cfg = {
        .va = (uint64_t)ramdisk_backend_base(mgr->backend),
        .len = ramdisk_backend_size(mgr->backend),
        .token_id = NULL,
        .token_value = mgr->token,
        .flag = reg_flag,
        .user_ctx = (uintptr_t)NULL,
        .iova = 0,
    };
    mgr->local_tseg = urma_register_seg(mgr->urma_ctx, &seg_cfg);
    if (mgr->local_tseg == NULL) {
        RD_LOG_ERR("urma_register_seg failed base=%p len=%llu",
                   ramdisk_backend_base(mgr->backend),
                   (unsigned long long)ramdisk_backend_size(mgr->backend));
        goto fail_jetty;
    }

    mgr->real_initialized = true;
    RD_LOG_INFO("real URMA initialized dev=%s eid_index=%d local_seg_va=%llu local_seg_len=%llu jetty_id=%u uasid=%u trans_mode=%u",
                mgr->urma_dev, eid_index,
                (unsigned long long)mgr->local_tseg->seg.ubva.va,
                (unsigned long long)mgr->local_tseg->seg.len,
                mgr->jetty->jetty_id.id, mgr->urma_ctx->uasid,URMA_TM_RM);

    
    RD_LOG_INFO("backend=%p reg_va=0x%llx tseg_va=0x%llx",
        ramdisk_backend_base(mgr->backend),
        (unsigned long long)seg_cfg.va,
        (unsigned long long)mgr->local_tseg->seg.ubva.va);

    return 0;

fail_jetty:
    urma_delete_jetty(mgr->jetty);
    mgr->jetty = NULL;
fail_jfr:
    urma_delete_jfr(mgr->jfr);
    mgr->jfr = NULL;
fail_jfc:
    urma_delete_jfc(mgr->jfc);
    mgr->jfc = NULL;
fail_jfce:
    urma_delete_jfce(mgr->jfce);
    mgr->jfce = NULL;
fail_ctx:
    urma_delete_context(mgr->urma_ctx);
    mgr->urma_ctx = NULL;
    urma_uninit();
    return -EIO;
}

static void real_destroy_peer(struct ramdisk_urma_peer *peer)
{
    if (peer->t_jetty != NULL) {
        urma_unimport_jetty(peer->t_jetty);
        peer->t_jetty = NULL;
    }
    if (peer->import_tseg != NULL) {
        urma_unimport_seg(peer->import_tseg);
        peer->import_tseg = NULL;
    }
}

static void real_destroy_provider(struct ramdisk_urma_mgr *mgr)
{
    uint32_t i;

    if (!mgr->real_initialized)
        return;
    for (i = 0; i < RAMDISK_URMA_MAX_PEERS; i++)
        real_destroy_peer(&mgr->peers[i]);
    if (mgr->local_tseg != NULL)
        urma_unregister_seg(mgr->local_tseg);
    if (mgr->jetty != NULL)
        urma_delete_jetty(mgr->jetty);
    if (mgr->jfr != NULL)
        urma_delete_jfr(mgr->jfr);
    if (mgr->jfc != NULL)
        urma_delete_jfc(mgr->jfc);
    if (mgr->jfce != NULL)
        urma_delete_jfce(mgr->jfce);
    if (mgr->urma_ctx != NULL)
        urma_delete_context(mgr->urma_ctx);
    urma_uninit();
    mgr->real_initialized = false;
    RD_LOG_INFO("real URMA destroyed");
}

static int real_import_peer(struct ramdisk_urma_mgr *mgr,
                            struct ramdisk_urma_peer *peer)
{
    urma_import_seg_flag_t seg_flag = {
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.mapping = URMA_SEG_NOMAP,
    };

    if (!mgr->real_initialized)
        return -ENODEV;

    memset(&peer->remote_seg, 0, sizeof(peer->remote_seg));
    memcpy(peer->remote_seg.ubva.eid.raw, peer->eid, sizeof(peer->eid));
    peer->remote_seg.ubva.uasid = peer->uasid;
    peer->remote_seg.ubva.va = peer->seg_va;
    peer->remote_seg.len = peer->seg_len;
    peer->remote_seg.attr.bs.cacheable = URMA_NON_CACHEABLE;
    peer->remote_seg.attr.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
    peer->remote_seg.token_id = peer->seg_token_id;

    peer->import_tseg = urma_import_seg(mgr->urma_ctx, &peer->remote_seg,
                                        &mgr->token, 0, seg_flag);
    if (peer->import_tseg == NULL) {
        RD_LOG_ERR("urma_import_seg failed peer=%llu seg_va=%llu len=%llu token_id=%u",
                   (unsigned long long)peer->peer_id,
                   (unsigned long long)peer->seg_va,
                   (unsigned long long)peer->seg_len, peer->seg_token_id);
        return -EIO;
    }

    urma_rjetty_t rjetty;
    memset(&rjetty, 0, sizeof(rjetty));
    memcpy(rjetty.jetty_id.eid.raw, peer->eid, sizeof(peer->eid));
    rjetty.jetty_id.uasid = peer->uasid;
    rjetty.jetty_id.id = peer->jetty_id;
    rjetty.trans_mode = URMA_TM_RM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = real_tp_type(mgr->tp_type);
    rjetty.flag.bs.order_type = mgr->trans_mode == 3 ? 1 : 0;
    rjetty.flag.bs.share_tp = mgr->trans_mode == 3 ? 1 : 0;

    peer->t_jetty = urma_import_jetty(mgr->urma_ctx, &rjetty, &mgr->token);
    if (peer->t_jetty == NULL) {
        RD_LOG_ERR("urma_import_jetty failed peer=%llu jetty=%u",
                   (unsigned long long)peer->peer_id, peer->jetty_id);
        real_destroy_peer(peer);
        return -EIO;
    }
    // if (real_trans_mode(mgr->trans_mode) == URMA_TM_RC &&
    //     urma_bind_jetty(mgr->jetty, peer->t_jetty) != URMA_SUCCESS) {
    //     RD_LOG_ERR("urma_bind_jetty failed peer=%llu jetty=%u",
    //                (unsigned long long)peer->peer_id, peer->jetty_id);
    //     real_destroy_peer(peer);
    //     return -EIO;
    // }
    return 0;
}

static int real_poll_completion(struct ramdisk_urma_mgr *mgr, uint64_t request_id)
{
    uint32_t loops;

    for (loops = 0; loops < 30000U; loops++) {
        urma_cr_t cr = {0};
        int cnt = urma_poll_jfc(mgr->jfc, 1, &cr);

        if (cnt < 0) {
            RD_LOG_ERR("urma_poll_jfc failed request=%llu",
                       (unsigned long long)request_id);
            return -EIO;
        }
        if (cnt == 0) {
            usleep(RAMDISK_URMA_POLL_SLEEP_US);
            continue;
        }
        if (cr.user_ctx != request_id) {
            RD_LOG_WARN("URMA completion request mismatch got=%llu want=%llu status=%d",
                        (unsigned long long)cr.user_ctx,
                        (unsigned long long)request_id, cr.status);
            continue;
        }
        if (cr.status != URMA_CR_SUCCESS) {
            RD_LOG_ERR("URMA completion failed request=%llu status=%d len=%u",
                       (unsigned long long)request_id, cr.status,
                       cr.completion_len);
            return -EIO;
        }
        return 0;
    }
    RD_LOG_ERR("URMA completion timeout request=%llu",
               (unsigned long long)request_id);
    return -ETIMEDOUT;
}
#endif

static void make_abs_timeout(struct timespec *ts, uint32_t timeout_ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += timeout_ms / 1000U;
    ts->tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static struct ramdisk_urma_peer *find_peer_locked(struct ramdisk_urma_mgr *mgr,
                                                  uint64_t peer_id)
{
    uint32_t i;

    for (i = 0; i < RAMDISK_URMA_MAX_PEERS; i++) {
        if (mgr->peers[i].active && mgr->peers[i].peer_id == peer_id)
            return &mgr->peers[i];
    }
    return NULL;
}

static int validate_alignment(struct ramdisk_urma_mgr *mgr,
                              const struct ramdisk_ctrl_urma_transfer *xfer)
{
    uint32_t bs = ramdisk_backend_block_size(mgr->backend);

    if (bs == 0)
        return -EINVAL;
    if ((xfer->local_offset % bs) != 0 ||
        (xfer->remote_hbm_addr % bs) != 0 ||
        (xfer->length % bs) != 0) {
        RD_LOG_ERR("URMA transfer alignment error request=%llu local=%llu remote=%llu len=%u bs=%u",
                   (unsigned long long)xfer->request_id,
                   (unsigned long long)xfer->local_offset,
                   (unsigned long long)xfer->remote_hbm_addr,
                   xfer->length, bs);
        return -EINVAL;
    }
    return 0;
}

static int validate_xfer_locked(struct ramdisk_urma_mgr *mgr,
                                const struct ramdisk_ctrl_urma_transfer *xfer,
                                struct ramdisk_urma_peer **peer_out)
{
    struct ramdisk_urma_peer *peer;
    uint64_t remote_off;

    if (!mgr->enabled)
        return -EOPNOTSUPP;
    if (xfer == NULL || xfer->length == 0)
        return -EINVAL;
    if (xfer->direction != RAMDISK_TO_HBM && xfer->direction != HBM_TO_RAMDISK)
        return -EINVAL;
    if (ramdisk_backend_validate_range(mgr->backend, xfer->local_offset,
                                       xfer->length) != 0)
        return -ERANGE;
    if (validate_alignment(mgr, xfer) != 0)
        return -EINVAL;

    peer = find_peer_locked(mgr, xfer->peer_id);
    if (peer == NULL)
        return -ENODEV;
    if (xfer->remote_hbm_addr < peer->seg_va)
        return -ERANGE;
    remote_off = xfer->remote_hbm_addr - peer->seg_va;
    if (remote_off > peer->seg_len || xfer->length > peer->seg_len - remote_off)
        return -ERANGE;

    *peer_out = peer;
    return 0;
}

static int execute_mock_xfer(struct ramdisk_urma_mgr *mgr,
                             const struct ramdisk_urma_request *req,
                             struct ramdisk_urma_peer *peer)
{
    uint64_t remote_off = req->remote_hbm_addr - peer->seg_va;
    int rc;

    if (peer->mock_hbm == NULL)
        return -ENODEV;

    rc = ramdisk_backend_lock(mgr->backend);
    if (rc != 0)
        return rc;

    if (req->direction == RAMDISK_TO_HBM) {
        memcpy((char *)peer->mock_hbm + remote_off,
               (const char *)ramdisk_backend_base(mgr->backend) +
                   req->local_offset,
               req->length);
    } else {
        memcpy((char *)ramdisk_backend_base(mgr->backend) + req->local_offset,
               (const char *)peer->mock_hbm + remote_off,
               req->length);
    }
    ramdisk_backend_unlock(mgr->backend);
    return 0;
}

static int execute_real_xfer(struct ramdisk_urma_mgr *mgr,
                             const struct ramdisk_urma_request *req,
                             struct ramdisk_urma_peer *peer)
{
#ifdef HAVE_URMA
    uint64_t remote_off = req->remote_hbm_addr - peer->seg_va;
    uint64_t local_addr;
    uint64_t remote_addr;
    urma_sge_t local_sge;
    urma_sge_t remote_sge;
    urma_sg_t local_sg;
    urma_sg_t remote_sg;
    urma_rw_wr_t rw;
    urma_jfs_wr_t wr;
    urma_jfs_wr_t *bad_wr = NULL;

    if (!mgr->real_initialized || peer->import_tseg == NULL ||
        peer->t_jetty == NULL || mgr->local_tseg == NULL)
        return -ENODEV;

    local_addr = (uint64_t)ramdisk_backend_base(mgr->backend) +
                 req->local_offset;
    // local_addr = (uint64_t)mgr->va + req->local_offset;
    // RD_LOG_INFO("$$$$$ local base addr=%llu local offset=%llu",
    //             (uint64_t)mgr->va,
    //             req->local_offset);
    
    remote_addr = peer->remote_seg.ubva.va + remote_off;

    memset(&local_sge, 0, sizeof(local_sge));
    local_sge.addr = local_addr;
    local_sge.len = req->length;
    local_sge.tseg = mgr->local_tseg;

    memset(&remote_sge, 0, sizeof(remote_sge));
    remote_sge.addr = remote_addr;
    remote_sge.len = req->length;
    remote_sge.tseg = peer->import_tseg;

    local_sg.sge = &local_sge;
    local_sg.num_sge = 1;
    remote_sg.sge = &remote_sge;
    remote_sg.num_sge = 1;

    memset(&rw, 0, sizeof(rw));
    if (req->direction == RAMDISK_TO_HBM) {
        rw.src = local_sg;
        rw.dst = remote_sg;
    } else {
        rw.src = remote_sg;
        rw.dst = local_sg;
    }

    memset(&wr, 0, sizeof(wr));
    wr.opcode = req->direction == RAMDISK_TO_HBM ? URMA_OPC_WRITE : URMA_OPC_READ;
    wr.flag.bs.complete_enable = 1;
    wr.flag.bs.inline_flag = 0;
    wr.tjetty = peer->t_jetty;
    wr.user_ctx = req->request_id;
    wr.rw = rw;

    RD_LOG_INFO("posting real URMA request=%llu opcode=%u local_addr=%llx remote_addr=%llx len=%u",
                (unsigned long long)req->request_id, wr.opcode,
                (unsigned long long)local_addr,
                (unsigned long long)remote_addr, req->length);

    if (urma_post_jetty_send_wr(mgr->jetty, &wr, &bad_wr) != URMA_SUCCESS) {
        RD_LOG_ERR("urma_post_jetty_send_wr failed request=%llu bad_wr=%p",
                   (unsigned long long)req->request_id, (void *)bad_wr);
        return -EIO;
    }
    return real_poll_completion(mgr, req->request_id);
#else
    (void)mgr;
    (void)req;
    (void)peer;
    RD_LOG_ERR("real URMA provider is not compiled in; rebuild with HAVE_URMA=1");
    return -ENOSYS;
#endif
}

static void *urma_worker_main(void *arg)
{
    struct ramdisk_urma_mgr *mgr = arg;

    RD_LOG_INFO("URMA worker started queue_depth=%u mock=%d",
                mgr->queue_depth, mgr->use_mock ? 1 : 0);
    for (;;) {
        struct ramdisk_urma_request *req;
        struct ramdisk_urma_peer *peer;
        struct ramdisk_ctrl_urma_transfer xfer;
        int rc;

        pthread_mutex_lock(&mgr->lock);
        while (!mgr->stopping && mgr->pending == 0)
            pthread_cond_wait(&mgr->not_empty, &mgr->lock);
        if (mgr->stopping && mgr->pending == 0) {
            pthread_mutex_unlock(&mgr->lock);
            break;
        }

        req = mgr->queue[mgr->head];
        mgr->queue[mgr->head] = NULL;
        mgr->head = (mgr->head + 1U) % mgr->queue_depth;
        mgr->pending--;
        mgr->inflight++;
        pthread_cond_signal(&mgr->not_full);
        memset(&xfer, 0, sizeof(xfer));
        xfer.peer_id = req->peer_id;
        xfer.request_id = req->request_id;
        xfer.local_offset = req->local_offset;
        xfer.remote_hbm_addr = req->remote_hbm_addr;
        xfer.length = req->length;
        xfer.direction = req->direction;
        rc = validate_xfer_locked(mgr, &xfer, &peer);
        pthread_mutex_unlock(&mgr->lock);

        if (rc == 0) {
            rc = mgr->use_mock ? execute_mock_xfer(mgr, req, peer)
                               : execute_real_xfer(mgr, req, peer);
        }

        pthread_mutex_lock(&mgr->lock);
        mgr->inflight--;
        if (rc == 0)
            mgr->completed++;
        else
            mgr->failed++;
        req->status = rc;
        req->done = true;
        pthread_cond_signal(&req->done_cond);
        pthread_mutex_unlock(&mgr->lock);

        RD_LOG_INFO("URMA request complete request=%llu peer=%llu dir=%u local=%llu remote=%llu len=%u rc=%d",
                    (unsigned long long)req->request_id,
                    (unsigned long long)req->peer_id, req->direction,
                    (unsigned long long)req->local_offset,
                    (unsigned long long)req->remote_hbm_addr,
                    req->length, rc);
    }
    RD_LOG_INFO("URMA worker stopped");
    return NULL;
}

int ramdisk_urma_mgr_init(struct ramdisk_urma_mgr *mgr,
                          struct ramdisk_backend *backend,
                          const struct ramdisk_urma_config *config)
{
    uint32_t qd;
    int rc;

    if (mgr == NULL || backend == NULL || !backend->initialized)
        return -EINVAL;
    memset(mgr, 0, sizeof(*mgr));

    qd = config != NULL && config->queue_depth != 0 ? config->queue_depth : 32U;
    if (qd > RAMDISK_URMA_MAX_QUEUE_DEPTH)
        return -EINVAL;

    mgr->queue = calloc(qd, sizeof(mgr->queue[0]));
    if (mgr->queue == NULL)
        return -ENOMEM;
    rc = pthread_mutex_init(&mgr->lock, NULL);
    if (rc != 0) {
        free(mgr->queue);
        return -rc;
    }
    pthread_cond_init(&mgr->not_empty, NULL);
    pthread_cond_init(&mgr->not_full, NULL);

    mgr->backend = backend;
    mgr->queue_depth = qd;
    mgr->use_mock = config == NULL || config->use_mock;
    mgr->enabled = config != NULL && config->enable;
    mgr->eid_index = config != NULL ? config->eid_index : UINT32_MAX;
    mgr->trans_mode = config != NULL ? config->trans_mode : 1U;
    mgr->tp_type = config != NULL ? config->tp_type : 0U;
    mgr->local_token_value =
        config != NULL && config->local_token_value != 0 ?
        config->local_token_value : RAMDISK_URMA_DEFAULT_TOKEN;
    if (config != NULL && config->urma_dev != NULL) {
        snprintf(mgr->urma_dev, sizeof(mgr->urma_dev), "%s",
                 config->urma_dev);
    }
    mgr->initialized = true;

    if (!mgr->use_mock) {
#ifdef HAVE_URMA
        rc = real_init_provider(mgr);
        if (rc != 0) {
            ramdisk_urma_mgr_destroy(mgr);
            return rc;
        }
#else
        RD_LOG_ERR("real URMA requested but binary was not built with HAVE_URMA=1");
        ramdisk_urma_mgr_destroy(mgr);
        return -ENOSYS;
#endif
    }

    rc = pthread_create(&mgr->worker, NULL, urma_worker_main, mgr);
    if (rc != 0) {
        ramdisk_urma_mgr_destroy(mgr);
        return -rc;
    }
    mgr->worker_started = true;

    RD_LOG_INFO("URMA manager initialized enabled=%d mock=%d queue_depth=%u",
                mgr->enabled ? 1 : 0, mgr->use_mock ? 1 : 0, qd);
    return 0;
}

void ramdisk_urma_mgr_destroy(struct ramdisk_urma_mgr *mgr)
{
    uint32_t i;

    if (mgr == NULL || !mgr->initialized)
        return;

    if (mgr->worker_started) {
        pthread_mutex_lock(&mgr->lock);
        mgr->stopping = true;
        pthread_cond_broadcast(&mgr->not_empty);
        pthread_cond_broadcast(&mgr->not_full);
        pthread_mutex_unlock(&mgr->lock);
        pthread_join(mgr->worker, NULL);
        mgr->worker_started = false;
    }

    for (i = 0; i < RAMDISK_URMA_MAX_PEERS; i++) {
        if (mgr->peers[i].active) {
#ifdef HAVE_URMA
            real_destroy_peer(&mgr->peers[i]);
#endif
            free(mgr->peers[i].mock_hbm);
            mgr->peers[i].mock_hbm = NULL;
            mgr->peers[i].active = false;
        }
    }
#ifdef HAVE_URMA
    real_destroy_provider(mgr);
#endif
    free(mgr->queue);
    mgr->queue = NULL;
    pthread_cond_destroy(&mgr->not_empty);
    pthread_cond_destroy(&mgr->not_full);
    pthread_mutex_destroy(&mgr->lock);
    mgr->initialized = false;
}

int ramdisk_urma_enable(struct ramdisk_urma_mgr *mgr)
{
    if (mgr == NULL || !mgr->initialized)
        return -EINVAL;
    pthread_mutex_lock(&mgr->lock);
    mgr->enabled = true;
    pthread_mutex_unlock(&mgr->lock);
    RD_LOG_INFO("URMA path enabled");
    return 0;
}

int ramdisk_urma_disable(struct ramdisk_urma_mgr *mgr)
{
    if (mgr == NULL || !mgr->initialized)
        return -EINVAL;
    pthread_mutex_lock(&mgr->lock);
    mgr->enabled = false;
    pthread_mutex_unlock(&mgr->lock);
    RD_LOG_INFO("URMA path disabled");
    return 0;
}

int ramdisk_urma_peer_connect(struct ramdisk_urma_mgr *mgr,
                              const struct ramdisk_ctrl_peer_connect_info *info)
{
    struct ramdisk_urma_peer *slot = NULL;
    struct ramdisk_urma_peer new_peer;
    void *mock_hbm = NULL;
    uint32_t i;

    if (mgr == NULL || !mgr->initialized || info == NULL)
        return -EINVAL;
    if (info->peer_id == 0 || info->seg_len == 0)
        return -EINVAL;
    memset(&new_peer, 0, sizeof(new_peer));
    new_peer.active = true;
    new_peer.peer_id = info->peer_id;
    memcpy(new_peer.eid, info->eid, sizeof(new_peer.eid));
    new_peer.uasid = info->uasid;
    new_peer.seg_va = info->seg_va;
    new_peer.seg_len = info->seg_len;
    new_peer.seg_token_id = info->seg_token_id;
    new_peer.jetty_id = info->jetty_id;

    if (mgr->use_mock) {
        mock_hbm = calloc(1, (size_t)info->seg_len);
        if (mock_hbm == NULL)
            return -ENOMEM;
        new_peer.mock_hbm = mock_hbm;
    } else {
#ifdef HAVE_URMA
        int rc = real_import_peer(mgr, &new_peer);
        if (rc != 0)
            return rc;
#else
        return -ENOSYS;
#endif
    }

    pthread_mutex_lock(&mgr->lock);
    if (!mgr->enabled) {
        pthread_mutex_unlock(&mgr->lock);
        free(new_peer.mock_hbm);
#ifdef HAVE_URMA
        real_destroy_peer(&new_peer);
#endif
        return -EOPNOTSUPP;
    }
    if (find_peer_locked(mgr, info->peer_id) != NULL) {
        pthread_mutex_unlock(&mgr->lock);
        free(new_peer.mock_hbm);
#ifdef HAVE_URMA
        real_destroy_peer(&new_peer);
#endif
        return -EEXIST;
    }
    for (i = 0; i < RAMDISK_URMA_MAX_PEERS; i++) {
        if (!mgr->peers[i].active) {
            slot = &mgr->peers[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&mgr->lock);
        free(new_peer.mock_hbm);
#ifdef HAVE_URMA
        real_destroy_peer(&new_peer);
#endif
        return -ENOSPC;
    }

    *slot = new_peer;
    pthread_mutex_unlock(&mgr->lock);

    RD_LOG_INFO("URMA peer connected peer=%llu seg_va=%llu seg_len=%llu token=%u jetty=%u mock=%d",
                (unsigned long long)info->peer_id,
                (unsigned long long)info->seg_va,
                (unsigned long long)info->seg_len,
                info->seg_token_id, info->jetty_id, mgr->use_mock ? 1 : 0);
    return 0;
}

int ramdisk_urma_peer_disconnect(struct ramdisk_urma_mgr *mgr, uint64_t peer_id)
{
    struct ramdisk_urma_peer old;
    struct ramdisk_urma_peer *peer;

    if (mgr == NULL || !mgr->initialized || peer_id == 0)
        return -EINVAL;

    memset(&old, 0, sizeof(old));
    pthread_mutex_lock(&mgr->lock);
    peer = find_peer_locked(mgr, peer_id);
    if (peer == NULL) {
        pthread_mutex_unlock(&mgr->lock);
        return -ENOENT;
    }
    old = *peer;
    memset(peer, 0, sizeof(*peer));
    pthread_mutex_unlock(&mgr->lock);

#ifdef HAVE_URMA
    real_destroy_peer(&old);
#endif
    free(old.mock_hbm);
    RD_LOG_INFO("URMA peer disconnected peer=%llu",
                (unsigned long long)peer_id);
    return 0;
}

int ramdisk_urma_transfer_sync(struct ramdisk_urma_mgr *mgr,
                               const struct ramdisk_ctrl_urma_transfer *xfer,
                               uint32_t timeout_ms)
{
    struct ramdisk_urma_request req;
    struct ramdisk_urma_peer *peer;
    struct timespec ts;
    int rc;

    if (mgr == NULL || !mgr->initialized || xfer == NULL)
        return -EINVAL;
    memset(&req, 0, sizeof(req));
    req.peer_id = xfer->peer_id;
    req.request_id = xfer->request_id;
    req.local_offset = xfer->local_offset;
    req.remote_hbm_addr = xfer->remote_hbm_addr;
    req.length = xfer->length;
    req.direction = xfer->direction;
    pthread_cond_init(&req.done_cond, NULL);

    pthread_mutex_lock(&mgr->lock);
    rc = validate_xfer_locked(mgr, xfer, &peer);
    (void)peer;
    if (rc != 0)
        goto out_unlock;
    if (mgr->pending >= mgr->queue_depth) {
        rc = -EBUSY;
        goto out_unlock;
    }

    mgr->queue[mgr->tail] = &req;
    mgr->tail = (mgr->tail + 1U) % mgr->queue_depth;
    mgr->pending++;
    pthread_cond_signal(&mgr->not_empty);

    make_abs_timeout(&ts, timeout_ms == 0 ? RAMDISK_URMA_DEFAULT_TIMEOUT_MS
                                          : timeout_ms);
    while (!req.done) {
        rc = pthread_cond_timedwait(&req.done_cond, &mgr->lock, &ts);
        if (rc == ETIMEDOUT) {
            RD_LOG_ERR("URMA transfer timeout request=%llu peer=%llu",
                       (unsigned long long)req.request_id,
                       (unsigned long long)req.peer_id);
            make_abs_timeout(&ts, timeout_ms == 0 ? RAMDISK_URMA_DEFAULT_TIMEOUT_MS
                                                  : timeout_ms);
            continue;
        }
        if (rc != 0) {
            rc = -rc;
            goto out_unlock;
        }
    }
    rc = req.status;

out_unlock:
    pthread_mutex_unlock(&mgr->lock);
    pthread_cond_destroy(&req.done_cond);
    return rc;
}

void ramdisk_urma_fill_status(struct ramdisk_urma_mgr *mgr,
                              struct ramdisk_ctrl_status *status)
{
    uint32_t i;

    if (status == NULL)
        return;
    if (mgr == NULL || !mgr->initialized)
        return;

    pthread_mutex_lock(&mgr->lock);
    status->urma_enabled = mgr->enabled ? 1U : 0U;
    status->queue_depth = mgr->queue_depth;
    status->inflight = mgr->inflight;
    status->pending = mgr->pending;
    status->completed = mgr->completed;
    status->failed = mgr->failed;
    for (i = 0; i < RAMDISK_URMA_MAX_PEERS; i++) {
        if (mgr->peers[i].active)
            status->peer_count++;
    }
    pthread_mutex_unlock(&mgr->lock);
}

int ramdisk_urma_mock_write_peer(struct ramdisk_urma_mgr *mgr,
                                 uint64_t peer_id, uint64_t remote_addr,
                                 const void *buf, uint32_t len)
{
    struct ramdisk_urma_peer *peer;
    uint64_t off;

    if (mgr == NULL || buf == NULL)
        return -EINVAL;
    pthread_mutex_lock(&mgr->lock);
    peer = find_peer_locked(mgr, peer_id);
    if (peer == NULL || peer->mock_hbm == NULL) {
        pthread_mutex_unlock(&mgr->lock);
        return -ENODEV;
    }
    if (remote_addr < peer->seg_va || len > peer->seg_len ||
        remote_addr - peer->seg_va > peer->seg_len - len) {
        pthread_mutex_unlock(&mgr->lock);
        return -ERANGE;
    }
    off = remote_addr - peer->seg_va;
    memcpy((char *)peer->mock_hbm + off, buf, len);
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

int ramdisk_urma_mock_read_peer(struct ramdisk_urma_mgr *mgr,
                                uint64_t peer_id, uint64_t remote_addr,
                                void *buf, uint32_t len)
{
    struct ramdisk_urma_peer *peer;
    uint64_t off;

    if (mgr == NULL || buf == NULL)
        return -EINVAL;
    pthread_mutex_lock(&mgr->lock);
    peer = find_peer_locked(mgr, peer_id);
    if (peer == NULL || peer->mock_hbm == NULL) {
        pthread_mutex_unlock(&mgr->lock);
        return -ENODEV;
    }
    if (remote_addr < peer->seg_va || len > peer->seg_len ||
        remote_addr - peer->seg_va > peer->seg_len - len) {
        pthread_mutex_unlock(&mgr->lock);
        return -ERANGE;
    }
    off = remote_addr - peer->seg_va;
    memcpy(buf, (const char *)peer->mock_hbm + off, len);
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}
