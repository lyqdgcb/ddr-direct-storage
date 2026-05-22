#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ramdisk_backend.h"
#include "ramdisk_ctrl.h"
#include "ramdisk_ctrl_server.h"
#include "ramdisk_urma.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static ssize_t read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static int connect_sock(const char *path)
{
    struct sockaddr_un addr;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int transact(const char *path, uint16_t opcode, const void *payload,
                    uint32_t payload_len, void *out, uint32_t *out_len)
{
    struct ramdisk_ctrl_hdr hdr;
    struct ramdisk_ctrl_resp resp;
    int fd = connect_sock(path);
    int status;

    if (fd < 0)
        return -errno;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = RAMDISK_CTRL_MAGIC;
    hdr.version = RAMDISK_CTRL_VERSION;
    hdr.opcode = opcode;
    hdr.payload_len = payload_len;
    hdr.request_id = 99;
    CHECK(write_full(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr));
    if (payload_len != 0)
        CHECK(write_full(fd, payload, payload_len) == (ssize_t)payload_len);
    CHECK(read_full(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp));
    if (resp.payload_len != 0) {
        CHECK(out != NULL && out_len != NULL && *out_len >= resp.payload_len);
        CHECK(read_full(fd, out, resp.payload_len) == (ssize_t)resp.payload_len);
        *out_len = resp.payload_len;
    }
    status = resp.status;
    close(fd);
    return status;
}

int main(void)
{
    const char *sock = "build/nbd-ramdisk-test.sock";
    struct ramdisk_backend b;
    struct ramdisk_urma_mgr mgr;
    struct ramdisk_ctrl_server server;
    struct ramdisk_urma_config ucfg = {
        .enable = false,
        .use_mock = true,
        .queue_depth = 4,
    };
    struct ramdisk_ctrl_server_config scfg;
    struct ramdisk_ctrl_status st;
    struct ramdisk_ctrl_peer_connect peer;
    uint32_t len;
    int rc;

    CHECK(ramdisk_backend_create(&b, 32768, 4096) == 0);
    CHECK(ramdisk_urma_mgr_init(&mgr, &b, &ucfg) == 0);
    memset(&scfg, 0, sizeof(scfg));
    scfg.socket_path = sock;
    scfg.backend = &b;
    scfg.urma = &mgr;
    rc = ramdisk_ctrl_server_start(&server, &scfg);
    if (rc == -EPERM || rc == -EACCES) {
        ramdisk_urma_mgr_destroy(&mgr);
        ramdisk_backend_destroy(&b);
        puts("test_ctrl_server skipped: sandbox does not allow Unix socket bind");
        return 0;
    }
    CHECK(rc == 0);

    memset(&st, 0, sizeof(st));
    len = sizeof(st);
    CHECK(transact(sock, RAMDISK_CTRL_QUERY_STATUS, NULL, 0, &st, &len) == 0);
    CHECK(st.size == 32768);
    CHECK(st.urma_enabled == 0);

    CHECK(transact(sock, RAMDISK_CTRL_URMA_ENABLE, NULL, 0, NULL, NULL) == 0);
    memset(&peer, 0, sizeof(peer));
    peer.peer_id = 55;
    peer.seg_va = 0x400000;
    peer.seg_len = 32768;
    CHECK(transact(sock, RAMDISK_CTRL_PEER_CONNECT, &peer, sizeof(peer), NULL, NULL) == 0);

    memset(&st, 0, sizeof(st));
    len = sizeof(st);
    CHECK(transact(sock, RAMDISK_CTRL_QUERY_STATUS, NULL, 0, &st, &len) == 0);
    CHECK(st.urma_enabled == 1);
    CHECK(st.peer_count == 1);

    ramdisk_ctrl_server_stop(&server);
    ramdisk_urma_mgr_destroy(&mgr);
    ramdisk_backend_destroy(&b);
    puts("test_ctrl_server passed");
    return 0;
}
