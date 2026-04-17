#include "dm-compress.h"

struct algorithm dm_compress_algorithms[] = {
	{ "deflate", NULL, false },
	{ "lzo", NULL, false },
	{ "842", NULL, false },
	{ "lz4", NULL, false },
	{ "lz4hc", NULL, false },
	{ "zstd", NULL, true },
};

const unsigned dm_compress_n_algorithms = ARRAY_SIZE(dm_compress_algorithms);

unsigned dm_compress_max_request_size = 0;

void dm_compress_error(struct dm_compress *c, const char *msg, int err)
{
        if (!cmpxchg(&c->failed, 0, err)) {
                DMERR("%s: %d", msg, err);
		DMERR("Disabling the device");
	}
}

void dm_compress_error_chunk(struct cached_chunk *cc, const char *msg, int err)
{
        if (!cmpxchg(&cc->failed, 0, err)) {
                DMERR("%s: %d", msg, err);
	}
}

static int sync_rw_metadata(struct dm_compress *c, blk_opf_t opf, enum dm_io_mem_type type, sector_t sector, void *ptr, sector_t sectors)
{
	struct dm_io_request io_req;
	struct dm_io_region io_loc;
	int r;

	io_req.bi_opf = opf;
	io_req.mem.type = type;
	io_req.mem.ptr.addr = ptr;
	io_req.notify.fn = NULL;
	io_req.client = c->io;
	io_loc.bdev = c->meta_dev ? c->meta_dev->bdev : c->dev->bdev;
	io_loc.sector = c->start + sector;
	io_loc.count = sectors;

	r = dm_io(&io_req, 1, &io_loc, NULL, IOPRIO_DEFAULT);
	if (unlikely(r))
		return r;

	return 0;
}

void dm_compress_sync(struct dm_compress *c)
{
	struct block_device *bdev = c->meta_dev ? c->meta_dev->bdev : c->dev->bdev;
	__u16 cc;
	unsigned granularity;
	unsigned offset;
	int r;

	r = dm_bufio_write_dirty_buffers(c->bufio);
	if (unlikely(r)) {
		dm_compress_error(c, "Error flushing buffers", r);
		return;
	}

	if (c->meta_dev) {
		r = blkdev_issue_flush(c->dev->bdev);
		if (unlikely(r)) {
			dm_compress_error(c, "Error flushing the data device", r);
			return;
		}
	}

	granularity = __fls(bdev_logical_block_size(bdev));
	if (granularity < 12)
		granularity = 12;

	cc = c->cc_txc;
	offset = (cc * 8) >> granularity;

	r = sync_rw_metadata(c, REQ_OP_WRITE | REQ_SYNC | REQ_FUA, DM_IO_VMA, le64_to_cpu(c->sb->cct) * (BUFFER_SIZE >> SECTOR_SHIFT) + (offset << granularity >> SECTOR_SHIFT), (__u8 *)c->cct + (offset << granularity), 1U << granularity >> SECTOR_SHIFT);
	if (unlikely(r)) {
		dm_compress_error(c, "Error updating crash count table", r);
		return;
	}

	c->cct->entries[c->cc_txc & 0xffff] = cpu_to_le64(le64_to_cpup(&c->cct->entries[c->cc_txc & 0xffff]) + 0x10000);
}

