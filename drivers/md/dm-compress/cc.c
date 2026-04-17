#include "dm-compress.h"

static struct scatterlist *dm_compress_alloc_sglist(void *data, unsigned len)
{
	struct scatterlist *sg;
	if (!is_vmalloc_addr(data)) {
		sg = kmalloc(sizeof(struct scatterlist), GFP_KERNEL);
		if (!sg)
			return NULL;
		sg_init_one(sg, data, len);
	} else {
		unsigned i;
		unsigned n_entries = DIV_ROUND_UP(len, PAGE_SIZE);
		size_t s = array_size(sizeof(struct scatterlist), n_entries);
		sg = kmalloc(s, GFP_KERNEL);
		if (!sg)
			return NULL;
		sg_init_table(sg, n_entries);
		for (i = 0; i < n_entries; i++) {
			struct page *pg = vmalloc_to_page(data);
			unsigned pg_len = min(len, PAGE_SIZE);
			sg_set_page(&sg[i], pg, pg_len, 0);
			data += PAGE_SIZE;
			len -= PAGE_SIZE;
		}
	}
	return sg;
}

struct cached_chunk *dm_compress_alloc_cached_chunk(struct dm_compress *c)
{
	struct cached_chunk *cc;
	unsigned short v;
	struct bio_vec *bv;

	cc = kzalloc(sizeof(struct cached_chunk) + (1U << c->log2_sectors_per_chunk), GFP_KERNEL);
	if (!cc)
		return NULL;

	cc->c = c;

	cc->uncompressed_data = __vmalloc(512UL << c->log2_sectors_per_chunk, GFP_KERNEL);
	if (!cc->uncompressed_data)
		goto free_fail;

	cc->uncompressed_sglist = dm_compress_alloc_sglist(cc->uncompressed_data, 512UL << c->log2_sectors_per_chunk);
	if (!cc->uncompressed_sglist)
		goto free_fail;

	cc->compressed_data = __vmalloc(512UL << c->log2_sectors_per_chunk, GFP_KERNEL);
	if (!cc->compressed_data)
		goto free_fail;

	cc->compressed_sglist = dm_compress_alloc_sglist(cc->compressed_data, 512UL << c->log2_sectors_per_chunk);
	if (!cc->compressed_sglist)
		goto free_fail;

	cc->merging_data = __vmalloc(512UL << c->log2_sectors_per_chunk, GFP_KERNEL);
	if (!cc->merging_data)
		goto free_fail;

	cc->merging_sglist = dm_compress_alloc_sglist(cc->merging_data, 512UL << c->log2_sectors_per_chunk);
	if (!cc->merging_sglist)
		goto free_fail;

	cc->req = kmalloc(dm_compress_max_request_size, GFP_KERNEL);
	if (!cc->req)
		goto free_fail;

	if (is_vmalloc_addr(cc->compressed_data)) {
		cc->bio_vectors = DIV_ROUND_UP(512UL << c->log2_sectors_per_chunk, PAGE_SIZE);
		cc->bio = bio_kmalloc(cc->bio_vectors, GFP_KERNEL);
		if (!cc->bio)
			goto free_fail;
		bv = bio_inline_vecs(cc->bio);
		for (v = 0; v < cc->bio_vectors; v++) {
			bv[v].bv_page = vmalloc_to_page(cc->compressed_data + (size_t)v * PAGE_SIZE);
			bv[v].bv_len = PAGE_SIZE;
			bv[v].bv_offset = 0;
		}
	} else {
		cc->bio_vectors = 1;
		cc->bio = bio_kmalloc(cc->bio_vectors, GFP_KERNEL);
		bv = bio_inline_vecs(cc->bio);
		bv[0].bv_page = virt_to_page(cc->compressed_data);
		bv[0].bv_len = 512UL << c->log2_sectors_per_chunk;
		bv[0].bv_offset = 0;
	}

	return cc;

free_fail:
	dm_compress_free_cached_chunk(cc);
	return NULL;
}

void dm_compress_free_cached_chunk(struct cached_chunk *cc)
{
	kvfree(cc->uncompressed_data);
	kfree(cc->uncompressed_sglist);
	kvfree(cc->compressed_data);
	kfree(cc->compressed_sglist);
	kvfree(cc->merging_data);
	kfree(cc->merging_sglist);
	kfree(cc->req);
	kfree(cc->bio);
	kfree(cc);
}

