#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ramdisk_backend.h"

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    struct ramdisk_backend b;
    uint8_t in[4096];
    uint8_t out[4096];
    uint8_t zero[4096];
    int rc;

    memset(in, 0xab, sizeof(in));
    memset(out, 0, sizeof(out));
    memset(zero, 0, sizeof(zero));

    CHECK(ramdisk_backend_create(NULL, 4096, 4096) == -EINVAL);
    CHECK(ramdisk_backend_create(&b, 0, 4096) == -EINVAL);
    CHECK(ramdisk_backend_create(&b, 4096, 123) == -EINVAL);

    rc = ramdisk_backend_create(&b, 8192, 4096);
    CHECK(rc == 0);
    CHECK(ramdisk_backend_size(&b) == 8192);
    CHECK(ramdisk_backend_block_size(&b) == 4096);

    CHECK(ramdisk_backend_write(&b, 0, in, sizeof(in)) == 0);
    CHECK(ramdisk_backend_read(&b, 0, out, sizeof(out)) == 0);
    CHECK(memcmp(in, out, sizeof(in)) == 0);

    memset(out, 0xcd, sizeof(out));
    CHECK(ramdisk_backend_read(&b, 4096, out, sizeof(out)) == 0);
    CHECK(memcmp(out, zero, sizeof(out)) == 0);

    CHECK(ramdisk_backend_read(&b, 8192, out, 1) == -ERANGE);
    CHECK(ramdisk_backend_write(&b, 8191, in, 2) == -ERANGE);
    CHECK(ramdisk_backend_validate_range(&b, UINT64_MAX, 1) == -ERANGE);

    CHECK(ramdisk_backend_zero(&b, 0, sizeof(in)) == 0);
    CHECK(ramdisk_backend_read(&b, 0, out, sizeof(out)) == 0);
    CHECK(memcmp(out, zero, sizeof(out)) == 0);

    ramdisk_backend_destroy(&b);
    puts("test_backend passed");
    return 0;
}