static int initialize_device(struct dm_compress *c)
{
	int r;
	struct superblock *sb = c->sb;
	sector_t position;
	sector_t meta_size;
	sector_t data_blocks;
	sector_t n_bitmap_blocks;
	sector_t n_bitmap_directories;
	sector_t n_chunk_blocks;
	sector_t n_chunk_directories;
	sector_t used_meta_size;
	unsigned i;
	sector_t s, dp;
	struct dm_buffer *bp;
	const unsigned log2_buffer_sectors = __ffs(BUFFER_SIZE >> SECTOR_SHIFT);

	memcpy(sb->magic, SB_MAGIC, 8);
	sb->version = SB_VERSION_1;
	sb->log2_buffer_size = __ffs(BUFFER_SIZE);
	sb->log2_sectors_per_block = c->log2_sectors_per_block;
	sb->log2_sectors_per_chunk = c->log2_sectors_per_chunk;
	position = 1;
	sb->cct = cpu_to_le64(position);
	sb->n_blocks[1] = cpu_to_le64(c->n_blocks);
	sb->n_chunks[1] = cpu_to_le64(c->n_chunks);
	position += dm_div_up(CCT_SIZE, BUFFER_SIZE);

	data_blocks = c->n_blocks;
	n_bitmap_blocks = dm_sector_div_up(data_blocks, BITS_PER_BITMAP);
	n_bitmap_directories = dm_sector_div_up(n_bitmap_blocks, DIRECTORY_ENTRIES);

	n_chunk_blocks = dm_sector_div_up(c->n_chunks, CHUNKS_PER_BUFFER);
	n_chunk_directories = dm_sector_div_up(n_chunk_blocks, DIRECTORY_ENTRIES);

	meta_size = bdev_nr_sectors(c->meta_dev ? c->meta_dev->bdev : c->dev->bdev);
	meta_size = meta_size >= c->start ? meta_size - c->start : 0;
	meta_size >>= log2_buffer_sectors;

	used_meta_size = position + n_bitmap_blocks + n_bitmap_directories + n_chunk_blocks + n_chunk_directories;
	if (used_meta_size > meta_size) {
		if (c->meta_dev)
			c->ti->error = "The metadata device is too small";
		else
			c->ti->error = "The device is too small";
		return -ENOSPC;
	}
	if (used_meta_size > 1ULL << 48) {
		c->ti->error = "The device is too big";
		return -ENOSPC;
	}

	/*printk("pos: %llx\n", position);
	printk("pos: %llx\n", n_bitmap_blocks);
	printk("pos: %llx\n", n_bitmap_directories);
	printk("pos: %llx\n", n_chunk_blocks);
	printk("pos: %llx\n", n_chunk_directories);
	printk("pos: %llx\n", used_meta_size);*/
	sb->bitmap_directory = cpu_to_le64(position + n_bitmap_blocks);
	sb->chunk_directory = cpu_to_le64(position + n_bitmap_blocks + n_bitmap_directories + n_chunk_blocks);

	if (c->log2_sectors_per_block < log2_buffer_sectors)
		used_meta_size <<= log2_buffer_sectors - c->log2_sectors_per_block;
	else if (c->log2_sectors_per_block > log2_buffer_sectors)
		used_meta_size = dm_sector_div_up(used_meta_size, 1U << (c->log2_sectors_per_block - log2_buffer_sectors));

	dp = 0;
	for (s = 0; s < n_bitmap_blocks; s++) {
		struct bitmap *bmp = dm_bufio_new(c->bufio, position + s, &bp);
		if (unlikely(IS_ERR(bmp))) {
			c->ti->error = "Error allocating buffer";
			return PTR_ERR(bmp);
		}
		memset(bmp, 0, BUFFER_SIZE);
		for (i = 0; i < BITS_PER_BITMAP; i++, dp++) {
			if (!c->meta_dev && dp < used_meta_size)
				__set_bit_le(i, bmp->bmp2);
			if (dp >= data_blocks)
				__set_bit_le(i, bmp->bmp2);
		}
		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
	}

	dp = 0;
	for (s = 0; s < n_bitmap_directories; s++) {
		struct directory *dir = dm_bufio_new(c->bufio, position + n_bitmap_blocks + s, &bp);
		if (unlikely(IS_ERR(dir))) {
			c->ti->error = "Error allocating buffer";
			return PTR_ERR(dir);
		}
		memset(dir, 0, BUFFER_SIZE);
		for (i = 0; i < DIRECTORY_ENTRIES; i++, dp++) {
			if (dp < n_bitmap_blocks) {
				dir->ptr[i].lo = cpu_to_le32(position + dp);
				dir->ptr[i].hi = cpu_to_le16((position + dp) >> 31 >> 1);
			}
		}
		if (s + 1 < n_bitmap_directories) {
			dir->next.lo = cpu_to_le32(position + n_bitmap_blocks + s + 1);
			dir->next.hi = cpu_to_le16((position + n_bitmap_blocks + s + 1) >> 31 >> 1);
		}
		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
	}
	position += n_bitmap_blocks + n_bitmap_directories;
	for (s = 0; s < n_chunk_blocks; s++) {
		struct chunk_entries *ch = dm_bufio_new(c->bufio, position + s, &bp);
		if (unlikely(IS_ERR(ch))) {
			c->ti->error = "Error allocating buffer";
			return PTR_ERR(ch);
		}
		memset(ch, 0, BUFFER_SIZE);
		for (i = 0; i < CHUNKS_PER_BUFFER; i++)
			ch->chunk[i].ptr[1].algorithm = ALGORITHM_ZERO;
		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
	}
	dp = 0;
	for (s = 0; s < n_chunk_directories; s++) {
		struct directory *dir = dm_bufio_new(c->bufio, position + n_chunk_blocks + s, &bp);
		if (unlikely(IS_ERR(dir))) {
			c->ti->error = "Error allocating buffer";
			return PTR_ERR(dir);
		}
		memset(dir, 0, BUFFER_SIZE);
		for (i = 0; i < DIRECTORY_ENTRIES; i++, dp++) {
			if (dp < n_chunk_blocks) {
				dir->ptr[i].lo = cpu_to_le32(position + dp);
				dir->ptr[i].hi = cpu_to_le16((position + dp) >> 31 >> 1);
			}
		}
		if (s + 1 < n_chunk_directories) {
			dir->next.lo = cpu_to_le32(position + n_chunk_blocks + s + 1);
			dir->next.hi = cpu_to_le16((position + n_chunk_blocks + s + 1) >> 31 >> 1);
		}
		dm_bufio_mark_buffer_dirty(bp);
		dm_bufio_release(bp);
	}

	r = dm_bufio_write_dirty_buffers(c->bufio);
	if (unlikely(r)) {
		c->ti->error = "Error writing buffers";
		return r;
	}

	for (i = 0; i < CCT_ENTRIES; i++)
		c->cct->entries[i] = cpu_to_le64(0xffff);

	r = sync_rw_metadata(c, REQ_OP_WRITE, DM_IO_VMA, le64_to_cpu(sb->cct) * (BUFFER_SIZE >> SECTOR_SHIFT), c->cct, CCT_SIZE >> SECTOR_SHIFT);
	if (unlikely(r)) {
		c->ti->error = "Error writing crash count table";
		return r;
	}

	r = sync_rw_metadata(c, REQ_OP_WRITE | REQ_PREFLUSH, DM_IO_KMEM, 0, c->sb, SB_SECTORS);
	if (unlikely(r)) {
		c->ti->error = "Error writing superblock";
		return r;
	}

	return 0;
}

