#include "dm-compress.h"

static bool dm_compress_process_chunk(struct dm_compress_io *dio, bool x);

static void dm_compress_chunk_work(struct work_struct *w);
static void dm_compress_write_endio(struct bio *bio);
static void dm_compress_chunk_work_2(struct work_struct *w);

static void dm_compress_read_and_decompress(struct cached_chunk *cc);
static void dm_compress_read_endio(struct bio *bio);
static void dm_compress_chunk_decompress(struct work_struct *w);

static void dm_compress_process_bios_on_chunk(struct dm_compress *c, sector_t chunk);

static struct chunk_entry *map_chunk(struct dm_compress *c, sector_t chunk, struct dm_buffer **bp)
{
	size_t dir_ptr;
	unsigned dir_offset, chunk_offset;
	struct directory *dir;
	sector_t ptr;
	struct chunk_entry *chunks;

	chunk_location(c, chunk, &dir_ptr, &dir_offset, &chunk_offset);

	dir = dm_bufio_read(c->bufio, c->chunk_directories[dir_ptr], bp);
	if (unlikely(IS_ERR(dir)))
		return (void *)dir;
	ptr = READ_PTR(&dir->ptr[dir_offset]);
	dm_bufio_release(*bp);

	chunks = dm_bufio_read(c->bufio, ptr, bp);
	if (unlikely(IS_ERR(chunks)))
		return chunks;

	return &chunks[chunk_offset];
}

void dm_compress_enqueue_chunk(struct cached_chunk *cc, unsigned intent)
{
	unsigned old_intent = cc->intent;
	cc->intent = max(cc->intent, intent);
	if (old_intent == INTENT_IDLE) {
		WARN_ON(work_pending(&cc->work));	// !!! FIXME
		INIT_WORK(&cc->work, dm_compress_chunk_work);
		queue_work(cc->c->compress_wq, &cc->work);
	}
}

int dm_compress_map(struct dm_target *ti, struct bio *bio)
{
	struct dm_compress *c = ti->private;
	struct dm_compress_io *dio = dm_per_bio_data(bio, sizeof(struct dm_compress_io));
	sector_t logical_sector;
	dio->c = c;

	if (unlikely(c->failed))
		return DM_MAPIO_KILL;

	if (unlikely(bio->bi_opf & REQ_PREFLUSH)) {
		dio->chunk = 0;
		dio->offset_in_chunk = 0;
	} else {
		logical_sector = dm_target_offset(ti, bio->bi_iter.bi_sector);
		dio->chunk = logical_sector >> c->log2_sectors_per_chunk;
		dio->offset_in_chunk = logical_sector & ((1U << c->log2_sectors_per_chunk) - 1);

		if (dio->chunk >= c->n_chunks)
			return DM_MAPIO_KILL;
	}

	if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) {
		if (unlikely(WARN_ON(dio->offset_in_chunk + bio_sectors(bio) > 1U << c->log2_sectors_per_chunk))) {
			return DM_MAPIO_KILL;
		}
		if (unlikely(bio_sectors(bio) != 1U << c->log2_sectors_per_chunk)) {
			bio_endio(bio);
			return DM_MAPIO_SUBMITTED;
		}
	}

	INIT_WORK(&dio->work, dm_compress_work);
	queue_work(c->wq, &dio->work);
	return DM_MAPIO_SUBMITTED;
}

void dm_compress_work(struct work_struct *w)
{
	struct dm_compress_io *dio = container_of(w, struct dm_compress_io, work);
	struct dm_compress *c = dio->c;
	struct bio *bio = dm_bio_from_per_bio_data(dio, sizeof(struct dm_compress_io));
	bool ret;

	mutex_lock(&c->mutex);
	ret = dm_compress_process_chunk(dio, false);
	if (!ret)
		dm_compress_wait_for_cached_chunk(dio);
	mutex_unlock(&c->mutex);

	if (ret)
		bio_endio(bio);
}

