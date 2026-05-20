#ifndef _LINUX_URMA_BLKDEV_H
#define _LINUX_URMA_BLKDEV_H

#include <linux/blkdev.h>
#include <linux/types.h>
#include <ub/urma/ubcore_types.h>

struct ubcore_jetty;

enum urma_blkdev_io_dir {
	URMA_BLKDEV_IO_READ = 1,  /* NVMe read: ramdisk -> HBM */
	URMA_BLKDEV_IO_WRITE = 2, /* NVMe write: HBM -> ramdisk */
};

struct urma_blkdev_peer {
	union ubcore_eid eid;
	struct ubcore_jetty *jetty;
};

struct urma_blkdev_io {
	enum urma_blkdev_io_dir dir;
	u64 lba;
	u32 block_count;
	u64 hbm_addr;
	u32 hbm_token_id;
};

typedef void (*urma_blkdev_done_fn)(void *priv, int status);

int urma_blkdev_submit_nvmet_io(struct block_device *bdev,
				const struct urma_blkdev_peer *peer,
				const struct urma_blkdev_io *io,
				urma_blkdev_done_fn done,
				void *priv);

#endif