static int verify_device(struct dm_compress *c)
{
	struct superblock *sb = c->sb;

	if (memcmp(sb->magic, SB_MAGIC, 8)) {
		c->ti->error = "The device is not initialized";
		return -EINVAL;
	}
	if (sb->version != SB_VERSION_1) {
		c->ti->error = "Version mismatch";
		return -EINVAL;
	}
	if (sb->log2_buffer_size != __ffs(BUFFER_SIZE)) {
		c->ti->error = "Buffer size mismatch";
		return -EINVAL;
	}
	if (sb->log2_sectors_per_block != c->log2_sectors_per_block) {
		c->ti->error = "Block size mismatch";
		return -EINVAL;
	}
	if (sb->log2_sectors_per_chunk != c->log2_sectors_per_chunk) {
		c->ti->error = "Chunk size mismatch";
		return -EINVAL;
	}

	return 0;
}

static void dm_compress_dtr(struct dm_target *ti);

/*
 * Arguments:
 *	underlying device
 *	offset from the start of the device
 *	block size
 *	chunk size
 *	number chunks
 *	algorithm
 *	number of optional arguments
 */

static int dm_compress_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct dm_compress *c;
	int r;
	char dummy;
	unsigned long long param;
	unsigned i;
	unsigned extra_args;
	size_t dir_ptr;
	unsigned dir_offset, offset;
	struct dm_arg_set as;
	static const struct dm_arg _args[] = {
		{0, 1, "Invalid number of feature args"},
	};

	c = kzalloc(sizeof(struct dm_compress), GFP_KERNEL);
	if (!c) {
		ti->error = "Cannot allocate compress structure";
		r = -ENOMEM;
		goto bad;
	}
	ti->private = c;
	c->ti = ti;
	ti->per_io_data_size = sizeof(struct dm_compress_io);
	ti->num_flush_bios = 1;
	ti->flush_supported = true;

	INIT_LIST_HEAD(&c->free_chunks);
	INIT_LIST_HEAD(&c->lru);
	c->chunks = RB_ROOT;
	mutex_init(&c->mutex);
	INIT_LIST_HEAD(&c->waiting_bio_lru);
	c->waiting_bio_tree = RB_ROOT;
	c->n_cached_chunks = N_CACHED_CHUNKS;

