#include "ramdisk_ctrl_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "ramdisk_ctrl.h"
#include "ramdisk_log.h"

static ssize_t read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n == 0)
            return done == 0 ? 0 : -EPIPE;
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

static int send_response(int fd, const struct ramdisk_ctrl_hdr *req,
                         int status, const void *payload, uint32_t payload_len)
{
    struct ramdisk_ctrl_resp resp;
    int rc;

    memset(&resp, 0, sizeof(resp));
    resp.hdr.magic = RAMDISK_CTRL_MAGIC;
    resp.hdr.version = RAMDISK_CTRL_VERSION;
    resp.hdr.opcode = req != NULL ? req->opcode : 0;
    resp.hdr.request_id = req != NULL ? req->request_id : 0;
    resp.hdr.payload_len = sizeof(resp) - sizeof(resp.hdr) + payload_len;
    resp.status = status;
    resp.payload_len = payload_len;

    rc = (int)write_full(fd, &resp, sizeof(resp));
    if (rc < 0)
        return rc;
    if (payload_len != 0)
        return (int)write_full(fd, payload, payload_len);
    return 0;
}

static int handle_request(struct ramdisk_ctrl_server *server, int fd,
                          const struct ramdisk_ctrl_hdr *hdr,
                          const uint8_t *payload)
{
    struct ramdisk_ctrl_status status;
    int rc = 0;

    switch (hdr->opcode) {
    case RAMDISK_CTRL_QUERY_STATUS:
        memset(&status, 0, sizeof(status));
        status.size = ramdisk_backend_size(server->backend);
        status.block_size = ramdisk_backend_block_size(server->backend);
        ramdisk_urma_fill_status(server->urma, &status);
        return send_response(fd, hdr, 0, &status, sizeof(status));
    case RAMDISK_CTRL_URMA_ENABLE:
        rc = ramdisk_urma_enable(server->urma);
        return send_response(fd, hdr, rc, NULL, 0);
    case RAMDISK_CTRL_URMA_DISABLE:
        rc = ramdisk_urma_disable(server->urma);
        return send_response(fd, hdr, rc, NULL, 0);
    case RAMDISK_CTRL_PEER_CONNECT:
        if (hdr->payload_len != sizeof(struct ramdisk_ctrl_peer_connect)) {
            rc = -EINVAL;
        } else {
            rc = ramdisk_urma_peer_connect(server->urma,
                    (const struct ramdisk_ctrl_peer_connect *)payload);
        }
        return send_response(fd, hdr, rc, NULL, 0);
    case RAMDISK_CTRL_PEER_DISCONNECT:
        if (hdr->payload_len != sizeof(struct ramdisk_ctrl_peer_disconnect)) {
            rc = -EINVAL;
        } else {
            const struct ramdisk_ctrl_peer_disconnect *disc =
                (const struct ramdisk_ctrl_peer_disconnect *)payload;
            rc = ramdisk_urma_peer_disconnect(server->urma, disc->peer_id);
        }
        return send_response(fd, hdr, rc, NULL, 0);
    case RAMDISK_CTRL_URMA_TRANSFER:
        if (hdr->payload_len != sizeof(struct ramdisk_ctrl_urma_transfer)) {
            rc = -EINVAL;
        } else {
            rc = ramdisk_urma_transfer_sync(server->urma,
                    (const struct ramdisk_ctrl_urma_transfer *)payload, 30000);
        }
        return send_response(fd, hdr, rc, NULL, 0);
    default:
        RD_LOG_ERR("unknown control opcode=%u", hdr->opcode);
        return send_response(fd, hdr, -EOPNOTSUPP, NULL, 0);
    }
}

static void serve_client(struct ramdisk_ctrl_server *server, int fd)
{
    for (;;) {
        struct ramdisk_ctrl_hdr hdr;
        uint8_t payload[RAMDISK_CTRL_MAX_PAYLOAD];
        ssize_t n;

        n = read_full(fd, &hdr, sizeof(hdr));
        if (n == 0)
            return;
        if (n < 0) {
            RD_LOG_ERR("control header read failed rc=%zd", n);
            return;
        }
        if (hdr.magic != RAMDISK_CTRL_MAGIC ||
            hdr.version != RAMDISK_CTRL_VERSION ||
            hdr.payload_len > RAMDISK_CTRL_MAX_PAYLOAD) {
            RD_LOG_ERR("bad control header magic=0x%x version=%u payload=%u",
                       hdr.magic, hdr.version, hdr.payload_len);
            send_response(fd, &hdr, -EINVAL, NULL, 0);
            return;
        }
        if (hdr.payload_len != 0) {
            n = read_full(fd, payload, hdr.payload_len);
            if (n < 0) {
                RD_LOG_ERR("control payload read failed rc=%zd", n);
                return;
            }
        }
        RD_LOG_INFO("control request opcode=%u request=%llu payload=%u",
                    hdr.opcode, (unsigned long long)hdr.request_id,
                    hdr.payload_len);
        if (handle_request(server, fd, &hdr, payload) < 0)
            return;
    }
}

static void *ctrl_server_main(void *arg)
{
    struct ramdisk_ctrl_server *server = arg;

    RD_LOG_INFO("control server listening path=%s", server->socket_path);
    while (server->running) {
        int fd = accept(server->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (server->running)
                RD_LOG_ERRNO("control accept failed");
            continue;
        }
        serve_client(server, fd);
        close(fd);
    }
    return NULL;
}

int ramdisk_ctrl_server_start(struct ramdisk_ctrl_server *server,
                              const struct ramdisk_ctrl_server_config *config)
{
    struct sockaddr_un addr;
    int rc;

    if (server == NULL || config == NULL || config->socket_path == NULL ||
        config->backend == NULL || config->urma == NULL)
        return -EINVAL;
    memset(server, 0, sizeof(*server));

    if (strlen(config->socket_path) >= sizeof(server->socket_path))
        return -ENAMETOOLONG;
    snprintf(server->socket_path, sizeof(server->socket_path), "%s",
             config->socket_path);

    server->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        RD_LOG_ERRNO("control socket create failed");
        return -errno;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", server->socket_path);

    unlink(server->socket_path);
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        rc = -errno;
        RD_LOG_ERR("control bind failed path=%s rc=%d errno=%d (%s)",
                   server->socket_path, rc, errno, strerror(errno));
        close(server->listen_fd);
        return rc;
    }
    chmod(server->socket_path, 0600);
    if (listen(server->listen_fd, 16) < 0) {
        rc = -errno;
        RD_LOG_ERR("control listen failed rc=%d errno=%d (%s)",
                   rc, errno, strerror(errno));
        close(server->listen_fd);
        unlink(server->socket_path);
        return rc;
    }

    server->backend = config->backend;
    server->urma = config->urma;
    server->running = true;
    server->initialized = true;

    rc = pthread_create(&server->thread, NULL, ctrl_server_main, server);
    if (rc != 0) {
        RD_LOG_ERR("control thread create failed rc=%d", rc);
        ramdisk_ctrl_server_stop(server);
        return -rc;
    }
    return 0;
}

void ramdisk_ctrl_server_stop(struct ramdisk_ctrl_server *server)
{
    if (server == NULL || !server->initialized)
        return;
    server->running = false;
    shutdown(server->listen_fd, SHUT_RDWR);
    close(server->listen_fd);
    pthread_join(server->thread, NULL);
    unlink(server->socket_path);
    server->initialized = false;
}