static bool dm_compress_process_chunk(struct dm_compress_io *dio, bool x)
{
	struct dm_compress *c = dio->c;
	struct bio *bio = dm_bio_from_per_bio_data(dio, sizeof(struct dm_compress_io));
	struct cached_chunk *cc;
	struct chunk_entry *ce;
	struct dm_buffer *bp;
	struct chunk_ptr *cp;

	if (unlikely(bio->bi_opf & REQ_PREFLUSH)) {
		if (dm_compress_is_something_in_progress(c))
			return false;
		dm_compress_sync(c);
		return true;
	}

	if (bio_op(bio) == REQ_OP_WRITE || bio_op(bio) == REQ_OP_DISCARD) {
		struct bio *oldest_bio;
		struct dm_compress_io *oldest_dio = dm_compress_pick_oldest_dio(c);
		if (oldest_dio) {
			oldest_bio = dm_bio_from_per_bio_data(oldest_dio, sizeof(struct dm_compress_io));
			if (unlikely(oldest_bio->bi_opf & REQ_PREFLUSH))
				return false;
		}
	}

	cc = dm_compress_find_cached_chunk(c, dio->chunk, NULL);
	if (cc) {
		char *data_ptr;
		unsigned offset;
		struct bvec_iter iter;
		struct bio_vec bv;
have_cached_chunk:
		if (unlikely(cc->failed)) {
			if (bio_op(bio) == REQ_OP_WRITE || bio_op(bio) == REQ_OP_DISCARD)
				dm_compress_error(c, "Aborting write to failed chunk", cc->failed);
			bio->bi_status = errno_to_blk_status(cc->failed);
			return true;
		}
		if ((bio_op(bio) == REQ_OP_WRITE || bio_op(bio) == REQ_OP_DISCARD) && cc->intent > INTENT_IDLE)
			return false;

		if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) {
			memset(cc->uncompressed_data, 0, 512U << c->log2_sectors_per_chunk);
			memset(cc->sector_tags, TAG_VALID, 1U << c->log2_sectors_per_chunk);
			cc->dirty = true;
			dm_compress_enqueue_chunk(cc, INTENT_FREE);
			return true;
		}

		data_ptr = cc->uncompressed_data + (dio->offset_in_chunk << SECTOR_SHIFT);
		offset = dio->offset_in_chunk;
		bio_for_each_segment(bv, bio, iter) {
			char *mem = bvec_kmap_local(&bv);
			if (bio_op(bio) == REQ_OP_READ) {
				if (memchr(cc->sector_tags + offset, 0, bv.bv_len >> SECTOR_SHIFT)) {
					kunmap_local(mem);
					dm_compress_enqueue_chunk(cc, INTENT_READ);
					return false;
				}
				memcpy(mem, data_ptr, bv.bv_len);
				flush_dcache_page(bv.bv_page);
			} else {
				flush_dcache_page(bv.bv_page);
				memcpy(data_ptr, mem, bv.bv_len);
				memset(cc->sector_tags + offset, TAG_VALID, bv.bv_len >> SECTOR_SHIFT);
				cc->dirty = true;
			}
			kunmap_local(mem);
			data_ptr += bv.bv_len;
			offset += bv.bv_len >> SECTOR_SHIFT;
		}
		if (bio_op(bio) == REQ_OP_WRITE) {
			if (!memchr(cc->sector_tags, 0, 1U << c->log2_sectors_per_chunk)) {
				dm_compress_enqueue_chunk(cc, INTENT_FREE);
			}
		}
		return true;
	}

	ce = map_chunk(c, dio->chunk, &bp);
	if (unlikely(IS_ERR(ce))) {
		bio->bi_status = errno_to_blk_status(PTR_ERR(ce));
		return true;
	}
	cp = &ce->ptr[CC_VALID(c, &ce->cc_txc)];
	if (cp->algorithm == ALGORITHM_ZERO) {
		if (bio_op(bio) == REQ_OP_READ) {
			dm_bufio_release(bp);
			zero_fill_bio(bio);
			return true;
		}
		if (bio_op(bio) == REQ_OP_DISCARD) {
			dm_bufio_release(bp);
			return true;
		}
	}
	dm_bufio_release(bp);

	cc = dm_compress_new_cached_chunk(dio);
	if (cc) {
		struct cached_chunk *qc = dm_compress_find_cached_chunk(c, dio->chunk, cc);
		if (unlikely(qc != NULL)) {
			dm_compress_release_cached_chunk(cc);
			cc = qc;
			goto have_cached_chunk;
		}
		goto have_cached_chunk;
	}

	cc = list_entry(c->lru.prev, struct cached_chunk, lru_entry);
	dm_compress_enqueue_chunk(cc, INTENT_FREE);
	return false;
}

