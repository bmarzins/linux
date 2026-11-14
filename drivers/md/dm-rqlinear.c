// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2001-2003 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include "dm-rq.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/dax.h>
#include <linux/slab.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "rqlinear"

/*
 * Linear: maps a linear range of a device.
 */
struct linear_c {
	struct dm_dev *dev;
};

/*
 * Construct a linear mapping: <dev_path> <offset>
 */
static int linear_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct linear_c *lc;
	unsigned long long tmp;
	char dummy;
	int ret;

	if (argc != 1) {
		ti->error = "Invalid argument count";
		return -EINVAL;
	}

	lc = kmalloc_obj(*lc);
	if (lc == NULL) {
		ti->error = "Cannot allocate linear context";
		return -ENOMEM;
	}

	ret = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &lc->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->num_secure_erase_bios = 1;
	ti->num_write_zeroes_bios = 1;
	ti->flush_bypasses_map = true;
	ti->private = lc;
	return 0;

bad:
	kfree(lc);
	return ret;
}

static void linear_dtr(struct dm_target *ti)
{
	struct linear_c *lc = ti->private;

	dm_put_device(ti, lc->dev);
	kfree(lc);
}

static sector_t linear_map_sector(struct dm_target *ti, sector_t bi_sector)
{
	struct linear_c *lc = ti->private;

	return dm_target_offset(ti, bi_sector);
}

int linear_map(struct dm_target *ti, struct bio *bio)
{
	struct linear_c *lc = ti->private;

	bio_set_dev(bio, lc->dev->bdev);
	bio->bi_iter.bi_sector = linear_map_sector(ti, bio->bi_iter.bi_sector);

	return DM_MAPIO_REMAPPED;
}

static int linear_clone_and_map_rq(struct dm_target *ti, struct request *rq,
				   union map_info *map_context,
				   struct request **clone_ptr)
{
	struct linear_c *lc = ti->private;
	struct block_device *bdev = lc->dev->bdev;
	struct request_queue *q = bdev_get_queue(bdev);
	struct request *clone;

	clone = blk_mq_alloc_request(q, rq->cmd_flags | REQ_NOMERGE,
				     BLK_MQ_REQ_NOWAIT);
	if (IS_ERR(clone))
		return blk_queue_dying(q) ? DM_MAPIO_KILL : DM_MAPIO_REQUEUE;

	clone->bio = clone->biotail = NULL;
	clone->cmd_flags |= REQ_FAILFAST_TRANSPORT;
	*clone_ptr = clone;

	return DM_MAPIO_REMAPPED;
}

static void linear_release_clone_rq(struct request *clone,
				    union map_info *map_context)
{
	blk_mq_free_request(clone);
}

static void linear_status(struct dm_target *ti, status_type_t type,
			  unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct linear_c *lc = ti->private;

	switch (type) {
	case STATUSTYPE_INFO:
		result[0] = '\0';
		break;

	case STATUSTYPE_TABLE:
		DMEMIT("%s", lc->dev->name);
		break;

	case STATUSTYPE_IMA:
		result[0] = '\0';
		break;
	}
}

static int linear_prepare_ioctl(struct dm_target *ti, struct block_device **bdev,
				unsigned int cmd, unsigned long arg,
				bool *forward)
{
	struct linear_c *lc = ti->private;
	struct dm_dev *dev = lc->dev;

	*bdev = dev->bdev;

	if (ti->len != bdev_nr_sectors(dev->bdev))
		return 1;
	return 0;
}

static int linear_iterate_devices(struct dm_target *ti,
				  iterate_devices_callout_fn fn, void *data)
{
	struct linear_c *lc = ti->private;

	return fn(ti, lc->dev, 0, ti->len, data);
}

static struct target_type rqlinear_target = {
	.name   = "rqlinear",
	.version = {1, 0, 0},
	.features = DM_TARGET_SINGLETON | DM_TARGET_IMMUTABLE |
		    DM_TARGET_PASSES_INTEGRITY | DM_TARGET_ATOMIC_WRITES,
	.module = THIS_MODULE,
	.ctr    = linear_ctr,
	.dtr    = linear_dtr,
	.clone_and_map_rq = linear_clone_and_map_rq,
	.release_clone_rq = linear_release_clone_rq,
	.status = linear_status,
	.prepare_ioctl = linear_prepare_ioctl,
	.iterate_devices = linear_iterate_devices,
};

module_dm(rqlinear);
