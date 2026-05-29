#include "ramdisk_ctrl_client.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "urma_api.h"


struct RsJettyKeyInfo {
    urma_jetty_id_t jettyId;
    urma_transport_mode_t transMode;
};

static ssize_t read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);

        if (n == 0)
            return -EPIPE;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
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

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static int connect_sock(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    if (path == NULL)
        return -EINVAL;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -errno;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int rc = -errno;

        close(fd);
        return rc;
    }
    return fd;
}

int ramdisk_ctrl_transact(const char *sock_path, uint16_t opcode,
                          const void *payload, uint32_t payload_len,
                          void *resp_payload, uint32_t *resp_payload_len,
                          uint64_t request_id)
{
    struct ramdisk_ctrl_hdr hdr;
    struct ramdisk_ctrl_resp resp;
    int fd;
    int rc;

    if (payload_len != 0 && payload == NULL)
        return -EINVAL;

    fd = connect_sock(sock_path);
    if (fd < 0)
        return fd;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = RAMDISK_CTRL_MAGIC;
    hdr.version = RAMDISK_CTRL_VERSION;
    hdr.opcode = opcode;
    hdr.payload_len = payload_len;
    hdr.request_id = request_id;

    rc = (int)write_full(fd, &hdr, sizeof(hdr));
    if (rc < 0)
        goto out;
    if (payload_len != 0) {
        rc = (int)write_full(fd, payload, payload_len);
        if (rc < 0)
            goto out;
    }

    rc = (int)read_full(fd, &resp, sizeof(resp));
    if (rc < 0)
        goto out;
    if (resp.hdr.magic != RAMDISK_CTRL_MAGIC ||
        resp.hdr.version != RAMDISK_CTRL_VERSION) {
        rc = -EPROTO;
        goto out;
    }

    if (resp.payload_len != 0) {
        if (resp_payload == NULL || resp_payload_len == NULL ||
            *resp_payload_len < resp.payload_len) {
            rc = -ENOSPC;
            goto out;
        }
        rc = (int)read_full(fd, resp_payload, resp.payload_len);
        if (rc < 0)
            goto out;
        *resp_payload_len = resp.payload_len;
    }

    rc = resp.status;
out:
    close(fd);
    return rc;
}

int ramdisk_ctrl_query_status(const char *sock_path,
                              struct ramdisk_ctrl_status *status)
{
    uint32_t len = sizeof(*status);

    if (status == NULL)
        return -EINVAL;
    memset(status, 0, sizeof(*status));
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_QUERY_STATUS, NULL, 0,
                                 status, &len, 1);
}

int ramdisk_ctrl_enable_urma(const char *sock_path)
{
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_URMA_ENABLE, NULL, 0,
                                 NULL, NULL, 1);
}

int ramdisk_ctrl_disable_urma(const char *sock_path)
{
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_URMA_DISABLE, NULL, 0,
                                 NULL, NULL, 1);
}

int ramdisk_ctrl_peer_connect(const char *sock_path,
                              const struct ramdisk_ctrl_peer_connect_info *peer)
{
    if (peer == NULL)
        return -EINVAL;
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_PEER_CONNECT, peer,
                                 sizeof(*peer), NULL, NULL, 1);
}

int ramdisk_ctrl_peer_disconnect(const char *sock_path, uint64_t peer_id)
{
    struct ramdisk_ctrl_peer_disconnect disc;

    memset(&disc, 0, sizeof(disc));
    disc.peer_id = peer_id;
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_PEER_DISCONNECT,
                                 &disc, sizeof(disc), NULL, NULL, 1);
}

int ramdisk_ctrl_urma_transfer(const char *sock_path,
                               const struct ramdisk_ctrl_urma_transfer *xfer)
{
    if (xfer == NULL)
        return -EINVAL;
    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_URMA_TRANSFER, xfer,
                                 sizeof(*xfer), NULL, NULL, xfer->request_id);
}

static int hex_value(int ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

int ramdisk_ctrl_parse_eid(const char *text, uint8_t eid[16])
{
    size_t i;
    int high = -1;
    uint32_t out = 0;

    if (text == NULL || eid == NULL)
        return -EINVAL;

    memset(eid, 0, 16);
    for (i = 0; text[i] != '\0'; i++) {
        int v;

        if (text[i] == ':' || text[i] == '-' || text[i] == '.')
            continue;
        v = hex_value((unsigned char)text[i]);
        if (v < 0)
            return -EINVAL;
        if (high < 0) {
            high = v;
        } else {
            if (out >= 16)
                return -EINVAL;
            eid[out++] = (uint8_t)((high << 4) | v);
            high = -1;
        }
    }
    return high == -1 && out == 16 ? 0 : -EINVAL;
}

int ramdisk_ctrl_parse_peer_connect(const char *sock_path, uint64_t peer_id,
                                    const uint8_t *jetty_info,
                                    uint32_t jetty_info_len,
                                    const uint8_t *seg_info,
                                    uint32_t seg_info_len)
{
    if (jetty_info == NULL || jetty_info_len == 0 ||
        seg_info == NULL || seg_info_len == 0)
        return -EINVAL;

    struct ramdisk_ctrl_peer_connect_info peer = {0};

    struct RsJettyKeyInfo *jettyKeyInfo = (struct RsJettyKeyInfo *)jetty_info;

    urma_seg_t *out_key_urma = (urma_seg_t*)seg_info;

    
    peer.peer_id = peer_id;
    memcpy(peer.eid, out_key_urma->ubva.eid.raw, sizeof(peer.eid));
    // peer.uasid = urma_ctx->uasid;
    peer.seg_va = out_key_urma->ubva.va;
    peer.seg_len = out_key_urma->len;
    peer.seg_token_id = out_key_urma->token_id;
    peer.jetty_id = jettyKeyInfo->jettyId.id;

    printf("$$$$ seg va is %llx\n", (unsigned long long)peer.seg_va);

    return ramdisk_ctrl_transact(sock_path, RAMDISK_CTRL_PEER_CONNECT, &peer,
                                 sizeof(peer), NULL, NULL, 1);
}
