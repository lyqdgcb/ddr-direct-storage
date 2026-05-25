#ifdef __linux__

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/nbd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nbd_protocol.h"
#include "ramdisk_backend.h"
#include "ramdisk_ctrl_server.h"
#include "ramdisk_log.h"
#include "ramdisk_urma.h"

#ifndef NBD_SET_FLAGS
#define NBD_SET_FLAGS _IO(0xab, 10)
#endif
#ifndef NBD_FLAG_SEND_FLUSH
#define NBD_FLAG_SEND_FLUSH (1 << 2)
#endif
#ifndef NBD_FLAG_SEND_TRIM
#define NBD_FLAG_SEND_TRIM (1 << 5)
#endif

struct daemon_ctx {
    const char *device;
    const char *control_sock;
    uint64_t size;
    uint32_t block_size;
    bool enable_urma;
    bool use_mock_urma;
    const char *urma_eid;
    uint32_t eid_index;
    uint32_t urma_trans_mode;
    uint32_t urma_tp_type;
    uint32_t urma_token;
    uint32_t queue_depth;
    int nbd_fd;
    int app_sock;
    int kernel_sock;
    pthread_t nbd_thread;
    bool nbd_thread_started;
    struct ramdisk_backend backend;
    struct ramdisk_urma_mgr urma;
    struct ramdisk_ctrl_server ctrl;
};

static volatile sig_atomic_t g_stop;
static struct daemon_ctx *g_ctx;

static uint64_t ntohll_u64(uint64_t value)
{
    uint32_t high = ntohl((uint32_t)(value >> 32));
    uint32_t low = ntohl((uint32_t)(value & 0xffffffffU));
    return ((uint64_t)low << 32) | high;
}

static uint64_t htonll_u64(uint64_t value)
{
    uint32_t high = htonl((uint32_t)(value >> 32));
    uint32_t low = htonl((uint32_t)(value & 0xffffffffU));
    return ((uint64_t)low << 32) | high;
}

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

static uint32_t nbd_error_from_rc(int rc)
{
    if (rc == 0)
        return 0;
    if (rc < 0)
        rc = -rc;
    return htonl((uint32_t)rc);
}

static int send_reply(int fd, const struct ramdisk_nbd_request *req, int rc)
{
    struct ramdisk_nbd_reply reply;

    memset(&reply, 0, sizeof(reply));
    reply.magic = htonl(RAMDISK_NBD_REPLY_MAGIC);
    reply.error = nbd_error_from_rc(rc);
    memcpy(reply.handle, req->handle, sizeof(reply.handle));
    return (int)write_full(fd, &reply, sizeof(reply));
}

static int serve_nbd(struct daemon_ctx *ctx)
{
    uint8_t *buf = NULL;
    uint32_t buf_cap = 0;

    while (!g_stop) {
        struct ramdisk_nbd_request req;
        uint32_t magic;
        uint32_t type;
        uint64_t from;
        uint32_t len;
        int rc = 0;
        ssize_t n = read_full(ctx->app_sock, &req, sizeof(req));

        if (n == 0)
            break;
        if (n < 0) {
            RD_LOG_ERR("NBD request read failed rc=%zd", n);
            break;
        }

        magic = ntohl(req.magic);
        type = ntohl(req.type);
        from = ntohll_u64(req.from);
        len = ntohl(req.len);
        if (magic != RAMDISK_NBD_REQUEST_MAGIC) {
            RD_LOG_ERR("bad NBD magic=0x%x type=%u from=%" PRIu64 " len=%u",
                       magic, type, from, len);
            break;
        }

        if (len > buf_cap) {
            uint8_t *new_buf = realloc(buf, len);
            if (new_buf == NULL) {
                rc = -ENOMEM;
            } else {
                buf = new_buf;
                buf_cap = len;
            }
        }

        switch (type) {
        case RAMDISK_NBD_CMD_READ:
            if (rc == 0)
                rc = ramdisk_backend_read(&ctx->backend, from, buf, len);
            if (send_reply(ctx->app_sock, &req, rc) < 0)
                goto out;
            if (rc == 0 && write_full(ctx->app_sock, buf, len) < 0)
                goto out;
            break;
        case RAMDISK_NBD_CMD_WRITE:
            if (read_full(ctx->app_sock, buf, len) < 0)
                goto out;
            if (rc == 0)
                rc = ramdisk_backend_write(&ctx->backend, from, buf, len);
            if (send_reply(ctx->app_sock, &req, rc) < 0)
                goto out;
            break;
        case RAMDISK_NBD_CMD_FLUSH:
            if (send_reply(ctx->app_sock, &req, 0) < 0)
                goto out;
            break;
        case RAMDISK_NBD_CMD_TRIM:
            rc = ramdisk_backend_zero(&ctx->backend, from, len);
            if (send_reply(ctx->app_sock, &req, rc) < 0)
                goto out;
            break;
        case RAMDISK_NBD_CMD_DISC:
            RD_LOG_INFO("NBD disconnect command received");
            send_reply(ctx->app_sock, &req, 0);
            goto out;
        default:
            RD_LOG_ERR("unsupported NBD command type=%u from=%" PRIu64 " len=%u",
                       type, from, len);
            if (type == RAMDISK_NBD_CMD_WRITE)
                read_full(ctx->app_sock, buf, len);
            if (send_reply(ctx->app_sock, &req, -EIO) < 0)
                goto out;
            break;
        }

        RD_LOG_INFO("NBD cmd=%u offset=%" PRIu64 " len=%u rc=%d",
                    type, from, len, rc);
    }

out:
    free(buf);
    return 0;
}