#define DIRECT_ARGUMENTS 6

	if (argc <= DIRECT_ARGUMENTS) {
		ti->error = "Invalid argument count";
		r = -EINVAL;
		goto bad;
	}

	r = dm_get_device(ti, argv[0], dm_table_get_mode(ti->table), &c->dev);
	if (r) {
		ti->error = "Device lookup failed";
		goto bad;
	}

	if (sscanf(argv[1], "%llu%c", &param, &dummy) != 1 || param != (sector_t)param) {
		ti->error = "Invalid starting offset";
		r = -EINVAL;
		goto bad;
	}
	c->start = param;

	if (sscanf(argv[2], "%llu%c", &param, &dummy) != 1 || param < 512 || param > MAX_BLOCK_SIZE || (param & (param - 1))) {
		ti->error = "Invalid block size";
		r = -EINVAL;
		goto bad;
	}
	c->log2_sectors_per_block = __ffs(param >> 9);

	if (sscanf(argv[3], "%llu%c", &param, &dummy) != 1 || param < 512 || param > MAX_CHUNK_SIZE || (param & (param - 1))) {
		ti->error = "Invalid chunk size";
		r = -EINVAL;
		goto bad;
	}
	c->log2_sectors_per_chunk = __ffs(param >> 9);

	if (c->log2_sectors_per_block >= c->log2_sectors_per_chunk) {
		ti->error = "Block size is not less than chunk size";
		r = -EINVAL;
		goto bad;
	}
	if (c->log2_sectors_per_block + 8 < c->log2_sectors_per_chunk) {
		ti->error = "Block size must be greater or equal than chunk size divied by 256";
		r = -EINVAL;
		goto bad;
	}

	if (sscanf(argv[4], "%llu%c", &param, &dummy) != 1 || !param || param != (sector_t)param) {
		ti->error = "Invalid number of chunks";
		r = -EINVAL;
		goto bad;
	}
	c->n_chunks = param;

	for (i = 0; i < dm_compress_n_algorithms; i++) {
		if (!strcmp(dm_compress_algorithms[i].name, argv[5])) {
			c->algorithm = i;
			goto found_alg;
		}
	}
	ti->error = "Invalid compression algorithm";
	r = -EINVAL;
	goto bad;
