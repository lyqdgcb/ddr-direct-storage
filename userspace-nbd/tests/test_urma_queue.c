#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ramdisk_backend.h"
#include "ramdisk_urma.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

struct thread_arg {
    struct ramdisk_urma_mgr *mgr;
    uint64_t idx;
    int rc;
};

static void *xfer_thread(void *arg)
{
    struct thread_arg *a = arg;
    struct ramdisk_ctrl_urma_transfer xfer;

    memset(&xfer, 0, sizeof(xfer));
    xfer.peer_id = 7;
    xfer.request_id = a->idx;
    xfer.local_offset = a->idx * 4096;
    xfer.remote_hbm_addr = 0x100000 + a->idx * 4096;
    xfer.length = 4096;
    xfer.direction = RAMDISK_TO_HBM;
    a->rc = ramdisk_urma_transfer_sync(a->mgr, &xfer, 5000);
    return NULL;
}

int main(void)
{
    struct ramdisk_backend b;
    struct ramdisk_urma_mgr mgr;
    struct ramdisk_urma_config cfg = {
        .enable = true,
        .use_mock = true,
        .queue_depth = 8,
    };
    struct ramdisk_ctrl_peer_connect peer;
    uint8_t pattern[4096];
    uint8_t out[4096];
    uint8_t hbm[4096];
    pthread_t threads[4];
    struct thread_arg args[4];
    uint32_t i;

    memset(pattern, 0x5a, sizeof(pattern));
    memset(hbm, 0xc3, sizeof(hbm));

    CHECK(ramdisk_backend_create(&b, 65536, 4096) == 0);
    CHECK(ramdisk_backend_write(&b, 0, pattern, sizeof(pattern)) == 0);
    CHECK(ramdisk_urma_mgr_init(&mgr, &b, &cfg) == 0);

    memset(&peer, 0, sizeof(peer));
    peer.peer_id = 7;
    peer.seg_va = 0x100000;
    peer.seg_len = 65536;
    peer.seg_token_id = 11;
    peer.jetty_id = 22;
    CHECK(ramdisk_urma_peer_connect(&mgr, &peer) == 0);

    {
        struct ramdisk_ctrl_urma_transfer xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.peer_id = 7;
        xfer.request_id = 1;
        xfer.local_offset = 0;
        xfer.remote_hbm_addr = 0x100000;
        xfer.length = 4096;
        xfer.direction = RAMDISK_TO_HBM;
        CHECK(ramdisk_urma_transfer_sync(&mgr, &xfer, 5000) == 0);
        CHECK(ramdisk_urma_mock_read_peer(&mgr, 7, 0x100000, out, sizeof(out)) == 0);
        CHECK(memcmp(pattern, out, sizeof(out)) == 0);

        CHECK(ramdisk_urma_mock_write_peer(&mgr, 7, 0x101000, hbm, sizeof(hbm)) == 0);
        xfer.request_id = 2;
        xfer.local_offset = 4096;
        xfer.remote_hbm_addr = 0x101000;
        xfer.direction = HBM_TO_RAMDISK;
        CHECK(ramdisk_urma_transfer_sync(&mgr, &xfer, 5000) == 0);
        CHECK(ramdisk_backend_read(&b, 4096, out, sizeof(out)) == 0);
        CHECK(memcmp(hbm, out, sizeof(out)) == 0);

        xfer.local_offset = 1;
        CHECK(ramdisk_urma_transfer_sync(&mgr, &xfer, 5000) == -EINVAL);
        xfer.local_offset = 4096;
        xfer.remote_hbm_addr = 0x200000;
        CHECK(ramdisk_urma_transfer_sync(&mgr, &xfer, 5000) == -ERANGE);
    }

    for (i = 0; i < 4; i++) {
        memset(pattern, (int)(0x30 + i), sizeof(pattern));
        CHECK(ramdisk_backend_write(&b, i * 4096, pattern, sizeof(pattern)) == 0);
        args[i].mgr = &mgr;
        args[i].idx = i;
        args[i].rc = -1;
        CHECK(pthread_create(&threads[i], NULL, xfer_thread, &args[i]) == 0);
    }
    for (i = 0; i < 4; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(args[i].rc == 0);
    }

    ramdisk_urma_mgr_destroy(&mgr);
    ramdisk_backend_destroy(&b);
    puts("test_urma_queue passed");
    return 0;
}