static void *nbd_do_it_thread(void *arg)
{
    struct daemon_ctx *ctx = arg;
    int rc;

    rc = ioctl(ctx->nbd_fd, NBD_DO_IT);
    if (rc < 0 && !g_stop)
        RD_LOG_ERRNO("NBD_DO_IT failed");
    ioctl(ctx->nbd_fd, NBD_CLEAR_QUE);
    return NULL;
}

static void signal_handler(int signo)
{
    (void)signo;
    g_stop = 1;
    if (g_ctx != NULL && g_ctx->nbd_fd >= 0)
        ioctl(g_ctx->nbd_fd, NBD_DISCONNECT);
}

static uint64_t parse_size(const char *text)
{
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    uint64_t mul = 1;

    if (end != NULL && *end != '\0') {
        if (strcmp(end, "K") == 0 || strcmp(end, "k") == 0)
            mul = 1024ULL;
        else if (strcmp(end, "M") == 0 || strcmp(end, "m") == 0)
            mul = 1024ULL * 1024ULL;
        else if (strcmp(end, "G") == 0 || strcmp(end, "g") == 0)
            mul = 1024ULL * 1024ULL * 1024ULL;
        else
            return 0;
    }
    if (value > UINT64_MAX / mul)
        return 0;
    return (uint64_t)value * mul;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --device /dev/nbd0 --size 128M [options]\n"
            "Options:\n"
            "  --block-size N              block size, default 4096\n"
            "  --control-sock PATH         default /run/nbd-ramdisk/control.sock\n"
            "  --enable-urma               enable URMA special path\n"
            "  --real-urma                 use real URMA provider skeleton instead of mock\n"
            "  --urma-eid EID_HEX          local 16-byte URMA EID for --real-urma\n"
            "                              format: 32 hex digits, ':'/'-' allowed\n"
            "  --eid-index N               optional EID index validation\n"
            "  --urma-trans-mode rm|rc|um  transport mode, default rc\n"
            "  --urma-tp-type rtp|ctp|utp  TP type, default rtp\n"
            "  --urma-token N              token value, default 0xACFE\n"
            "  --queue-depth N             URMA queue depth, default 32\n",
            prog);
}

static int parse_trans_mode(const char *s)
{
    if (strcmp(s, "rm") == 0)
        return 0;
    if (strcmp(s, "rc") == 0)
        return 1;
    if (strcmp(s, "um") == 0)
        return 2;
    return -1;
}

static int parse_tp_type(const char *s)
{
    if (strcmp(s, "rtp") == 0)
        return 0;
    if (strcmp(s, "ctp") == 0)
        return 1;
    if (strcmp(s, "utp") == 0)
        return 2;
    return -1;
}

static int parse_args(int argc, char **argv, struct daemon_ctx *ctx)
{
    static const struct option opts[] = {
        {"device", required_argument, NULL, 'd'},
        {"size", required_argument, NULL, 's'},
        {"block-size", required_argument, NULL, 'b'},
        {"control-sock", required_argument, NULL, 'c'},
        {"enable-urma", no_argument, NULL, 'u'},
        {"real-urma", no_argument, NULL, 'r'},
        {"urma-eid", required_argument, NULL, 1000},
        {"eid-index", required_argument, NULL, 1001},
        {"urma-trans-mode", required_argument, NULL, 1002},
        {"urma-tp-type", required_argument, NULL, 1003},
        {"urma-token", required_argument, NULL, 1004},
        {"queue-depth", required_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int ch;

    ctx->block_size = 4096;
    ctx->control_sock = "/run/nbd-ramdisk/control.sock";
    ctx->queue_depth = 32;
    ctx->use_mock_urma = false;
    ctx->eid_index = UINT32_MAX;
    ctx->urma_trans_mode = 0;
    ctx->urma_tp_type = 1;
    ctx->urma_token = 0xACFE;
    ctx->nbd_fd = -1;
    ctx->app_sock = -1;
    ctx->kernel_sock = -1;

    while ((ch = getopt_long(argc, argv, "d:s:b:c:urq:h", opts, NULL)) != -1) {
        switch (ch) {
        case 'd':
            ctx->device = optarg;
            break;
        case 's':
            ctx->size = parse_size(optarg);
            break;
        case 'b':
            ctx->block_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'c':
            ctx->control_sock = optarg;
            break;
        case 'u':
            ctx->enable_urma = true;
            break;
        case 'r':
            ctx->use_mock_urma = false;
            break;
        case 'q':
            ctx->queue_depth = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 1000:
            ctx->urma_eid = optarg;
            break;
        case 1001:
            ctx->eid_index = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 1002: {
            int mode = parse_trans_mode(optarg);
            if (mode < 0)
                return -EINVAL;
            ctx->urma_trans_mode = (uint32_t)mode;
            break;
        }
        case 1003: {
            int type = parse_tp_type(optarg);
            if (type < 0)
                return -EINVAL;
            ctx->urma_tp_type = (uint32_t)type;
            break;
        }
        case 1004:
            ctx->urma_token = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return -EINVAL;
        }
    }

    if (ctx->device == NULL || ctx->size == 0) {
        usage(argv[0]);
        return -EINVAL;
    }
    return 0;
}

static int setup_nbd(struct daemon_ctx *ctx)
{
    int socks[2];
    int flags = NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_TRIM;
    int rc;

    ctx->nbd_fd = open(ctx->device, O_RDWR);
    if (ctx->nbd_fd < 0)
        return -errno;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, socks) < 0)
        return -errno;
    ctx->app_sock = socks[0];
    ctx->kernel_sock = socks[1];

    if (ioctl(ctx->nbd_fd, NBD_SET_BLKSIZE, ctx->block_size) < 0)
        return -errno;
    if (ioctl(ctx->nbd_fd, NBD_SET_SIZE, ctx->size) < 0)
        return -errno;
    ioctl(ctx->nbd_fd, NBD_SET_FLAGS, flags);
    if (ioctl(ctx->nbd_fd, NBD_SET_SOCK, ctx->kernel_sock) < 0)
        return -errno;

    rc = pthread_create(&ctx->nbd_thread, NULL, nbd_do_it_thread, ctx);
    if (rc == 0)
        ctx->nbd_thread_started = true;
    return rc == 0 ? 0 : -rc;
}