static void process_bio_list(struct dm_compress *c, struct bio_list *bl)
{
	while (!bio_list_empty(bl)) {
		struct bio *bio = bio_list_pop(bl);
		struct dm_compress_io *dio = dm_per_bio_data(bio, sizeof(struct dm_compress_io));
		INIT_WORK(&dio->work, dm_compress_work);
		queue_work(c->wq, &dio->work);
	}
}

static void dm_compress_chunk_work(struct work_struct *w)
{
	struct cached_chunk *cc = container_of(w, struct cached_chunk, work);
	struct dm_compress *c = cc->c;
	struct acomp_req *req;
	unsigned len_in, len_out, len_compressed, trailing_space;
	struct crypto_wait wait;
	int ret;
	unsigned algorithm, n_blocks;
	sector_t blk;
	sector_t preferred_block;
	struct bio *bio;
	struct dm_buffer *bp;
	struct chunk_ptr *cp;
	sector_t existing_block;
	unsigned extra_space = 0;

	init_completion(&cc->completion);

	if (memchr(cc->sector_tags, 0, 1U << c->log2_sectors_per_chunk)) {
		dm_compress_read_and_decompress(cc);
		return;
	}

	mutex_lock(&c->mutex);
	if (cc->intent == INTENT_READ) {
		cc->intent = INTENT_IDLE;
		dm_compress_process_bios_on_chunk(c, cc->chunk);
		return;
	}
	if (!cc->dirty) {
		mutex_unlock(&c->mutex);
		complete(&cc->completion);
		dm_compress_chunk_work_2(&cc->work);
		return;
	}
	cc->dirty = false;
	if (unlikely(cc->failed)) {
		dm_compress_error(c, "Could not write failed chunk", cc->failed);
		mutex_unlock(&c->mutex);
		complete(&cc->completion);
		dm_compress_chunk_work_2(&cc->work);
		return;
	}
	mutex_unlock(&c->mutex);

	req = cc->req;
	len_in = 512U << c->log2_sectors_per_chunk;
	len_out = (512U << c->log2_sectors_per_chunk) - (512U << c->log2_sectors_per_block);

	if (mem_is_zero(cc->uncompressed_data, len_in)) {
		algorithm = ALGORITHM_ZERO;
		len_compressed = 0;
		goto skip_compression;
	}

	algorithm = c->algorithm;
	if (dm_compress_algorithms[algorithm].needs_size) {
		len_out -= 4;
		extra_space = 4;
	}

	acomp_request_set_tfm(req, dm_compress_algorithms[algorithm].cc);
	acomp_request_set_params(req, cc->uncompressed_sglist, cc->compressed_sglist, len_in, len_out);
	acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);

	ret = crypto_wait_req(crypto_acomp_compress(req), &wait);

	len_compressed = req->dlen;
	/*printk("compressed: %u\n", len_compressed);*/
	if (unlikely(ret) || unlikely(!len_compressed) || unlikely(len_compressed > len_out)) {
		memcpy(cc->compressed_data, cc->uncompressed_data, len_in);
		algorithm = ALGORITHM_UNCOMPRESSED;
		len_compressed = len_in;
		extra_space = 0;
	}

