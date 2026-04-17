===========
dm-compress
===========

The dm-compress target provides transparent compression of data on a block
device.

The device is divided into blocks, a block is the basic allocation unit. Block
size must be a power of two and it must be greater or equal than the logical
block size of the underlying device. A smaller block size improves
space-efficiency. SSDs and some hard disks perform read-modify-write cycle when
writing data that is not aligned on 4k boundary, so a block size smaller than 4k
may degrade performance.

The device is managed in the units of chunks. Chunk size must be a power of two
and it must be greater than block size. Every chunk is fully compressed using
the selected algorithm and it is stored in one or more blocks (or no blocks, if
it contains only zeros). Greater chunk size improves the compression ration, but
it also degrades performance because the full chunk must be decompressed when
accessing any part of the chunk.

When loading the target for the first time, the kernel driver will format
the device. The 4k superblock (after the reserved area) must contain zeros. If
the superblock is not zeroed, the device can't be formatted and the target
constructor will report an error.


Target arguments:

1. the underlying block device

2. the number of reserved sector at the beginning of the device - dm-compress
   won't read or write these sectors

3. block size (a power of two in the range 512 - 524288)

4. chunk size (a power of two in the range 1024 - 1048576)
   the chunk size divided by the block size must be less or equal than 256

5. the total number of chunks

6. the compression algorithm - the supported algorithms are:
   deflate, lzo, 842, lz4, lz4hc, zstd

7. the number of additional arguments

Additional arguments:

meta_device:device
	Store metadata on a separate device

n_cached_chunks:number (default 128)
	The number of cached decompressed chunks that is kept in memory. The
	cached chunks are preallocated when the target is loaded. The memory
	consumption of the target is n_cached_chunks*chunk_size*3.


Status line:

1. The total number of blocks
2. The number of free blocks