found_alg:

	as.argc = argc - DIRECT_ARGUMENTS;
	as.argv = argv + DIRECT_ARGUMENTS;
	r = dm_read_arg_group(_args, &as, &extra_args, &ti->error);
	if (r)
		goto bad;
	while (extra_args--) {
		const char *opt_string = dm_shift_arg(&as);
		if (!opt_string) {
			ti->error = "Not enough feature arguments";
			r = -EINVAL;
			goto bad;
		}
		if (!strncmp(opt_string, "meta_device:", strlen("meta_device:"))) {
			if (c->meta_dev) {
				dm_put_device(ti, c->meta_dev);
				c->meta_dev = NULL;
			}
			r = dm_get_device(ti, strchr(opt_string, ':') + 1, dm_table_get_mode(ti->table), &c->meta_dev);
			if (r) {
				ti->error = "Device lookup failed";
				goto bad;
			}
		} else if (sscanf(opt_string, "n_cached_chunks:%llu%c", &param, &dummy) == 1 && param >= 1 && param < 1048576) {
			c->n_cached_chunks = param;
		} else {
			ti->error = "Invalid argument";
			r = -EINVAL;
			goto bad;
		}
	}

	c->wq = alloc_workqueue("dm-compress", WQ_PERCPU | WQ_MEM_RECLAIM, METADATA_WORKQUEUE_MAX_ACTIVE);
	if (unlikely(!c->wq)) {
		ti->error = "Cannot allocate workqueue";
		r = -ENOMEM;
		goto bad;
	}

	c->compress_wq = alloc_workqueue("dm-compress-compress", WQ_PERCPU | WQ_MEM_RECLAIM, METADATA_WORKQUEUE_MAX_ACTIVE);
	if (unlikely(!c->compress_wq)) {
		ti->error = "Cannot allocate workqueue";
		r = -ENOMEM;
		goto bad;
	}

	c->flush_wq = alloc_workqueue("dm-compress-flush", WQ_PERCPU | WQ_MEM_RECLAIM, 1);
	if (unlikely(!c->flush_wq)) {
		ti->error = "Cannot allocate workqueue";
		r = -ENOMEM;
		goto bad;
	}

	c->bufio = dm_bufio_client_create(c->meta_dev ? c->meta_dev->bdev : c->dev->bdev, BUFFER_SIZE, 1, 0, NULL, NULL, 0);
	if (IS_ERR(c->bufio)) {
		ti->error = "Cannot initialize dm-bufio";
		r = PTR_ERR(c->bufio);
		c->bufio = NULL;
		goto bad;
	}
	dm_bufio_set_sector_offset(c->bufio, c->start);

	c->io = dm_io_client_create();
	if (IS_ERR(c->io)) {
		ti->error = "Cannot allocate dm io";
		r = PTR_ERR(c->io);
		c->io = NULL;
		goto bad;
	}

	c->sb = kmalloc(SB_SECTORS << SECTOR_SHIFT, GFP_KERNEL);
	if (!c->sb) {
		ti->error = "Cannot allocate superblock";
		r = -ENOMEM;
		goto bad;
	}

	c->cct = vmalloc(sizeof(struct crash_count_table));
	if (!c->cct) {
		ti->error = "Cannot allocate crash count table";
		r = -ENOMEM;
		goto bad;
	}

	c->n_blocks = bdev_nr_sectors(c->dev->bdev);
	if (!c->meta_dev) {
		if (c->n_blocks <= c->start) {
			ti->error = "The device is too small";
			r = -ENOSPC;
			goto bad;
		}
		c->n_blocks -= c->start;
	}
	c->n_blocks >>= c->log2_sectors_per_block;


	for (i = 0; i < c->n_cached_chunks; i++) {
		struct cached_chunk *cc = dm_compress_alloc_cached_chunk(c);
		if (!cc) {
			ti->error = "Error allocating cached chunk";
			r = -ENOMEM;
			goto bad;
		}
		list_add(&cc->lru_entry, &c->free_chunks);
	}


	r = sync_rw_metadata(c, REQ_OP_READ, DM_IO_KMEM, 0, c->sb, SB_SECTORS);
	if (r) {
		ti->error = "Error reading superblock";
		goto bad;
	}

	if (mem_is_zero(c->sb, SB_SECTORS << SECTOR_SHIFT)) {
		r = initialize_device(c);
		if (r)
			goto bad;
	}

	r = verify_device(c);
	if (r)
		goto bad;

	block_location(c, c->n_blocks - 1, &c->n_groups, &dir_ptr, &dir_offset, &offset);
	c->n_groups++;
	dir_ptr++;
	c->block_directories = vmalloc_array(dir_ptr, sizeof(sector_t));
	c->groups = vmalloc_array(c->n_groups, sizeof(struct block_group_descriptor));
	for (i = 0; i < c->n_groups; i++) {
		mutex_init(&c->groups[i].mutex);
		c->groups[i].free = -1;
	}

	chunk_location(c, c->n_chunks - 1, &dir_ptr, &dir_offset, &offset);
	dir_ptr++;
	c->chunk_directories = vmalloc_array(dir_ptr, sizeof(sector_t));

	r = dm_set_target_max_io_len(ti, (sector_t)1 << c->log2_sectors_per_chunk);
	if (r)
		goto bad;
	ti->discards_supported = true;
	ti->num_discard_bios = 1;

	return 0;

bad:
	dm_compress_dtr(ti);
	return r;
}

static void dm_compress_dtr(struct dm_target *ti)
{
	struct dm_compress *c = ti->private;

	if (!c)
		return;

	if (c->wq)
		destroy_workqueue(c->wq);
	if (c->compress_wq)
		destroy_workqueue(c->compress_wq);
	if (c->flush_wq)
		destroy_workqueue(c->flush_wq);
	if (c->bufio)
		dm_bufio_client_destroy(c->bufio);
	if (c->io)
		dm_io_client_destroy(c->io);

	if (c->dev)
		dm_put_device(ti, c->dev);
	if (c->meta_dev)
		dm_put_device(ti, c->meta_dev);

	while (!list_empty(&c->free_chunks)) {
		struct cached_chunk *cc = list_entry(c->free_chunks.next, struct cached_chunk, lru_entry);
		list_del(&cc->lru_entry);
		dm_compress_free_cached_chunk(cc);
	}
	while (!RB_EMPTY_ROOT(&c->chunks)) {
		struct cached_chunk *cc = rb_entry(rb_first(&c->chunks), struct cached_chunk, node);
		list_del(&cc->lru_entry);
		rb_erase(&cc->node, &c->chunks);
		dm_compress_free_cached_chunk(cc);
	}

	kfree(c->sb);
	vfree(c->cct);
	vfree(c->block_directories);
	vfree(c->chunk_directories);
	if (c->groups) {
		size_t i;
		for (i = 0; i < c->n_groups; i++)
			mutex_destroy(&c->groups[i].mutex);
		vfree(c->groups);
	}

	mutex_destroy(&c->mutex);

	kfree(c);
}