static void cleanup(struct daemon_ctx *ctx)
{
    if (ctx->ctrl.initialized)
        ramdisk_ctrl_server_stop(&ctx->ctrl);
    if (ctx->nbd_fd >= 0) {
        ioctl(ctx->nbd_fd, NBD_DISCONNECT);
        ioctl(ctx->nbd_fd, NBD_CLEAR_SOCK);
        ioctl(ctx->nbd_fd, NBD_CLEAR_QUE);
    }
    if (ctx->app_sock >= 0)
        close(ctx->app_sock);
    if (ctx->kernel_sock >= 0)
        close(ctx->kernel_sock);
    if (ctx->nbd_thread_started)
        pthread_join(ctx->nbd_thread, NULL);
    if (ctx->nbd_fd >= 0)
        close(ctx->nbd_fd);
    if (ctx->urma.initialized)
        ramdisk_urma_mgr_destroy(&ctx->urma);
    ramdisk_backend_destroy(&ctx->backend);
}

int main(int argc, char **argv)
{
    struct daemon_ctx ctx;
    struct ramdisk_urma_config urma_cfg;
    struct ramdisk_ctrl_server_config ctrl_cfg;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    rc = parse_args(argc, argv, &ctx);
    if (rc != 0)
        return 2;

    g_ctx = &ctx;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    rc = ramdisk_backend_create(&ctx.backend, ctx.size, ctx.block_size);
    if (rc != 0)
        goto out;

    memset(&urma_cfg, 0, sizeof(urma_cfg));
    urma_cfg.enable = ctx.enable_urma;
    urma_cfg.use_mock = ctx.use_mock_urma;
    urma_cfg.queue_depth = ctx.queue_depth;
    urma_cfg.urma_dev = ctx.urma_eid;
    urma_cfg.eid_index = ctx.eid_index;
    urma_cfg.trans_mode = ctx.urma_trans_mode;
    urma_cfg.tp_type = ctx.urma_tp_type;
    urma_cfg.local_token_value = ctx.urma_token;
    rc = ramdisk_urma_mgr_init(&ctx.urma, &ctx.backend, &urma_cfg);
    if (rc != 0)
        goto out;

    memset(&ctrl_cfg, 0, sizeof(ctrl_cfg));
    ctrl_cfg.socket_path = ctx.control_sock;
    ctrl_cfg.backend = &ctx.backend;
    ctrl_cfg.urma = &ctx.urma;
    rc = ramdisk_ctrl_server_start(&ctx.ctrl, &ctrl_cfg);
    if (rc != 0)
        goto out;

    rc = setup_nbd(&ctx);
    if (rc != 0) {
        RD_LOG_ERR("NBD setup failed rc=%d", rc);
        goto out;
    }

    RD_LOG_INFO("nbd-ramdisk serving device=%s size=%" PRIu64 " block=%u ctrl=%s urma=%d",
                ctx.device, ctx.size, ctx.block_size, ctx.control_sock,
                ctx.enable_urma ? 1 : 0);
    serve_nbd(&ctx);

out:
    cleanup(&ctx);
    if (rc != 0)
        RD_LOG_ERR("daemon exit rc=%d", rc);
    return rc == 0 ? 0 : 1;
}

#else

#include <stdio.h>

int main(void)
{
    fprintf(stderr, "nbd-ramdisk requires Linux NBD ioctls\n");
    return 1;
}

#endif
