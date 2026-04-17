#include <linux/device-mapper.h>
#include <linux/dm-io.h>
#include <linux/dm-bufio.h>
#include <linux/vmalloc.h>
#include <crypto/acompress.h>

#define DM_MSG_PREFIX "compress"

#define BUFFER_SIZE			4096
#define METADATA_WORKQUEUE_MAX_ACTIVE	128
#define N_CACHED_CHUNKS			128

#define MAX_BLOCK_SIZE			1048576
#define MAX_CHUNK_SIZE			1048576

#define SB_MAGIC	"dm-comp"

#define SB_VERSION_1	1

#define SB_SECTORS	(BUFFER_SIZE >> SECTOR_SHIFT)

struct superblock {
	__u8 magic[8];
	__u8 version;
	__u8 log2_buffer_size;		/* log2 of BUFFER_SIZE */
	__u8 log2_sectors_per_block;
	__u8 log2_sectors_per_chunk;
	__le16 cc;
	__le16 pad;
	__le64 cct;			/* ptr to struct crash_count_table */
	__le64 n_cc_txc;
	__le64 n_blocks[2];
	__le64 n_chunks[2];
	__le64 bitmap_directory;	/* ptr to struct directory */
	__le64 chunk_directory;		/* ptr to struct directory */
};

#define CCT_ENTRIES	65536

struct crash_count_table {
	__le64 entries[CCT_ENTRIES];
};

#define CCT_SIZE	sizeof(struct crash_count_table)

#define ALGORITHM_ZERO		0xff
#define ALGORITHM_UNCOMPRESSED	0xfe

struct chunk_ptr {
	__le32 lo;
	__le16 hi;
	__u8 length;
	__u8 algorithm;
};

struct chunk_entry {
	__le64 cc_txc;
	struct chunk_ptr ptr[2];
};

#define CHUNKS_PER_BUFFER	(BUFFER_SIZE / sizeof(struct chunk_entry))

struct chunk_entries {
	struct chunk_entry chunk[CHUNKS_PER_BUFFER];
} __attribute__((__aligned__(BUFFER_SIZE)));

#define BITS_PER_BITMAP		((BUFFER_SIZE / 2 - 8) * 8)

struct bitmap {
	__le64 cc_txc;
	__u64 bmp1[BITS_PER_BITMAP / 64];
	__le64 pad2;
	__u64 bmp2[BITS_PER_BITMAP / 64];
} __attribute__((__aligned__(BUFFER_SIZE)));

struct ptr {
	__le32 lo;
	__le16 hi;
} __attribute__((__packed__));

#define DIRECTORY_ENTRIES	(BUFFER_SIZE / sizeof(struct ptr) - 1)

struct directory {
	struct ptr ptr[DIRECTORY_ENTRIES];	/* ptr to struct chunk_entries or struct bitmap */
	struct ptr next;			/* ptr to next struct directory */
} __attribute__((__aligned__(BUFFER_SIZE)));

struct algorithm {
	const char *name;
	struct crypto_acomp *cc;
	bool needs_size;
};

struct block_group_descriptor {
	struct mutex mutex;
	sector_t free;
} ____cacheline_aligned_in_smp;

#define TAG_VALID		1

struct cached_chunk {
	struct rb_node node;
	struct list_head lru_entry;
	sector_t chunk;
	struct dm_compress *c;
	bool in_tree;
	bool dirty;
#define INTENT_IDLE		0
#define INTENT_READ		1
#define INTENT_WRITE		2
#define INTENT_FREE		3
	uint8_t intent;
	uint8_t algorithm;
	unsigned n_blocks;
	int failed;
	void *uncompressed_data;
	struct scatterlist *uncompressed_sglist;
	void *compressed_data;
	struct scatterlist *compressed_sglist;
	void *merging_data;
	struct scatterlist *merging_sglist;
	struct work_struct work;
	struct acomp_req *req;
	struct bio_list waiting_bios;
	unsigned short bio_vectors;
	struct completion completion;
	struct bio *bio;
	uint8_t sector_tags[];
};

struct dm_compress {
	struct dm_target *ti;
	sector_t start;
	struct dm_dev *dev;
	struct dm_dev *meta_dev;
	unsigned char log2_sectors_per_block;
	unsigned char log2_sectors_per_chunk;
	unsigned char algorithm;
	sector_t n_blocks;
	sector_t n_chunks;
	struct superblock *sb;
	struct crash_count_table *cct;
	__u64 cc_txc;

	int failed;

	struct workqueue_struct *wq;
	struct workqueue_struct *compress_wq;
	struct workqueue_struct *flush_wq;

