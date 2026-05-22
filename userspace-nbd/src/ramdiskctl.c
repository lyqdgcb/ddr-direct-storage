#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ramdisk_ctrl_client.h"

static uint64_t parse_u64(const char *s)
{
    return strtoull(s, NULL, 0);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --sock PATH COMMAND [args]\n"
            "Commands:\n"
            "  status\n"
            "  enable\n"
            "  disable\n"
            "  peer-connect PEER_ID EID_HEX UASID SEG_VA SEG_LEN TOKEN JETTY\n"
            "  peer-disconnect PEER_ID\n"
            "  transfer PEER_ID REQUEST_ID LOCAL_OFF REMOTE_ADDR LEN to-hbm|from-hbm\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *sock = "/run/nbd-ramdisk/control.sock";
    int arg = 1;
    int rc;

    if (argc > 3 && strcmp(argv[1], "--sock") == 0) {
        sock = argv[2];
        arg = 3;
    }
    if (arg >= argc) {
        usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[arg], "status") == 0) {
        struct ramdisk_ctrl_status st;
        memset(&st, 0, sizeof(st));
        rc = ramdisk_ctrl_query_status(sock, &st);
        if (rc == 0) {
            printf("size=%" PRIu64 " block_size=%u urma=%u peers=%u queue_depth=%u inflight=%u pending=%u completed=%" PRIu64 " failed=%" PRIu64 "\n",
                   st.size, st.block_size, st.urma_enabled, st.peer_count,
                   st.queue_depth, st.inflight, st.pending, st.completed,
                   st.failed);
        }
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[arg], "enable") == 0) {
        rc = ramdisk_ctrl_enable_urma(sock);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[arg], "disable") == 0) {
        rc = ramdisk_ctrl_disable_urma(sock);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[arg], "peer-connect") == 0 && argc - arg == 8) {
        struct ramdisk_ctrl_peer_connect peer;
        memset(&peer, 0, sizeof(peer));
        peer.peer_id = parse_u64(argv[arg + 1]);
        if (ramdisk_ctrl_parse_eid(argv[arg + 2], peer.eid) != 0) {
            fprintf(stderr, "invalid EID, expected 16-byte hex string\n");
            return 2;
        }
        peer.uasid = (uint32_t)parse_u64(argv[arg + 3]);
        peer.seg_va = parse_u64(argv[arg + 4]);
        peer.seg_len = parse_u64(argv[arg + 5]);
        peer.seg_token_id = (uint32_t)parse_u64(argv[arg + 6]);
        peer.jetty_id = (uint32_t)parse_u64(argv[arg + 7]);
        rc = ramdisk_ctrl_peer_connect(sock, &peer);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[arg], "peer-disconnect") == 0 && argc - arg == 2) {
        rc = ramdisk_ctrl_peer_disconnect(sock, parse_u64(argv[arg + 1]));
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(argv[arg], "transfer") == 0 && argc - arg == 7) {
        struct ramdisk_ctrl_urma_transfer xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.peer_id = parse_u64(argv[arg + 1]);
        xfer.request_id = parse_u64(argv[arg + 2]);
        xfer.local_offset = parse_u64(argv[arg + 3]);
        xfer.remote_hbm_addr = parse_u64(argv[arg + 4]);
        xfer.length = (uint32_t)parse_u64(argv[arg + 5]);
        if (strcmp(argv[arg + 6], "to-hbm") == 0)
            xfer.direction = RAMDISK_TO_HBM;
        else if (strcmp(argv[arg + 6], "from-hbm") == 0)
            xfer.direction = HBM_TO_RAMDISK;
        else
            return 2;
        rc = ramdisk_ctrl_urma_transfer(sock, &xfer);
        return rc == 0 ? 0 : 1;
    }

    usage(argv[0]);
    return 2;
}
