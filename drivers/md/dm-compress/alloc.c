#include "dm-compress.h"

static bool prepare_bitmap(struct dm_compress *c, sector_t bmp_block, __u64 **working_copy, __u64 **stable_copy, struct dm_buffer **bp)
{
	struct bitmap *bmp;
	bool idx;

	bmp = dm_bufio_read(c->bufio, bmp_block, bp);
	if (IS_ERR(bmp)) {
		dm_compress_error(c, "Error reading bitmap", PTR_ERR(bmp));
		return false;
	}

	idx = !CC_VALID(c, &bmp->cc_txc);
	if (unlikely(!CC_CURRENT(c, &bmp->cc_txc))) {
		CC_SET_CURRENT(c, &bmp->cc_txc);
		//WARN_ON(idx != CC_VALID(c, &bmp->cc_txc));
		if (!idx)
			memcpy(bmp->bmp1, bmp->bmp2, BITS_PER_BITMAP / 8);
		else
			memcpy(bmp->bmp2, bmp->bmp1, BITS_PER_BITMAP / 8);
		idx = !idx;
		dm_bufio_mark_buffer_dirty(*bp);
	}
	if (idx)
		*working_copy = bmp->bmp1, *stable_copy = bmp->bmp2;
	else
		*working_copy = bmp->bmp2, *stable_copy = bmp->bmp1;

	return true;
}

sector_t dm_compress_allocate_space(struct dm_compress *c, sector_t preferred_block, unsigned n_blocks)
{
	struct dm_buffer *bp;
	size_t group_ptr, dir_ptr;
	unsigned dir_offset, bmp_offset;
	sector_t dir_block, bmp_block;
	struct block_group_descriptor *group;
	struct directory *dir;
	__u64 *working_copy, *stable_copy;
	unsigned bit;
	size_t n_retries = 0;

try_next_bitmap:
	if (unlikely(preferred_block >= c->n_blocks))
		preferred_block = 0;

	block_location(c, preferred_block, &group_ptr, &dir_ptr, &dir_offset, &bmp_offset);
	group = &c->groups[group_ptr];
	dir_block = c->block_directories[dir_ptr];
	preferred_block -= bmp_offset;

	dir = dm_bufio_read(c->bufio, dir_block, &bp);
	if (IS_ERR(dir)) {
		dm_compress_error(c, "Error reading bitmap directory", PTR_ERR(dir));
		return (sector_t)-1;
	}
	bmp_block = READ_PTR(&dir->ptr[dir_offset]);
	dm_bufio_release(bp);

	mutex_lock(&group->mutex);
	if (unlikely(!prepare_bitmap(c, bmp_block, &working_copy, &stable_copy, &bp))) {
		mutex_unlock(&group->mutex);
		return (sector_t)-1;
	}

find_again:
	bit = find_next_zero_bit_le(working_copy, BITS_PER_BITMAP, bmp_offset);
	if (bit <= BITS_PER_BITMAP - n_blocks) {
		unsigned i;
		for (i = n_blocks; i--; ) {
			if (test_bit_le(bit + i, working_copy) || test_bit_le(bit + i, stable_copy)) {
				bmp_offset = bit + i + 1;
				goto find_again;
			}
		}
		for (i = 0; i < n_blocks; i++) {
			__set_bit_le(bit + i, working_copy);
		}
		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
		mutex_unlock(&group->mutex);

		atomic64_sub(n_blocks, &c->free_blocks);

		return preferred_block + bit;
	}

	dm_bufio_release(bp);
	mutex_unlock(&group->mutex);

	if (++n_retries <= c->n_groups) {
		preferred_block += BITS_PER_BITMAP;
		goto try_next_bitmap;
	}

	dm_compress_error(c, "The device is out of space", -ENOSPC);
	return (sector_t)-1;
}