	struct dm_bufio_client *bufio;
	struct dm_io_client *io;

	sector_t *block_directories;
	sector_t *chunk_directories;
	struct block_group_descriptor *groups;
	size_t n_groups;
	sector_t last_allocated;

	atomic64_t free_blocks;

	struct list_head free_chunks;
	struct list_head lru;
	struct rb_root chunks;
	struct mutex mutex;

	struct list_head waiting_bio_lru;
	struct rb_root waiting_bio_tree;

	unsigned n_cached_chunks;
};

struct dm_compress_io {
	struct dm_compress *c;
	struct work_struct work;
	sector_t chunk;
	unsigned offset_in_chunk;

	struct list_head waiting_list_entry;
	struct rb_node tree_entry;
};

static inline bool CC_VALID(struct dm_compress *c, __le64 *cc_txc)
{
	__u64 ct = le64_to_cpup(cc_txc);
	__u16 cc = ct;
	return (__s64)(c->cct->entries[cc] - ct) >= 0;
}

static inline bool CC_CURRENT(struct dm_compress *c, __le64 *cc_txc)
{
	__u64 ct = le64_to_cpup(cc_txc);
	return ((c->cc_txc ^ ct) & ~(1ULL << 63)) == 0;
}

static inline void CC_SET_CURRENT(struct dm_compress *c, __le64 *cc_txc)
{
	__u64 ct = le64_to_cpup(cc_txc);
	__u16 cc = ct;
	__u64 top_bit = ~(c->cct->entries[cc] - ct) & (1ULL << 63);
	*cc_txc = cpu_to_le64(c->cc_txc | top_bit);
}

static inline sector_t READ_PTR(struct ptr *ptr)
{
	return le32_to_cpup(&(ptr)->lo) | ((__u64)le16_to_cpup(&(ptr)->hi) << 32);
}

static inline void block_location(struct dm_compress *c, sector_t block, size_t *group_ptr, size_t *dir_ptr, unsigned *dir_offset, unsigned *bmp_offset)
{
	*bmp_offset = do_div(block, BITS_PER_BITMAP);
	*group_ptr = block;
	*dir_offset = do_div(block, DIRECTORY_ENTRIES);
	*dir_ptr = block;
}

static inline void chunk_location(struct dm_compress *c, sector_t chunk, size_t *dir_ptr, unsigned *dir_offset, unsigned *chunk_offset)
{
	*chunk_offset = do_div(chunk, CHUNKS_PER_BUFFER);
	*dir_offset = do_div(chunk, DIRECTORY_ENTRIES);
	*dir_ptr = chunk;
}

/* target.c */

extern struct algorithm dm_compress_algorithms[];
extern const unsigned dm_compress_n_algorithms;
extern unsigned dm_compress_max_request_size;

void dm_compress_error(struct dm_compress *c, const char *msg, int err);
void dm_compress_error_chunk(struct cached_chunk *cc, const char *msg, int err);
void dm_compress_sync(struct dm_compress *c);

/* cc.c */

struct cached_chunk *dm_compress_alloc_cached_chunk(struct dm_compress *c);
void dm_compress_free_cached_chunk(struct cached_chunk *cc);
struct cached_chunk *dm_compress_find_cached_chunk(struct dm_compress *c, sector_t chunk, struct cached_chunk *for_insert);
struct cached_chunk *dm_compress_new_cached_chunk(struct dm_compress_io *dio);
void dm_compress_release_cached_chunk(struct cached_chunk *cc);
void dm_compress_wait_for_cached_chunk(struct dm_compress_io *dio);
struct dm_compress_io *dm_compress_pick_dio_for_chunk(struct dm_compress *c, sector_t chunk);
struct dm_compress_io *dm_compress_pick_oldest_dio(struct dm_compress *c);
void dm_compress_unlink_dio(struct dm_compress_io *dio);
bool dm_compress_is_something_in_progress(struct dm_compress *c);

/* io.c */

void dm_compress_enqueue_chunk(struct cached_chunk *cc, unsigned intent);
int dm_compress_map(struct dm_target *ti, struct bio *bio);
void dm_compress_work(struct work_struct *w);

/* alloc.c */

sector_t dm_compress_allocate_space(struct dm_compress *c, sector_t preferred_block, unsigned n_blocks);
void dm_compress_free_space(struct dm_compress *c, sector_t block, unsigned n_blocks);
int dm_compress_get_free_space(struct dm_compress *c);
struct chunk_ptr *dm_compress_prepare_chunk(struct dm_compress *c, sector_t chunk, struct dm_buffer **bp);