skip_compression:
	mutex_lock(&c->mutex);

	trailing_space = -(len_compressed + extra_space) & ((512U << c->log2_sectors_per_block) - 1);
	memset(cc->compressed_data + len_compressed, 0, trailing_space);
	if (extra_space) {
		//printk("chunk %llx: storing compressed len: %x\n", cc->chunk, len_compressed);
		*(uint32_t *)(cc->compressed_data + len_compressed + trailing_space) = cpu_to_le32(len_compressed);
	}

	n_blocks = (len_compressed + extra_space + (512U << c->log2_sectors_per_block) - 1) >> (9 + c->log2_sectors_per_block);

	process_bio_list(c, &cc->waiting_bios);
	preferred_block = READ_ONCE(c->last_allocated);
	mutex_unlock(&c->mutex);

	if (n_blocks != 0) {
		blk = dm_compress_allocate_space(c, preferred_block, n_blocks);
		/*printk("allocate space: %u blocks -> %llu\n", n_blocks, blk);*/
		if (blk == (sector_t)-1) {
			complete(&cc->completion);
			dm_compress_chunk_work_2(&cc->work);
			return;
		}
		WRITE_ONCE(c->last_allocated, blk + n_blocks);
		bio = cc->bio;
		bio_init_inline(bio, c->dev->bdev, cc->bio_vectors, REQ_OP_WRITE);
		bio->bi_iter.bi_sector = blk << c->log2_sectors_per_block;
		if (!c->meta_dev)
			bio->bi_iter.bi_sector += c->start;
		bio->bi_iter.bi_size = n_blocks << (9 + c->log2_sectors_per_block);
		bio->bi_iter.bi_idx = 0;
		bio->bi_iter.bi_bvec_done = 0;
		bio->bi_vcnt = cc->bio_vectors;
		bio->bi_end_io = dm_compress_write_endio;
		bio->bi_private = cc;
		submit_bio(bio);
	} else {
		blk = 0;
	}

	cp = dm_compress_prepare_chunk(c, cc->chunk, &bp);
	if (unlikely(!cp)) {
		complete(&cc->completion);
		dm_compress_chunk_work_2(&cc->work);
		return;
	}

	existing_block = ((__u64)cp->hi << 32) | cp->lo;
	if (cp->algorithm != ALGORITHM_ZERO)
		dm_compress_free_space(c, existing_block, cp->length + 1);

	cp->hi = cpu_to_le16((__u64)blk >> 32);
	cp->lo = cpu_to_le32(blk);
	cp->length = algorithm != ALGORITHM_ZERO ? n_blocks - 1 : 0;
	cp->algorithm = algorithm;

	dm_bufio_mark_buffer_dirty(bp);
	dm_bufio_release(bp);

	complete(&cc->completion);

	if (!n_blocks) {
		dm_compress_chunk_work_2(&cc->work);
	}
}

static void dm_compress_write_endio(struct bio *bio)
{
	struct cached_chunk *cc = bio->bi_private;
	struct dm_compress *c = cc->c;
	if (unlikely(bio->bi_status != BLK_STS_OK))
		dm_compress_error(c, "Error writing compressed data", blk_status_to_errno(bio->bi_status));
	bio_uninit(bio);
	INIT_WORK(&cc->work, dm_compress_chunk_work_2);
	queue_work(c->compress_wq, &cc->work);
}