void dm_compress_free_space(struct dm_compress *c, sector_t block, unsigned n_blocks)
{
	struct dm_buffer *bp;
	size_t group_ptr, dir_ptr;
	unsigned dir_offset, bmp_offset;
	sector_t dir_block, bmp_block;
	struct block_group_descriptor *group;
	struct directory *dir;
	__u64 *working_copy, *stable_copy;

	atomic64_add(n_blocks, &c->free_blocks);

	while (n_blocks) {
		block_location(c, block, &group_ptr, &dir_ptr, &dir_offset, &bmp_offset);
		group = &c->groups[group_ptr];
		dir_block = c->block_directories[dir_ptr];

		dir = dm_bufio_read(c->bufio, dir_block, &bp);
		if (IS_ERR(dir)) {
			dm_compress_error(c, "Error reading bitmap directory", PTR_ERR(dir));
			return;
		}
		bmp_block = READ_PTR(&dir->ptr[dir_offset]);
		dm_bufio_release(bp);

		mutex_lock(&group->mutex);
		if (unlikely(!prepare_bitmap(c, bmp_block, &working_copy, &stable_copy, &bp))) {
			mutex_unlock(&group->mutex);
			return;
		}
		//printk("free bmp block: %llx\n", bmp_block);
		while (n_blocks && bmp_offset < BITS_PER_BITMAP) {
			if (!__test_and_clear_bit_le(bmp_offset, working_copy)) {
				dm_compress_error(c, "Bitmap bit is already cleared", -EUCLEAN);
				//printk("chunk %llx %x %x\n", block, n_blocks, bmp_offset);
				dm_bufio_mark_buffer_dirty(bp);
				dm_bufio_release(bp);
				mutex_unlock(&group->mutex);
				return;
			}
			bmp_offset++;
			block++;
			n_blocks--;
		}

		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
		mutex_unlock(&group->mutex);
	}
}

int dm_compress_get_free_space(struct dm_compress *c)
{
	sector_t block = 0;
	sector_t free_blocks = 0;
	while (block < c->n_blocks) {
		size_t group_ptr, dir_ptr;
		unsigned dir_offset, bmp_offset;
		sector_t dir_block, bmp_block;
		struct dm_buffer *bp;
		struct directory *dir;
		struct bitmap *bmp;
		bool idx;
		__u64 *bmp_data;

		block_location(c, block, &group_ptr, &dir_ptr, &dir_offset, &bmp_offset);
		dir_block = c->block_directories[dir_ptr];

		dir = dm_bufio_read(c->bufio, dir_block, &bp);
		if (IS_ERR(dir)) {
			dm_compress_error(c, "Error reading bitmap directory", PTR_ERR(dir));
			return PTR_ERR(dir);
		}
		bmp_block = READ_PTR(&dir->ptr[dir_offset]);
		dm_bufio_release(bp);

		bmp = dm_bufio_read(c->bufio, bmp_block, &bp);
		if (IS_ERR(bmp)) {
			dm_compress_error(c, "Error reading bitmap", PTR_ERR(bmp));
			return PTR_ERR(bmp);
		}
		idx = !CC_VALID(c, &bmp->cc_txc);
		if (idx)
			bmp_data = bmp->bmp1;
		else
			bmp_data = bmp->bmp2;
		free_blocks += BITS_PER_BITMAP - bitmap_weight((void *)bmp_data, BITS_PER_BITMAP);
		dm_bufio_release(bp);

		block += BITS_PER_BITMAP;
	}

	atomic64_set(&c->free_blocks, free_blocks);

	return 0;
}

struct chunk_ptr *dm_compress_prepare_chunk(struct dm_compress *c, sector_t chunk, struct dm_buffer **bp)
{
	struct dm_buffer *dbp;
	size_t dir_ptr;
	unsigned dir_offset, chunk_offset;
	sector_t dir_block, chunk_block;
	struct directory *dir;
	struct chunk_entries *chunks;
	struct chunk_entry *ce;
	bool idx;

	chunk_location(c, chunk, &dir_ptr, &dir_offset, &chunk_offset);

	dir_block = c->chunk_directories[dir_ptr];

	dir = dm_bufio_read(c->bufio, dir_block, &dbp);
	if (IS_ERR(dir)) {
		dm_compress_error(c, "Error reading chunk directory", PTR_ERR(dir));
		return NULL;
	}
	chunk_block = READ_PTR(&dir->ptr[dir_offset]);
	dm_bufio_release(dbp);

	chunks = dm_bufio_read(c->bufio, chunk_block, bp);
	if (IS_ERR(chunks)) {
		dm_compress_error(c, "Error reading chunk block", PTR_ERR(chunks));
		return NULL;
	}
	ce = &chunks->chunk[chunk_offset];

	idx = !CC_VALID(c, &ce->cc_txc);
	if (unlikely(!CC_CURRENT(c, &ce->cc_txc))) {
		CC_SET_CURRENT(c, &ce->cc_txc);
		WARN_ON(idx != CC_VALID(c, &ce->cc_txc));
		ce->ptr[idx] = ce->ptr[!idx];
		idx = !idx;
		dm_bufio_mark_buffer_dirty(*bp);
	}
	return &ce->ptr[!idx];
}
