#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

#include "ouichefs.h"
#include "bitmap.h"

/* Slice allocation */

/*
 * Return: the largest sequence of free spaces in the given bitmap starting at start_at
 */
static uint32_t get_size_of_gap(const uint32_t bitmap, const uint32_t start_at)
{
	uint32_t gap = 0;
	for (uint32_t i = start_at;
	     i < OUICHEFS_SLICES_PER_BLOCK && (bitmap & (1U << i)); ++i) {
		gap++;
	}
	return gap;
}

/**
 * update_largest_gap - Update the largest gap value in a slice bitmap.
 * @bitmap: Slice bitmap.
 * @largest_gap: Pointer to largest gap variable to update.
 * Scans the bitmap and sets *@largest_gap to the largest free gap found.
 */
static void update_largest_gap(const unsigned long bitmap,
			       uint32_t *largest_gap)
{
	uint32_t gap;

	int i = 1; /* Skip metadata slice */
	while (i < OUICHEFS_SLICES_PER_BLOCK) {
		i = find_next_bit(&bitmap, OUICHEFS_SLICES_PER_BLOCK, i);
		gap = get_size_of_gap(bitmap, i);
		if (gap >= *largest_gap)
			*largest_gap = gap;

		i += gap;
	}
}

/*
 * Uses the best fit algorithm to find the a position of at least @needed size in the given bitmap.
 * Return: 0 if not enough free slices found (assumes that the first slice is always metadata)
 */
static int find_best_fit(const unsigned long bitmap, const uint32_t needed)
{
	if (!needed)
		return -EINVAL;

	uint32_t best_len = OUICHEFS_SLICES_PER_BLOCK;
	int best_start = 0;
	uint32_t gap;

	int i = 1; /* Skip metadata slice */
	while (i <= OUICHEFS_SLICES_PER_BLOCK - needed) {
		i = find_next_bit(&bitmap, OUICHEFS_SLICES_PER_BLOCK, i);
		gap = get_size_of_gap(bitmap, i);
		if (gap >= needed && gap < best_len) {
			best_len = gap;
			best_start = i;
		}

		i += gap;
	}
	if (!best_start)
		return 0;

	return best_start;
}

/**
 * get_new_sliced_block - Allocate and initialize a new sliced block.
 * @sbi: Filesystem superblock info.
 *
 * Allocates a new block, initializes its slice bitmap, and links it into
 * the partial block list.
 * Return: 0 on success, -ENOSPC or -EIO on error.
 */
static int get_new_sliced_block(struct ouichefs_sb_info *sbi)
{
	struct buffer_head *bh;
	sector_t sliced_block_nr = get_free_block(sbi);

	if (!sliced_block_nr)
		return -ENOSPC;

	bh = sb_bread(sbi->sb, sliced_block_nr);
	if (!bh)
		return -EIO;

	/* Scrub new block */
	memset((char *)bh->b_data, 0, OUICHEFS_BLOCK_SIZE);

	struct ouichefs_sliced_block *s_block =
		(struct ouichefs_sliced_block *)bh->b_data;

	unsigned long slice_bitmap = GENMASK(31, 1);
	s_block->header.slice_bitmap = cpu_to_le32(slice_bitmap);
	s_block->header.next_partial_block = sbi->s_free_sliced_blocks;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	sbi->s_free_sliced_blocks = sliced_block_nr;
	sbi->nr_sliced_blocks++;

	return 0;
}

/**
 * alloc_slices - Allocate slices for an inode.
 * @inode: Inode to allocate for.
 * @res_bno: Pointer to return block number.
 * @res_slice: Pointer to return starting slice.
 * @bh: Pointer to buffer_head pointer for the block (caller must brelse).
 * @nr_slices: Number of slices to allocate.
 *
 * Finds or allocates a sliced block with enough free slices, updates the bitmap,
 * and sets the block and slice index.
 *
 * Context: If a buffer_head is returned, caller must release it with brelse().
 * Return: 0 on success, negative error code on failure.
 */