static void dm_compress_chunk_work_2(struct work_struct *w)
{
	struct cached_chunk *cc = container_of(w, struct cached_chunk, work);
	struct dm_compress *c = cc->c;
	struct bio *bio;
	struct bio_list bio_list;

	wait_for_completion(&cc->completion);

	bio_list_init(&bio_list);

	mutex_lock(&c->mutex);
	if (cc->intent >= INTENT_FREE) {
		dm_compress_release_cached_chunk(cc);
	} else {
		cc->intent = INTENT_IDLE;
	}
	while (true) {
		sector_t chunk;
		struct dm_compress_io *dio = dm_compress_pick_oldest_dio(c);
		if (!dio)
			break;
		chunk = dio->chunk;
		/*printk("picked dio for chunk %llx\n", chunk);*/
		if (!dm_compress_process_chunk(dio, false)) {
			/*printk("dio failed\n");*/
			break;
		}
		dm_compress_unlink_dio(dio);
		bio = dm_bio_from_per_bio_data(dio, sizeof(struct dm_compress_io));
		bio_list_add(&bio_list, bio);
		while ((dio = dm_compress_pick_dio_for_chunk(c, chunk))) {
			/*printk("picked another dio for chunk %llx\n", chunk);*/
			if (!dm_compress_process_chunk(dio, false)) {
				/*printk("dio failed\n");*/
				break;
			}
			dm_compress_unlink_dio(dio);
			bio = dm_bio_from_per_bio_data(dio, sizeof(struct dm_compress_io));
			bio_list_add(&bio_list, bio);
		}
	}
	mutex_unlock(&c->mutex);

	while ((bio = bio_list_pop(&bio_list)))
		bio_endio(bio);
}

static void dm_compress_read_and_decompress(struct cached_chunk *cc)
{
	struct dm_compress *c = cc->c;
	struct dm_buffer *bp;
	struct chunk_entry *ce;
	struct chunk_ptr *cp;
	sector_t blk;
	struct bio *bio;

	ce = map_chunk(c, cc->chunk, &bp);
	if (unlikely(IS_ERR(ce))) {
		dm_compress_error_chunk(cc, "Unable to load chunk descriptor", PTR_ERR(ce));
		mutex_lock(&c->mutex);
		cc->intent = INTENT_IDLE;
		dm_compress_process_bios_on_chunk(c, cc->chunk);
		return;
	}

	cp = &ce->ptr[CC_VALID(c, &ce->cc_txc)];
	blk = ((__u64)le16_to_cpu(cp->hi) << 16) | le32_to_cpu(cp->lo);
	cc->algorithm = cp->algorithm;
	cc->n_blocks = cc->algorithm == ALGORITHM_ZERO ? 0 : cp->length + 1;
	dm_bufio_release(bp);

	if (!cc->n_blocks) {
		dm_compress_chunk_decompress(&cc->work);
		return;
	}

	bio = cc->bio;
	bio_init_inline(bio, c->dev->bdev, cc->bio_vectors, REQ_OP_READ);
	bio->bi_iter.bi_sector = blk << c->log2_sectors_per_block;
	if (!c->meta_dev)
		bio->bi_iter.bi_sector += c->start;
	bio->bi_iter.bi_size = cc->n_blocks << (9 + c->log2_sectors_per_block);
	bio->bi_iter.bi_idx = 0;
	bio->bi_iter.bi_bvec_done = 0;
	bio->bi_vcnt = cc->bio_vectors;
	bio->bi_end_io = dm_compress_read_endio;
	bio->bi_private = cc;
	submit_bio(bio);
}

static void dm_compress_read_endio(struct bio *bio)
{
	struct cached_chunk *cc = bio->bi_private;
	struct dm_compress *c = cc->c;
	if (unlikely(bio->bi_status != BLK_STS_OK))
		dm_compress_error(c, "Error reading compressed data", blk_status_to_errno(bio->bi_status));
	bio_uninit(bio);
	INIT_WORK(&cc->work, dm_compress_chunk_decompress);
	queue_work(c->compress_wq, &cc->work);
}

