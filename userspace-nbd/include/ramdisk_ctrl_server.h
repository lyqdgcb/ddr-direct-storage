#ifndef RAMDISK_CTRL_SERVER_H
#define RAMDISK_CTRL_SERVER_H

#include <pthread.h>
#include <stdbool.h>

#include "ramdisk_backend.h"
#include "ramdisk_urma.h"

struct ramdisk_ctrl_server {
    char socket_path[256];
    int listen_fd;
    pthread_t thread;
    bool initialized;
    bool running;
    struct ramdisk_backend *backend;
    struct ramdisk_urma_mgr *urma;
};

struct ramdisk_ctrl_server_config {
    const char *socket_path;
    struct ramdisk_backend *backend;
    struct ramdisk_urma_mgr *urma;
};

int ramdisk_ctrl_server_start(struct ramdisk_ctrl_server *server,
                              const struct ramdisk_ctrl_server_config *config);
void ramdisk_ctrl_server_stop(struct ramdisk_ctrl_server *server);

#endif