static int load_directories(struct dm_compress *c, sector_t block, sector_t *directory, size_t n)
{
	size_t i = 0;
read_next:
	directory[i++] = block;
	if (i < n) {
		struct dm_buffer *bp;
		struct directory *dir = dm_bufio_read(c->bufio, block, &bp);
		if (IS_ERR(dir))
			return PTR_ERR(dir);
		block = READ_PTR(&dir->next);
		dm_bufio_release(bp);
		goto read_next;
	}
	return 0;
}

static int dm_compress_preresume(struct dm_target *ti)
{
	struct dm_compress *c = ti->private;
	struct superblock *sb = c->sb;
	bool b;
	size_t group_ptr, dir_ptr;
	unsigned dir_offset, offset;
	int r;

	r = sync_rw_metadata(c, REQ_OP_READ, DM_IO_KMEM, 0, c->sb, SB_SECTORS);
	if (r) {
		dm_compress_error(c, "Error reading superblock", r);
		return r;
	}

	r = sync_rw_metadata(c, REQ_OP_READ, DM_IO_VMA, le64_to_cpu(sb->cct) * (BUFFER_SIZE >> SECTOR_SHIFT), c->cct, CCT_SIZE >> SECTOR_SHIFT);
	if (r) {
		dm_compress_error(c, "Error reading crash count table", r);
		return r;
	}

	b = CC_VALID(c, &sb->n_cc_txc);
	if (sb->n_blocks[b] != c->n_blocks) {
		DMERR("Resizing is not yet supported");
		return -EOPNOTSUPP;
	}
	if (sb->n_chunks[b] != c->n_chunks) {
		DMERR("Resizing is not yet supported");
		return -EOPNOTSUPP;
	}

	block_location(c, c->n_blocks - 1, &group_ptr, &dir_ptr, &dir_offset, &offset);
	dir_ptr++;
	r = load_directories(c, le64_to_cpu(c->sb->bitmap_directory), c->block_directories, dir_ptr);
	if (r) {
		dm_compress_error(c, "Unable to read bitmap directories", r);
		return r;
	}

	chunk_location(c, c->n_chunks - 1, &dir_ptr, &dir_offset, &offset);
	dir_ptr++;
	r = load_directories(c, le64_to_cpu(c->sb->chunk_directory), c->chunk_directories, dir_ptr);
	if (r) {
		dm_compress_error(c, "Unable to read chunk directories", r);
		return r;
	}

	r = dm_compress_get_free_space(c);
	if (r)
		return r;

	return 0;
}

static void dm_compress_resume(struct dm_target *ti)
{
	struct dm_compress *c = ti->private;
	int r;

	c->sb->cc = cpu_to_le16(le16_to_cpup(&c->sb->cc) + 1);
	c->cc_txc += 0x10000;
	c->cct->entries[c->cc_txc & 0xffff] = cpu_to_le64(le64_to_cpup(&c->cct->entries[c->cc_txc & 0xffff]) + 0x10000);

	r = sync_rw_metadata(c, REQ_OP_WRITE | REQ_PREFLUSH, DM_IO_KMEM, 0, c->sb, SB_SECTORS);
	if (unlikely(r)) {
		dm_compress_error(c, "Error writing superblock", r);
		return;
	}
}

static void dm_compress_postsuspend(struct dm_target *ti)
{
	struct dm_compress *c = ti->private;
	int r;

check_again:
	mutex_lock(&c->mutex);
	if (dm_compress_is_something_in_progress(c)) {
		mutex_unlock(&c->mutex);
		schedule_timeout_uninterruptible(1);
		goto check_again;
	}
	dm_compress_sync(c);
	mutex_unlock(&c->mutex);

	c->cct->entries[c->cc_txc & 0xffff] = cpu_to_le64(le64_to_cpup(&c->cct->entries[c->cc_txc & 0xffff]) - 0x10000);
	c->sb->cc = cpu_to_le16(le16_to_cpup(&c->sb->cc) - 1);
	c->cc_txc -= 0x10000;

	r = sync_rw_metadata(c, REQ_OP_WRITE | REQ_PREFLUSH, DM_IO_KMEM, 0, c->sb, SB_SECTORS);
	if (unlikely(r)) {
		dm_compress_error(c, "Error writing superblock", r);
		return;
	}
}