static void dm_compress_chunk_decompress(struct work_struct *w)
{
	struct cached_chunk *cc = container_of(w, struct cached_chunk, work);
	struct dm_compress *c = cc->c;
	unsigned i;
	unsigned intent;

	if (cc->algorithm == ALGORITHM_ZERO) {
		mutex_lock(&c->mutex);
		for (i = 0; i < 1 << c->log2_sectors_per_chunk; i++) {
			if (!cc->sector_tags[i]) {
				unsigned offset = i << 9;
				memset(cc->uncompressed_data + offset, 0, 512);
			}
		}
	} else if (cc->algorithm == ALGORITHM_UNCOMPRESSED) {
		mutex_lock(&c->mutex);
		for (i = 0; i < 1 << c->log2_sectors_per_chunk; i++) {
			if (!cc->sector_tags[i]) {
				unsigned offset = i << 9;
				memcpy(cc->uncompressed_data + offset, cc->compressed_data + offset, 512);
			}
		}
	} else if (cc->algorithm < dm_compress_n_algorithms) {
		int ret;
		struct acomp_req *req;
		struct crypto_wait wait;
		unsigned len_in, len_out;

		req = cc->req;
		len_out = 512U << c->log2_sectors_per_chunk;
		len_in = cc->n_blocks << (9 + c->log2_sectors_per_block);
		if (dm_compress_algorithms[cc->algorithm].needs_size) {
			uint32_t used_len = le32_to_cpu(*(int32_t *)(cc->compressed_data + len_in - 4));
			len_in = used_len;
			//printk("chunk %llx: retrieving compressed len: %x\n", cc->chunk, used_len);
		}
		if (unlikely(len_in > 512 << c->log2_sectors_per_chunk)) {
			dm_compress_error_chunk(cc, "Invalid length", -EUCLEAN);
			mutex_lock(&c->mutex);
			goto cont;
		}

		acomp_request_set_tfm(req, dm_compress_algorithms[cc->algorithm].cc);
		acomp_request_set_params(req, cc->compressed_sglist, cc->merging_sglist, len_in, len_out);
		acomp_request_set_callback(req, CRYPTO_TFM_REQ_MAY_BACKLOG, crypto_req_done, &wait);

		ret = crypto_wait_req(crypto_acomp_decompress(req), &wait);
		if (unlikely(ret)) {
			dm_compress_error_chunk(cc, "Decompression failed", ret);
			mutex_lock(&c->mutex);
			goto cont;
		}
		if (unlikely(req->dlen != len_out)) {
			dm_compress_error_chunk(cc, "Decompressed less data than requested", -EUCLEAN);
			mutex_lock(&c->mutex);
			goto cont;
		}

		mutex_lock(&c->mutex);
		for (i = 0; i < 1 << c->log2_sectors_per_chunk; i++) {
			if (!cc->sector_tags[i]) {
				unsigned offset = i << 9;
				memcpy(cc->uncompressed_data + offset, cc->merging_data + offset, 512);
			}
		}
	} else {
		dm_compress_error_chunk(cc, "Unsupported algorithm", -EOPNOTSUPP);
		mutex_lock(&c->mutex);
	}

cont:
	memset(cc->sector_tags, 1, 1U << c->log2_sectors_per_chunk);

	intent = cc->intent;
	if (intent == INTENT_READ) {
		cc->intent = INTENT_IDLE;
	}

	dm_compress_process_bios_on_chunk(c, cc->chunk);

	if (intent >= INTENT_WRITE) {
		dm_compress_chunk_work(&cc->work);
	}
}

static void dm_compress_process_bios_on_chunk(struct dm_compress *c, sector_t chunk)
{
	struct bio_list bio_list;
	struct dm_compress_io *dio;
	struct bio *bio;

	bio_list_init(&bio_list);

	while ((dio = dm_compress_pick_dio_for_chunk(c, chunk)) || (dio = dm_compress_pick_oldest_dio(c))) {
		if (!dm_compress_process_chunk(dio, false)) {
			break;
		}
		dm_compress_unlink_dio(dio);
		bio = dm_bio_from_per_bio_data(dio, sizeof(struct dm_compress_io));
		bio_list_add(&bio_list, bio);
	}

	mutex_unlock(&c->mutex);

	while ((bio = bio_list_pop(&bio_list)))
		bio_endio(bio);
}
