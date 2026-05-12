#include "mock_rdma.h"
#include "rdma_ramdisk.h"
#include "target_session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_OK(expr) do { \
    int _rc = (expr); \
    if (_rc != 0) { \
        fprintf(stderr, "%s failed: rc=%d\n", #expr, _rc); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "assertion failed: %s\n", #expr); \
        return 1; \
    } \
} while (0)

static int test_lba_mapping(void)
{
    uint8_t remote[4096] = {0};
    struct mock_rdma_context rdma;
    struct rdma_ramdisk_config cfg = {
        .size_bytes = 1024 * 1024,
        .logical_block_size = 512,
    };
    struct rdma_ramdisk *disk;
    struct rdma_ramdisk_mapping mapping;
    const struct rdma_registered_mr *mr;

    mock_rdma_context_init(&rdma, remote, sizeof(remote));
    disk = rdma_ramdisk_create(&cfg, mock_rdma_ops(), &rdma);
    ASSERT_TRUE(disk != NULL);

    ASSERT_OK(rdma_ramdisk_map_lba(disk, 4, 2, &mapping));
    mr = rdma_ramdisk_mr(disk);
    ASSERT_TRUE(mapping.byte_offset == 2048);
    ASSERT_TRUE(mapping.length == 1024);
    ASSERT_TRUE(mapping.sge.addr == mr->iova + 2048);
    ASSERT_TRUE(mapping.sge.lkey == mr->lkey);

    ASSERT_TRUE(rdma_ramdisk_map_lba(disk, 2048, 1, &mapping) == -ERANGE);
    ASSERT_TRUE(rdma_ramdisk_map_lba(disk, 0, 0, &mapping) == -EINVAL);

    rdma_ramdisk_destroy(disk);
    return 0;
}

static int test_target_read_writes_to_remote(void)
{
    uint8_t remote[4096] = {0};
    struct mock_rdma_context rdma;
    struct rdma_ramdisk_config cfg = {
        .size_bytes = 1024 * 1024,
        .logical_block_size = 512,
    };
    struct rdma_ramdisk *disk;
    struct target_session session;
    struct target_io_request req = {
        .direction = TARGET_IO_READ,
        .lba = 8,
        .block_count = 2,
        .remote_addr = 128,
        .rkey = 0xCAFE,
        .remote_length = 1024,
    };

    mock_rdma_context_init(&rdma, remote, sizeof(remote));
    disk = rdma_ramdisk_create(&cfg, mock_rdma_ops(), &rdma);
    ASSERT_TRUE(disk != NULL);

    target_session_init(&session, disk, mock_rdma_ops(), &rdma);
    ASSERT_OK(target_session_connect(&session,
                                     &(struct target_connect_info){
                                         .endpoint = "mock-c",
                                     }));

    ASSERT_OK(rdma_ramdisk_fill(disk, 8, 2, 0xA5));
    ASSERT_OK(target_session_submit_io(&session, &req));
    ASSERT_TRUE(rdma.last_direction == RDMA_TRANSFER_WRITE_TO_REMOTE);
    for (size_t i = 0; i < 1024; i++) {
        ASSERT_TRUE(remote[128 + i] == 0xA5);
    }

    target_session_disconnect(&session);
    rdma_ramdisk_destroy(disk);
    return 0;
}

static int test_target_write_reads_from_remote(void)
{
    uint8_t remote[4096];
    struct mock_rdma_context rdma;
    struct rdma_ramdisk_config cfg = {
        .size_bytes = 1024 * 1024,
        .logical_block_size = 512,
    };
    struct rdma_ramdisk *disk;
    struct target_session session;
    struct target_io_request req = {
        .direction = TARGET_IO_WRITE,
        .lba = 16,
        .block_count = 4,
        .remote_addr = 512,
        .rkey = 0xCAFE,
        .remote_length = 2048,
    };

    memset(remote, 0x3C, sizeof(remote));
    mock_rdma_context_init(&rdma, remote, sizeof(remote));
    disk = rdma_ramdisk_create(&cfg, mock_rdma_ops(), &rdma);
    ASSERT_TRUE(disk != NULL);

    target_session_init(&session, disk, mock_rdma_ops(), &rdma);
    ASSERT_OK(target_session_connect(&session,
                                     &(struct target_connect_info){
                                         .endpoint = "mock-c",
                                     }));

    ASSERT_OK(target_session_submit_io(&session, &req));
    ASSERT_TRUE(rdma.last_direction == RDMA_TRANSFER_READ_FROM_REMOTE);
    ASSERT_OK(rdma_ramdisk_verify(disk, 16, 4, 0x3C));

    target_session_disconnect(&session);
    rdma_ramdisk_destroy(disk);
    return 0;
}

static int test_rejects_short_remote_buffer(void)
{
    uint8_t remote[4096] = {0};
    struct mock_rdma_context rdma;
    struct rdma_ramdisk_config cfg = {
        .size_bytes = 1024 * 1024,
        .logical_block_size = 512,
    };
    struct rdma_ramdisk *disk;
    struct target_session session;
    int rc;

    mock_rdma_context_init(&rdma, remote, sizeof(remote));
    disk = rdma_ramdisk_create(&cfg, mock_rdma_ops(), &rdma);
    ASSERT_TRUE(disk != NULL);

    target_session_init(&session, disk, mock_rdma_ops(), &rdma);
    ASSERT_OK(target_session_connect(&session,
                                     &(struct target_connect_info){
                                         .endpoint = "mock-c",
                                     }));

    rc = target_session_submit_io(&session,
                                  &(struct target_io_request){
                                      .direction = TARGET_IO_READ,
                                      .lba = 0,
                                      .block_count = 4,
                                      .remote_addr = 0,
                                      .rkey = 0xCAFE,
                                      .remote_length = 1024,
                                  });
    ASSERT_TRUE(rc == -ERANGE);

    target_session_disconnect(&session);
    rdma_ramdisk_destroy(disk);
    return 0;
}

int main(void)
{
    ASSERT_OK(test_lba_mapping());
    ASSERT_OK(test_target_read_writes_to_remote());
    ASSERT_OK(test_target_write_reads_from_remote());
    ASSERT_OK(test_rejects_short_remote_buffer());

    puts("all tests passed");
    return 0;
}
