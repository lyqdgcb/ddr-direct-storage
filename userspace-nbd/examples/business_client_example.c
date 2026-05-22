#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ramdisk_ctrl_client.h"

static uint64_t parse_u64(const char *s)
{
    return strtoull(s, NULL, 0);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [sock peer_id eid uasid seg_va seg_len token jetty local_off remote_addr len]\n"
            "\n"
            "Example:\n"
            "  %s /tmp/nbd-ramdisk-control.sock 7 00000000000000000000ffff0a000001 0 0x100000 0x100000 11 22 0 0x100000 4096\n",
            prog, prog);
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

int main(int argc, char **argv)
{
    const char *sock = "/run/nbd-ramdisk/control.sock";
    const char *eid_text = "00000000000000000000ffff0a000001";
    struct ramdisk_ctrl_peer_connect peer;
    struct ramdisk_ctrl_urma_transfer xfer;
    uint64_t peer_id = 7;
    uint64_t local_off = 0;
    uint64_t remote_addr = 0x100000;
    uint32_t length = 4096;
    int rc;

    memset(&peer, 0, sizeof(peer));
    peer.uasid = 0;
    peer.seg_va = 0x100000;
    peer.seg_len = 0x100000;
    peer.seg_token_id = 11;
    peer.jetty_id = 22;

    if (argc != 1 && argc != 12) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 12) {
        sock = argv[1];
        peer_id = parse_u64(argv[2]);
        eid_text = argv[3];
        peer.uasid = (uint32_t)parse_u64(argv[4]);
        peer.seg_va = parse_u64(argv[5]);
        peer.seg_len = parse_u64(argv[6]);
        peer.seg_token_id = (uint32_t)parse_u64(argv[7]);
        peer.jetty_id = (uint32_t)parse_u64(argv[8]);
        local_off = parse_u64(argv[9]);
        remote_addr = parse_u64(argv[10]);
        length = (uint32_t)parse_u64(argv[11]);
    }

    peer.peer_id = peer_id;
    rc = ramdisk_ctrl_parse_eid(eid_text, peer.eid);
    if (rc != 0) {
        fprintf(stderr, "invalid eid: %s\n", eid_text);
        return 2;
    }

    rc = print_status(sock, "before");
    if (rc != 0)
        return 1;

    rc = ramdisk_ctrl_enable_urma(sock);
    if (rc != 0) {
        fprintf(stderr, "enable URMA path failed rc=%d\n", rc);
        return 1;
    }

    rc = ramdisk_ctrl_peer_connect(sock, &peer);
    if (rc != 0 && rc != -EEXIST) {
        fprintf(stderr, "peer connect failed rc=%d\n", rc);
        return 1;
    }
    if (rc == -EEXIST)
        printf("peer %" PRIu64 " already exists, reuse it\n", peer_id);

    memset(&xfer, 0, sizeof(xfer));
    xfer.peer_id = peer_id;
    xfer.request_id = 1001;
    xfer.local_offset = local_off;
    xfer.remote_hbm_addr = remote_addr;
    xfer.length = length;
    xfer.direction = RAMDISK_TO_HBM;
    rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
    if (rc != 0) {
        fprintf(stderr, "RAMDISK_TO_HBM transfer failed rc=%d\n", rc);
        return 1;
    }
    printf("RAMDISK_TO_HBM transfer done request=%" PRIu64 "\n",
           xfer.request_id);

    xfer.request_id = 1002;
    xfer.direction = HBM_TO_RAMDISK;
    rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
    if (rc != 0) {
        fprintf(stderr, "HBM_TO_RAMDISK transfer failed rc=%d\n", rc);
        return 1;
    }
    printf("HBM_TO_RAMDISK transfer done request=%" PRIu64 "\n",
           xfer.request_id);

    rc = print_status(sock, "after");
    return rc == 0 ? 0 : 1;
}