struct cached_chunk *dm_compress_find_cached_chunk(struct dm_compress *c, sector_t chunk, struct cached_chunk *for_insert)
{
	struct rb_node *parent = NULL;
	struct rb_node **node = &c->chunks.rb_node;

	while (*node) {
		struct cached_chunk *cc = container_of(*node, struct cached_chunk, node);
		if (cc->chunk == chunk) {
			list_del(&cc->lru_entry);
			list_add(&cc->lru_entry, &c->lru);
			return cc;
		}
		parent = &cc->node;
		if (cc->chunk > chunk)
			node = &parent->rb_left;
		else
			node = &parent->rb_right;
	}

	if (for_insert) {
		rb_link_node(&for_insert->node, parent, node);
		rb_insert_color(&for_insert->node, &c->chunks);
		list_add(&for_insert->lru_entry, &c->lru);
		for_insert->in_tree = true;
	}

	return NULL;
}

struct cached_chunk *dm_compress_new_cached_chunk(struct dm_compress_io *dio)
{
	struct dm_compress *c = dio->c;
	struct cached_chunk *cc;
	if (!list_empty(&c->free_chunks)) {
		cc = list_entry(c->free_chunks.next, struct cached_chunk, lru_entry);
		list_del(&cc->lru_entry);
		cc->chunk = dio->chunk;
		cc->in_tree = false;
		cc->dirty = false;
		cc->intent = INTENT_IDLE;
		cc->failed = 0;
		bio_list_init(&cc->waiting_bios);
		memset(cc->sector_tags, 0, 1U << c->log2_sectors_per_chunk);
		return cc;
	}
	return NULL;
}

void dm_compress_release_cached_chunk(struct cached_chunk *cc)
{
	struct dm_compress *c = cc->c;
	if (cc->in_tree) {
		rb_erase(&cc->node, &c->chunks);
		list_del(&cc->lru_entry);
		cc->in_tree = false;
	}
	list_add(&cc->lru_entry, &c->free_chunks);
}

void dm_compress_wait_for_cached_chunk(struct dm_compress_io *dio)
{
	struct dm_compress *c = dio->c;
	struct rb_node *parent, **node;

	list_add(&dio->waiting_list_entry, &c->waiting_bio_lru);

	parent = NULL;
	node = &c->waiting_bio_tree.rb_node;

	while (*node) {
		struct dm_compress_io *tdio = container_of(*node, struct dm_compress_io, tree_entry);
		parent = &tdio->tree_entry;
		if (tdio->chunk > dio->chunk) {
			node = &parent->rb_left;
		} else {
			node = &parent->rb_right;
		}
	}

	rb_link_node(&dio->tree_entry, parent, node);
	rb_insert_color(&dio->tree_entry, &c->waiting_bio_tree);
}

struct dm_compress_io *dm_compress_pick_dio_for_chunk(struct dm_compress *c, sector_t chunk)
{
	struct rb_node *node = c->waiting_bio_tree.rb_node;

	while (node) {
		struct dm_compress_io *tdio = container_of(node, struct dm_compress_io, tree_entry);
		if (tdio->chunk == chunk) {
			return tdio;
		}
		if (tdio->chunk > chunk)
			node = tdio->tree_entry.rb_left;
		else
			node = tdio->tree_entry.rb_right;
	}

	return NULL;
}

struct dm_compress_io *dm_compress_pick_oldest_dio(struct dm_compress *c)
{
	struct dm_compress_io *dio;

	if (list_empty(&c->waiting_bio_lru))
		return NULL;

	dio = list_entry(c->waiting_bio_lru.prev, struct dm_compress_io, waiting_list_entry);

	return dio;
}

void dm_compress_unlink_dio(struct dm_compress_io *dio)
{
	struct dm_compress *c = dio->c;
	list_del(&dio->waiting_list_entry);
	rb_erase(&dio->tree_entry, &c->waiting_bio_tree);
}

bool dm_compress_is_something_in_progress(struct dm_compress *c)
{
	bool ret = false;
	struct cached_chunk *cc;
	list_for_each_entry(cc, &c->lru, lru_entry) {
		if (cc->dirty)
			dm_compress_enqueue_chunk(cc, INTENT_WRITE);
		if (cc->intent > INTENT_IDLE)
			ret = true;
	}
	return ret;
}