static int alloc_slices(struct inode *inode, sector_t *res_bno,
			unsigned int *res_slice, struct buffer_head **bh,
			uint32_t nr_slices)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_sliced_block *s_block;
	uint32_t largest_gap = 0;
	bool new_block = false;

	*res_bno = sbi->s_free_sliced_blocks;

	while (*res_bno != 0) {
		*bh = sb_bread(sb, *res_bno);
		if (!(*bh))
			return -EIO;

		s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
		largest_gap = le32_to_cpu(s_block->header.largest_gap);
		if (largest_gap >= nr_slices)
			break;

		*res_bno = le32_to_cpu(s_block->header.next_partial_block);
		brelse(*bh);
	}
	if (*res_bno == 0) {
		int err = get_new_sliced_block(sbi);
		if (err < 0)
			return err;
		*res_bno = sbi->s_free_sliced_blocks;
		*bh = sb_bread(sb, *res_bno);
		if (!(*bh))
			return -EIO;
		s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
		new_block = true;
	}

	unsigned long bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	if (!new_block) {
		*res_slice = find_best_fit(bitmap, nr_slices);

		if (*res_slice <= 0) {
			brelse(*bh);
			return -EFAULT;
		}
	} else {
		*res_slice = 1;
		largest_gap = 0;
	}

	bitmap_clear(&bitmap, *res_slice, nr_slices);

	if (bitmap == 0) {
		sbi->s_free_sliced_blocks =
			le32_to_cpu(s_block->header.next_partial_block);
		s_block->header.largest_gap = 0;
	} else {
		largest_gap = 0;
		update_largest_gap(bitmap, &largest_gap);
		s_block->header.largest_gap = cpu_to_le32(largest_gap);
	}

	s_block->header.slice_bitmap = cpu_to_le32(bitmap);

	return 0;
}

/*
 * Sets the sliced_block_nr and the slice for the given inode by setting the given pointers.
 * Return: 0 on success, negative error if given file has no data
 */
static int file_get_first_slice(struct inode *inode, sector_t *sliced_block_nr,
				uint32_t *slice)
{
	uint32_t index = OUICHEFS_INODE(inode)->index_block;
	if (!index)
		return -RETURN_UNALLOCATED;

	*sliced_block_nr = index & GENMASK(26, 0);
	*slice = (index >> 27) & GENMASK(4, 0);

	return 0;
}

/**
 * sliced_get_start - Get pointer to start of slice for reading.
 * @inode: Inode to read from.
 * @pos: Position in file.
 * @bh: Pointer to buffer_head pointer (caller must brelse).
 * @start: Pointer to return slice data pointer.
 *
 * Returns a pointer to the start of the slice for reading.
 * Context: Caller must release the buffer_head with brelse().
 * Return: 0 on success, negative error code on failure.
 */
int read_sliced_get_start(struct inode *inode, loff_t pos,
			  struct buffer_head **bh, char **start)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sliced_block *s_block;
	sector_t sliced_block_nr;
	uint32_t slice;

	int err = file_get_first_slice(inode, &sliced_block_nr, &slice);
	if (err)
		return err;

	*bh = sb_bread(sb, sliced_block_nr);
	if (!(*bh))
		return -EIO;

	s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

/**
 * write_sliced_get_start - Get pointer to start of slice for writing.
 * @inode: Inode to write to.
 * @pos: Position to write at.
 * @count: Number of bytes to write.
 * @bh: Pointer to buffer_head pointer (caller must brelse).
 * @start: Pointer to return slice data pointer.
 * @new_index_block: Pointer to return new index_block value.
 *
 * Allocates slices if needed and returns a pointer to the start of the slice for writing.
 * Context: Caller must release the buffer_head with brelse().
 * Return: 0 on success, negative error code on failure.
 */
