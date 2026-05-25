#include "ramdisk_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "ramdisk_log.h"

#define RAMDISK_MAX_BLOCK_SIZE (1024U * 1024U)

static bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static int validate_block_size(uint32_t block_size)
{
    if (!is_power_of_two(block_size))
        return -EINVAL;
    if (block_size < 512 || block_size > RAMDISK_MAX_BLOCK_SIZE)
        return -EINVAL;
    return 0;
}

int ramdisk_backend_create(struct ramdisk_backend *backend, uint64_t size,
                           uint32_t block_size)
{
    int rc;

    if (backend == NULL)
        return -EINVAL;
    memset(backend, 0, sizeof(*backend));

    if (size == 0 || size > SIZE_MAX) {
        RD_LOG_ERR("invalid backend size=%llu", (unsigned long long)size);
        return -EINVAL;
    }

    rc = validate_block_size(block_size);
    if (rc != 0) {
        RD_LOG_ERR("invalid block size=%u", block_size);
        return rc;
    }

    rc = pthread_mutex_init(&backend->lock, NULL);
    if (rc != 0) {
        RD_LOG_ERR("pthread_mutex_init failed rc=%d", rc);
        return -rc;
    }

    backend->base = memalign(PAGE_SIZE, (size_t)size);
    if (backend->base == NULL) {
        pthread_mutex_destroy(&backend->lock);
        RD_LOG_ERR("backend allocation failed size=%llu",
                   (unsigned long long)size);
        return -ENOMEM;
    }
    RD_LOG_INFO("before memset backend created base=%p size=%llu block_size=%u",
                backend->base, (unsigned long long)size, block_size);
    (void)memset(backend->base, 0, size);
    backend->size = size;
    backend->block_size = block_size;
    backend->initialized = true;
    RD_LOG_INFO("backend created base=%p size=%llu block_size=%u",
                backend->base, (unsigned long long)size, block_size);
    return 0;
}

void ramdisk_backend_destroy(struct ramdisk_backend *backend)
{
    if (backend == NULL || !backend->initialized)
        return;

    RD_LOG_INFO("backend destroy base=%p size=%llu", backend->base,
                (unsigned long long)backend->size);
    free(backend->base);
    backend->base = NULL;
    backend->size = 0;
    backend->block_size = 0;
    backend->initialized = false;
    pthread_mutex_destroy(&backend->lock);
}

int ramdisk_backend_validate_range(const struct ramdisk_backend *backend,
                                   uint64_t offset, uint64_t len)
{
    if (backend == NULL || !backend->initialized || backend->base == NULL)
        return -EINVAL;
    if (offset > backend->size)
        return -ERANGE;
    if (len > backend->size - offset)
        return -ERANGE;
    return 0;
}

int ramdisk_backend_lock(struct ramdisk_backend *backend)
{
    int rc;

    if (backend == NULL || !backend->initialized)
        return -EINVAL;
    rc = pthread_mutex_lock(&backend->lock);
    return rc == 0 ? 0 : -rc;
}

void ramdisk_backend_unlock(struct ramdisk_backend *backend)
{
    if (backend != NULL && backend->initialized)
        pthread_mutex_unlock(&backend->lock);
}

int ramdisk_backend_read(struct ramdisk_backend *backend, uint64_t offset,
                         void *buf, uint32_t len)
{
    int rc;

    if (buf == NULL)
        return -EINVAL;
    rc = ramdisk_backend_validate_range(backend, offset, len);
    if (rc != 0)
        return rc;

    rc = ramdisk_backend_lock(backend);
    if (rc != 0)
        return rc;
    memcpy(buf, (const char *)backend->base + offset, len);
    ramdisk_backend_unlock(backend);
    return 0;
}

int ramdisk_backend_write(struct ramdisk_backend *backend, uint64_t offset,
                          const void *buf, uint32_t len)
{
    int rc;

    if (buf == NULL)
        return -EINVAL;
    rc = ramdisk_backend_validate_range(backend, offset, len);
    if (rc != 0)
        return rc;

    rc = ramdisk_backend_lock(backend);
    if (rc != 0)
        return rc;
    memcpy((char *)backend->base + offset, buf, len);
    ramdisk_backend_unlock(backend);
    return 0;
}

int ramdisk_backend_zero(struct ramdisk_backend *backend, uint64_t offset,
                         uint32_t len)
{
    int rc = ramdisk_backend_validate_range(backend, offset, len);

    if (rc != 0)
        return rc;
    rc = ramdisk_backend_lock(backend);
    if (rc != 0)
        return rc;
    memset((char *)backend->base + offset, 0, len);
    ramdisk_backend_unlock(backend);
    return 0;
}

void *ramdisk_backend_base(struct ramdisk_backend *backend)
{
    if (backend == NULL || !backend->initialized)
        return NULL;
    return backend->base;
}

uint64_t ramdisk_backend_size(const struct ramdisk_backend *backend)
{
    if (backend == NULL || !backend->initialized)
        return 0;
    return backend->size;
}

uint32_t ramdisk_backend_block_size(const struct ramdisk_backend *backend)
{
    if (backend == NULL || !backend->initialized)
        return 0;
    return backend->block_size;
}