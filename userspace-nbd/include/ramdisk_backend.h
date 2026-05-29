#ifndef RAMDISK_BACKEND_H
#define RAMDISK_BACKEND_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (0x1 << PAGE_SHIFT) // 4KB

struct ramdisk_backend {
    void *base;
    uint64_t size;
    uint32_t block_size;
    pthread_mutex_t lock;
    bool initialized;
};

int ramdisk_backend_create(struct ramdisk_backend *backend, uint64_t size,
                           uint32_t block_size);
void ramdisk_backend_destroy(struct ramdisk_backend *backend);

int ramdisk_backend_read(struct ramdisk_backend *backend, uint64_t offset,
                         void *buf, uint32_t len);
int ramdisk_backend_write(struct ramdisk_backend *backend, uint64_t offset,
                          const void *buf, uint32_t len);
int ramdisk_backend_zero(struct ramdisk_backend *backend, uint64_t offset,
                         uint32_t len);

int ramdisk_backend_validate_range(const struct ramdisk_backend *backend,
                                   uint64_t offset, uint64_t len);
int ramdisk_backend_lock(struct ramdisk_backend *backend);
void ramdisk_backend_unlock(struct ramdisk_backend *backend);

void *ramdisk_backend_base(struct ramdisk_backend *backend);
uint64_t ramdisk_backend_size(const struct ramdisk_backend *backend);
uint32_t ramdisk_backend_block_size(const struct ramdisk_backend *backend);

#endif
