#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ramdisk_ctrl_client.h"

#ifdef HAVE_URMA
#include "urma_api.h"
#endif

#define DEFAULT_CONTROL_SOCK "/run/nbd-ramdisk/control.sock"
#define DEFAULT_URMA_DEV "ub0"
#define BUSINESS_MEM_SIZE (1024ULL * 1024ULL)
#define DEFAULT_TRANSFER_LEN 4096U
#define DEFAULT_QUEUE_DEPTH 64U
#define BUSINESS_TOKEN_VALUE 0xB15CU

#ifdef HAVE_URMA
struct business_urma_ctx {
    void *buf;
    urma_context_t *ctx;
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jfr_t *jfr;
    urma_jetty_t *jetty;
    urma_target_seg_t *local_tseg;
    urma_token_t token;
    uint32_t eid_index;
};

static void format_eid(const uint8_t eid[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t i;

    for (i = 0; i < 16; i++) {
        out[i * 2] = hex[eid[i] >> 4];
        out[i * 2 + 1] = hex[eid[i] & 0x0fU];
    }
    out[32] = '\0';
}

static int print_status(const char *sock, const char *tag)
{
    struct ramdisk_ctrl_status st;
    int rc = ramdisk_ctrl_query_status(sock, &st);

    if (rc != 0) {
        fprintf(stderr, "%s: query status failed rc=%d\n", tag, rc);
        return rc;
    }

    printf("%s: size=%" PRIu64 " block=%u urma=%u peers=%u pending=%u inflight=%u completed=%" PRIu64 " failed=%" PRIu64 "\n",
           tag, st.size, st.block_size, st.urma_enabled, st.peer_count,
           st.pending, st.inflight, st.completed, st.failed);
    return 0;
}

static urma_transport_mode_t business_trans_mode(void)
{
    return URMA_TM_RC;
}

static uint64_t request_id(uint32_t seq)
{
    return ((uint64_t)(uint32_t)getpid() << 32) | seq;
}

static int select_business_eid_index(urma_device_t *dev, uint32_t *eid_index)
{
    urma_eid_info_t *eid_list;
    uint32_t eid_cnt = 0;

    eid_list = urma_get_eid_list(dev, &eid_cnt);
    if (eid_list == NULL || eid_cnt == 0)
        return -ENODEV;

    /*
     * The ramdisk daemon defaults to the first available EID. Pick the second
     * one for this business-side endpoint so its EID is not the ramdisk EID.
     */
    if (eid_cnt < 2U) {
        urma_free_eid_list(eid_list);
        return -EADDRINUSE;
    }

    *eid_index = eid_list[1].eid_index;
    urma_free_eid_list(eid_list);
    return 0;
}

static int create_business_urma(struct business_urma_ctx *ctx,
                                const char *dev_name)
{
    urma_init_attr_t init_attr = {0};
    urma_device_attr_t dev_attr;
    urma_device_t *dev;
    uint32_t depth;
    int rc;

    memset(ctx, 0, sizeof(*ctx));
    init_attr.uasid = 0;
    if (urma_init(&init_attr) != URMA_SUCCESS) {
        fprintf(stderr, "urma_init failed\n");
        return -EIO;
    }

    dev = urma_get_device_by_name(dev_name);
    if (dev == NULL) {
        fprintf(stderr, "urma_get_device_by_name failed dev=%s\n", dev_name);
        rc = -ENODEV;
        goto fail_uninit;
    }

    if (urma_query_device(dev, &dev_attr) != URMA_SUCCESS) {
        fprintf(stderr, "urma_query_device failed dev=%s\n", dev_name);
        rc = -EIO;
        goto fail_uninit;
    }

    rc = select_business_eid_index(dev, &ctx->eid_index);
    if (rc != 0) {
        fprintf(stderr,
                "no non-ramdisk EID available on dev=%s rc=%d; start ramdisk and business endpoint with different EIDs\n",
                dev_name, rc);
        goto fail_uninit;
    }

    ctx->ctx = urma_create_context(dev, ctx->eid_index);
    if (ctx->ctx == NULL) {
        fprintf(stderr, "urma_create_context failed dev=%s eid_index=%u\n",
                dev_name, ctx->eid_index);
        rc = -EIO;
        goto fail_uninit;
    }
    ctx->token.token = BUSINESS_TOKEN_VALUE;

    ctx->jfce = urma_create_jfce(ctx->ctx);
    if (ctx->jfce == NULL) {
        fprintf(stderr, "urma_create_jfce failed\n");
        rc = -EIO;
        goto fail_ctx;
    }

    depth = DEFAULT_QUEUE_DEPTH;
    if (dev_attr.dev_cap.max_jfc_depth != 0 &&
        depth > dev_attr.dev_cap.max_jfc_depth)
        depth = dev_attr.dev_cap.max_jfc_depth;

    urma_jfc_cfg_t jfc_cfg = {
        .depth = depth,
        .flag = {.value = 0},
        .jfce = ctx->jfce,
        .user_ctx = 0,
    };
    ctx->jfc = urma_create_jfc(ctx->ctx, &jfc_cfg);
    if (ctx->jfc == NULL) {
        fprintf(stderr, "urma_create_jfc failed depth=%u\n", depth);
        rc = -EIO;
        goto fail_jfce;
    }

    urma_jfr_cfg_t jfr_cfg = {
        .depth = depth,
        .flag.bs.tag_matching = URMA_NO_TAG_MATCHING,
        .trans_mode = business_trans_mode(),
        .min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER,
        .jfc = ctx->jfc,
        .token_value = ctx->token,
        .id = 0,
        .max_sge = 1,
    };
    ctx->jfr = urma_create_jfr(ctx->ctx, &jfr_cfg);
    if (ctx->jfr == NULL) {
        fprintf(stderr, "urma_create_jfr failed depth=%u\n", depth);
        rc = -EIO;
        goto fail_jfc;
    }

    urma_jfs_cfg_t jfs_cfg = {
        .depth = depth,
        .trans_mode = business_trans_mode(),
        .priority = URMA_MAX_PRIORITY,
        .max_sge = 1,
        .max_inline_data = 0,
        .rnr_retry = URMA_TYPICAL_RNR_RETRY,
        .err_timeout = URMA_TYPICAL_ERR_TIMEOUT,
        .jfc = ctx->jfc,
        .user_ctx = 0,
    };
    urma_jetty_cfg_t jetty_cfg = {
        .flag.bs.share_jfr = 1,
        .jfs_cfg = &jfs_cfg,
        .shared.jfr = ctx->jfr,
    };
    ctx->jetty = urma_create_jetty(ctx->ctx, &jetty_cfg);
    if (ctx->jetty == NULL) {
        fprintf(stderr, "urma_create_jetty failed\n");
        rc = -EIO;
        goto fail_jfr;
    }

    rc = posix_memalign(&ctx->buf, (size_t)sysconf(_SC_PAGESIZE),
                        BUSINESS_MEM_SIZE);
    if (rc != 0) {
        ctx->buf = NULL;
        rc = -rc;
        goto fail_jetty;
    }
    memset(ctx->buf, 0, BUSINESS_MEM_SIZE);

    urma_reg_seg_flag_t reg_flag = {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_REMOTE_READ | URMA_ACCESS_REMOTE_WRITE | URMA_ACCESS_REMOTE_ATOMIC,
        .bs.token_id_valid = 0,
    };
    urma_seg_cfg_t seg_cfg = {
        .va = (uint64_t)ctx->buf,
        .len = BUSINESS_MEM_SIZE,
        .token_id = NULL,
        .token_value = &(ctx->token),
        .flag = reg_flag,
        .user_ctx = 0,
        .iova = 0,
    };
    ctx->local_tseg = urma_register_seg(ctx->ctx, &seg_cfg);
    if (ctx->local_tseg == NULL) {
        fprintf(stderr, "urma_register_seg failed buf=%p len=%llu\n",
                ctx->buf, (unsigned long long)BUSINESS_MEM_SIZE);
        rc = -EIO;
        goto fail_buf;
    }

    return 0;

fail_buf:
    free(ctx->buf);
    ctx->buf = NULL;
fail_jetty:
    urma_delete_jetty(ctx->jetty);
    ctx->jetty = NULL;
fail_jfr:
    urma_delete_jfr(ctx->jfr);
    ctx->jfr = NULL;
fail_jfc:
    urma_delete_jfc(ctx->jfc);
    ctx->jfc = NULL;
fail_jfce:
    urma_delete_jfce(ctx->jfce);
    ctx->jfce = NULL;
fail_ctx:
    urma_delete_context(ctx->ctx);
    ctx->ctx = NULL;
fail_uninit:
    urma_uninit();
    return rc;
}

static void destroy_business_urma(struct business_urma_ctx *ctx)
{
    if (ctx->local_tseg != NULL)
        urma_unregister_seg(ctx->local_tseg);
    if (ctx->jetty != NULL)
        urma_delete_jetty(ctx->jetty);
    if (ctx->jfr != NULL)
        urma_delete_jfr(ctx->jfr);
    if (ctx->jfc != NULL)
        urma_delete_jfc(ctx->jfc);
    if (ctx->jfce != NULL)
        urma_delete_jfce(ctx->jfce);
    free(ctx->buf);
    if (ctx->ctx != NULL)
        urma_delete_context(ctx->ctx);
    urma_uninit();
}

static void fill_peer_from_urma(struct ramdisk_ctrl_peer_connect *peer,
                                const struct business_urma_ctx *ctx)
{
    memset(peer, 0, sizeof(*peer));
    peer->peer_id = ((uint64_t)ctx->eid_index << 32) |
                    (uint64_t)ctx->jetty->jetty_id.id;
    if (peer->peer_id == 0)
        peer->peer_id = 1;
    memcpy(peer->eid, ctx->local_tseg->seg.ubva.eid.raw, sizeof(peer->eid));
    peer->uasid = ctx->ctx->uasid;
    peer->seg_va = ctx->local_tseg->seg.ubva.va;
    peer->seg_len = ctx->local_tseg->seg.len;
    peer->seg_token_id = ctx->local_tseg->seg.token_id;
    peer->jetty_id = ctx->jetty->jetty_id.id;
}
#endif

int main(int argc, char **argv)
{
#ifdef HAVE_URMA
    const char *sock = DEFAULT_CONTROL_SOCK;
    const char *dev_name = getenv("RAMDISK_URMA_DEV");
    struct business_urma_ctx urma;
    struct ramdisk_ctrl_peer_connect peer;
    struct ramdisk_ctrl_urma_transfer xfer;
    uint64_t remote_addr;
    char eid_text[33];
    int rc;

    if (argc != 1) {
        fprintf(stderr, "Usage: %s\n", argv[0]);
        return 2;
    }
    if (dev_name == NULL || dev_name[0] == '\0')
        dev_name = DEFAULT_URMA_DEV;

    rc = create_business_urma(&urma, dev_name);
    if (rc != 0)
        return 1;

    fill_peer_from_urma(&peer, &urma);
    format_eid(peer.eid, eid_text);
    printf("created real URMA peer: dev=%s eid_index=%u peer_id=%" PRIu64 " eid=%s uasid=%u seg_va=0x%" PRIx64 " seg_len=0x%" PRIx64 " token=%u jetty=%u\n",
           dev_name, urma.eid_index, peer.peer_id, eid_text, peer.uasid,
           peer.seg_va, peer.seg_len, peer.seg_token_id, peer.jetty_id);

    rc = print_status(sock, "before");
    if (rc != 0)
        goto out_destroy;

    rc = ramdisk_ctrl_enable_urma(sock);
    if (rc != 0) {
        fprintf(stderr, "enable URMA path failed rc=%d\n", rc);
        goto out_destroy;
    }

    rc = ramdisk_ctrl_peer_connect(sock, &peer);
    if (rc != 0 && rc != -EEXIST) {
        fprintf(stderr, "peer connect failed rc=%d\n", rc);
        goto out_destroy;
    }
    if (rc == -EEXIST)
        printf("peer %" PRIu64 " already exists, reuse it\n", peer.peer_id);

    remote_addr = peer.seg_va;

    memset(&xfer, 0, sizeof(xfer));
    xfer.peer_id = peer.peer_id;
    xfer.request_id = request_id(1001U);
    xfer.local_offset = 0;
    xfer.remote_hbm_addr = remote_addr;
    xfer.length = DEFAULT_TRANSFER_LEN;
    xfer.direction = RAMDISK_TO_HBM;
    rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
    if (rc != 0) {
        fprintf(stderr, "RAMDISK_TO_HBM transfer failed rc=%d\n", rc);
        goto out_destroy;
    }
    printf("RAMDISK_TO_HBM transfer done request=%" PRIu64 "\n",
           xfer.request_id);

    xfer.request_id = request_id(1002U);
    xfer.direction = HBM_TO_RAMDISK;
    rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
    if (rc != 0) {
        fprintf(stderr, "HBM_TO_RAMDISK transfer failed rc=%d\n", rc);
        goto out_destroy;
    }
    printf("HBM_TO_RAMDISK transfer done request=%" PRIu64 "\n",
           xfer.request_id);

    rc = print_status(sock, "after");

out_destroy:
    destroy_business_urma(&urma);
    return rc == 0 ? 0 : 1;
#else
    (void)argc;
    (void)argv;
    fprintf(stderr,
            "business_client_example requires real URMA resources; rebuild with HAVE_URMA=1\n");
    return 1;
#endif
}