int write_sliced_get_start(struct inode *inode, loff_t pos, size_t count,
			   struct buffer_head **bh, char **start,
			   sector_t *new_index_block)
{
	struct ouichefs_sliced_block *s_block;
	sector_t sliced_block_nr;
	uint32_t slice;
	uint32_t current_slices;
	uint32_t needed_slices;
	int err;

	current_slices = idiv_ceil(inode->i_size, OUICHEFS_SLICE_SIZE);
	needed_slices = idiv_ceil(pos + count, OUICHEFS_SLICE_SIZE);
	if (needed_slices > current_slices) {
alloc_new:
		if (current_slices > 0) {
			err = free_sliced_file(inode);
			if (err)
				return err;
		}
		err = alloc_slices(inode, &sliced_block_nr, &slice, bh,
				   needed_slices);
		if (err)
			return err;
	} else {
		err = file_get_first_slice(inode, &sliced_block_nr, &slice);
		if (err) {
			pr_err("got UNALLOCATED where it should not be possible");
			goto alloc_new;
		}
	}

	if (!(*bh)) {
		*bh = sb_bread(inode->i_sb, sliced_block_nr);
		if (!(*bh))
			return -EIO;
	}

	/* WARN: block number has 27 bits at max */
	*new_index_block = (slice << 27) | sliced_block_nr;

	s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

/* Partial list handling */

/**
 * remove_from_partial_list - Remove a block from the list of partial blocks.
 * @sb: Superblock.
 * @block: Block number to remove.
 * @s_block: Sliced block struct for @block.
 *
 * Unlinks @block from the partial block list.
 * Return: 0 on success, negative error code on failure.
 */
int remove_from_partial_list(struct super_block *sb, sector_t block,
			     struct ouichefs_sliced_block *s_block)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_prev;
	struct ouichefs_sliced_block *prev_block;
	sector_t iter_bno;
	int ret = 0;

	iter_bno = sbi->s_free_sliced_blocks;

	if (!iter_bno)
		return -ENOENT;

	if (iter_bno == block) {
		sbi->s_free_sliced_blocks =
			le32_to_cpu(s_block->header.next_partial_block);
		return 0;
	}

	bh_prev = sb_bread(sb, iter_bno);
	if (!bh_prev)
		return -EIO;

	prev_block = (struct ouichefs_sliced_block *)bh_prev->b_data;
	iter_bno = le32_to_cpu(prev_block->header.next_partial_block);

	/* Iterates through list of partial blocks till block or 0 is next */
	while (iter_bno != block && iter_bno != 0) {
		brelse(bh_prev);
		bh_prev = sb_bread(sb, iter_bno);
		if (!bh_prev)
			return -EIO;

		prev_block = (struct ouichefs_sliced_block *)bh_prev->b_data;
		iter_bno = le32_to_cpu(prev_block->header.next_partial_block);
	}
	if (!iter_bno) {
		ret = -ENOENT;
	} else {
		prev_block->header.next_partial_block =
			s_block->header.next_partial_block;
		mark_buffer_dirty(bh_prev);
	}
	brelse(bh_prev);
	return ret;
}

/*
 * Add a block to the list of partial blocks
 */
static inline void add_to_partial_list(struct ouichefs_sb_info *sbi,
				       struct ouichefs_sliced_block *s_block,
				       sector_t block)
{
	s_block->header.next_partial_block =
		cpu_to_le32(sbi->s_free_sliced_blocks);
	sbi->s_free_sliced_blocks = block;
}

/* Releasing slices*/

/**
 * put_slices - Free slices in a block.
 * @sb: Superblock.
 * @block: Block number.
 * @starting_slice: First slice to free.
 * @nr_slices: Number of slices to free.
 *
 * Marks slices as free in the bitmap, clears their data, and updates the partial list.
 * Return: 0 on success, negative error code on failure.
 */
int put_slices(struct super_block *sb, sector_t block, uint32_t starting_slice,
	       uint32_t nr_slices)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;

	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	struct ouichefs_sliced_block *s_block =
		(struct ouichefs_sliced_block *)bh->b_data;
	unsigned long slice_bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	if (slice_bitmap == 0) { /* Slice was put from full block */
		add_to_partial_list(sbi, s_block, block);
	}

	bitmap_set(&slice_bitmap, starting_slice, nr_slices);
	memset(s_block->slices[starting_slice], 0,
	       OUICHEFS_SLICE_SIZE * nr_slices);

	/* If slice was last in block, put block */
	if (slice_bitmap == GENMASK((OUICHEFS_SLICES_PER_BLOCK - 1), 1)) {
		memset(&s_block->header, 0, OUICHEFS_SLICE_SIZE);
		int err = remove_from_partial_list(sb, block, s_block);
		if (err)
			return err;

		sbi->nr_sliced_blocks--;
		put_block(sbi, block);
	} else {
		uint32_t largest_gap = le32_to_cpu(s_block->header.largest_gap);
		update_largest_gap(slice_bitmap, &largest_gap);
		s_block->header.slice_bitmap = cpu_to_le32(slice_bitmap);
		s_block->header.largest_gap = cpu_to_le32(largest_gap);
	}

	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

/**
 * free_sliced_file - Free all slices for a file.
 * @inode: Inode to free.
 *
 * Frees all slices associated with the file.
 * Return: 0 on success, negative error code on failure.
 */
int free_sliced_file(struct inode *inode)
{
	sector_t block;
	uint32_t slice;

	int err = file_get_first_slice(inode, &block, &slice);
	if (err)
		return err;

	uint32_t nr_slices = idiv_ceil(inode->i_size, OUICHEFS_SLICE_SIZE);

	put_slices(
		inode->i_sb, block, slice,
		nr_slices); /* Ignore error, but can result in data space loss */

	pr_debug("unlink in block nr: %llu, for slice %d", block, slice);
	return 0;
}