static void dm_compress_status(struct dm_target *ti, status_type_t type,
			       unsigned int status_flags, char *result, unsigned int maxlen)
{
	struct dm_compress *c = ti->private;
	size_t sz = 0;

	switch (type) {
	case STATUSTYPE_INFO: {
		DMEMIT("%llu %llu",
			(unsigned long long)c->n_blocks,
			(unsigned long long)atomic64_read(&c->free_blocks));
		break;
	}

	case STATUSTYPE_TABLE: {
		unsigned arg_count;
		DMEMIT("%s", c->dev->name);
		DMEMIT(" %llu", (unsigned long long)c->start);
		DMEMIT(" %u", 512U << c->log2_sectors_per_block);
		DMEMIT(" %u", 512U << c->log2_sectors_per_chunk);
		DMEMIT(" %llu", (unsigned long long)c->n_chunks);
		DMEMIT(" %s", dm_compress_algorithms[c->algorithm].name);
		arg_count = 0;
		arg_count += !!c->meta_dev;
		arg_count += c->n_cached_chunks != N_CACHED_CHUNKS;
		DMEMIT(" %u", arg_count);
		if (c->meta_dev)
			DMEMIT(" meta_device:%s", c->meta_dev->name);
		if (c->n_cached_chunks != N_CACHED_CHUNKS)
			DMEMIT(" n_cached_chunks:%u", c->n_cached_chunks);
		break;
	}

	case STATUSTYPE_IMA:
		break;
	}
}

static void dm_compress_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct dm_compress *c = ti->private;
	limits->io_min = 512U << c->log2_sectors_per_chunk;
	limits->io_opt = 512U << c->log2_sectors_per_chunk;
	limits->discard_granularity = 512U << c->log2_sectors_per_chunk;
	limits->max_hw_discard_sectors = limits->discard_granularity >> SECTOR_SHIFT;
}

static struct target_type compress_target = {
	.name			= "compress",
	.version		= {1, 0, 0},
	.module			= THIS_MODULE,
	.features		= 0,
	.ctr			= dm_compress_ctr,
	.dtr			= dm_compress_dtr,
	.map			= dm_compress_map,
	.preresume		= dm_compress_preresume,
	.resume			= dm_compress_resume,
	.postsuspend		= dm_compress_postsuspend,
	.status			= dm_compress_status,
	.io_hints		= dm_compress_io_hints,
};

static int __init dm_compress_init(void)
{
	int r;
	unsigned i;

	for (i = 0; i < dm_compress_n_algorithms; i++) {
		unsigned request_size;
		dm_compress_algorithms[i].cc = crypto_alloc_acomp(dm_compress_algorithms[i].name, 0, 0);
		if (IS_ERR(dm_compress_algorithms[i].cc)) {
			r = PTR_ERR(dm_compress_algorithms[i].cc);
			dm_compress_algorithms[i].cc = NULL;
			DMERR("Could not initialize algorithm %s", dm_compress_algorithms[i].name);
			goto bad;
		}
		request_size = ALIGN(sizeof(struct acomp_req) + crypto_acomp_reqsize(dm_compress_algorithms[i].cc), CRYPTO_MINALIGN);
		dm_compress_max_request_size = max(dm_compress_max_request_size, request_size);
	}

	r = dm_register_target(&compress_target);
	if (r < 0)
		goto bad;

	return 0;

bad:
	for (i = 0; i < dm_compress_n_algorithms; i++) {
		if (dm_compress_algorithms[i].cc)
			crypto_free_acomp(dm_compress_algorithms[i].cc);
	}
	return r;
}

static void __exit dm_compress_exit(void)
{
	unsigned i;

	dm_unregister_target(&compress_target);

	for (i = 0; i < dm_compress_n_algorithms; i++)
		crypto_free_acomp(dm_compress_algorithms[i].cc);
}

module_init(dm_compress_init);
module_exit(dm_compress_exit);

MODULE_AUTHOR("Mikulas Patocka");
MODULE_DESCRIPTION(DM_NAME " target for transparent data compression");
MODULE_LICENSE("GPL");
