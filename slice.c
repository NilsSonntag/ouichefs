#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

#include "ouichefs.h"
#include "slice.h"
#include "bitmap.h"

/*
 * 
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

static void update_largest_gap(const unsigned long bitmap,
			       uint32_t *largest_gap)
{
	uint32_t gap;

	int i = 1; /* skip metadata slice */
	while (i < OUICHEFS_SLICES_PER_BLOCK) {
		i = find_next_bit(&bitmap, OUICHEFS_SLICES_PER_BLOCK, i);
		gap = get_size_of_gap(bitmap, i);
		if (gap >= *largest_gap)
			*largest_gap = gap;

		i += gap;
	}
}

/*
 * Return 0 if not enough free slices found (we assume that the first slice is never free
 * because of the metadata, thus allowing us to use 0 as an error value).
 */
static int find_best_fit(const unsigned long bitmap, const uint32_t needed)
{
	if (!needed) {
		pr_err("find_best_fit was called to find 0 slices");
		return -EINVAL;
	}

	uint32_t best_len = OUICHEFS_SLICES_PER_BLOCK;
	int best_start = 0;
	uint32_t gap;

	int i = 1; /* skip metadata slice */
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

static int get_new_sliced_block(struct ouichefs_sb_info *sbi)
{
	struct buffer_head *bh;
	sector_t sliced_block_nr = get_free_block(sbi);

	if (!sliced_block_nr) {
		pr_err("ENOSPC at %s:%d", __FILE_NAME__, __LINE__);
		return -ENOSPC;
	}

	bh = sb_bread(sbi->sb, sliced_block_nr);
	if (!bh) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}
	/* scrub new block */
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

int alloc_slices(struct inode *inode, sector_t *res_bno,
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
		if (!(*bh)) {
			pr_err("Hit EIO at %s:%d", __FILE_NAME__, __LINE__);
			return -EIO;
		}

		s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
		largest_gap = le32_to_cpu(s_block->header.largest_gap);
		if (largest_gap >= nr_slices)
			break;

		*res_bno = le32_to_cpu(s_block->header.next_partial_block);
		brelse(*bh);
	}
	if (*res_bno == 0) {
		pr_info("alloc new sliced block");
		int err = get_new_sliced_block(sbi);
		if (err < 0)
			return err;
		*res_bno = sbi->s_free_sliced_blocks;
		*bh = sb_bread(sb, *res_bno);
		if (!(*bh)) {
			pr_err("Hit EIO at %s:%d", __FILE_NAME__, __LINE__);
			return -EIO;
		}
		s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
		new_block = true;
	}

	unsigned long bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	if (!new_block) {
		*res_slice = find_best_fit(bitmap, nr_slices);

		if (*res_slice <= 0) {
			brelse(*bh);
			pr_err("largest_gap (%u) and bitmap were out of sync, could not find %u free slices",
			       largest_gap, nr_slices);
			return -EFAULT;
		}
	} else {
		*res_slice = 1;
		largest_gap = 0;
	}

	bitmap_clear(&bitmap, *res_slice, nr_slices);

	pr_info("bitmap val: %lu", bitmap);
	if (bitmap == 0) {
		pr_info("block %llu full", *res_bno);
		sbi->s_free_sliced_blocks =
			le32_to_cpu(s_block->header.next_partial_block);
		s_block->header.largest_gap = 0;
	} else {
		largest_gap = 0;
		update_largest_gap(bitmap, &largest_gap);
		s_block->header.largest_gap = cpu_to_le32(largest_gap);
		pr_info("new largest_gap for block %llu is: %u", *res_bno,
			largest_gap);
	}

	s_block->header.slice_bitmap = cpu_to_le32(bitmap);

	return 0;
}

static int file_get_first_slice(struct inode *inode, sector_t *sliced_block_nr,
				uint32_t *slice)
{
	uint32_t index = OUICHEFS_INODE(inode)->index_block;
	if (index == 0)
		return -RETURN_UNALLOCATED;

	*sliced_block_nr = index & GENMASK(26, 0);
	*slice = (index >> 27) & GENMASK(4, 0);

	return 0;
}

int sliced_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		     char **start)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sliced_block *s_block;
	sector_t sliced_block_nr;
	uint32_t slice;

	int err = file_get_first_slice(inode, &sliced_block_nr, &slice);
	if (err)
		return err;

	*bh = sb_bread(sb, sliced_block_nr);
	if (!(*bh)) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

	s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

int write_sliced_get_start(struct inode *inode, loff_t pos, size_t count,
			   struct buffer_head **bh, char **start,
			   sector_t *new_index_block)
{
	pr_info("write_sliced_get_start called");
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
		/* ignore return, cant get UNALLOCATED when size > 0 */
		err = file_get_first_slice(inode, &sliced_block_nr, &slice);
		if (err) {
			pr_err("got UNALLOCATED where it should not be possible");
			goto alloc_new;
		}
	}

	if (!(*bh)) {
		*bh = sb_bread(inode->i_sb, sliced_block_nr);
		if (!(*bh)) {
			pr_err("We hit this EIO at %s:%d", __FILE_NAME__,
			       __LINE__);
			return -EIO;
		}
	}

	// TODO: where to put this?
	/* WARN: block number is 27 bits max */
	*new_index_block = (slice << 27) | sliced_block_nr;

	pr_info("write to block no: %llu, at slice: %u", sliced_block_nr,
		slice);

	s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

int remove_from_partial_list(struct super_block *sb, sector_t block,
			     struct ouichefs_sliced_block *s_block)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_prev;
	struct ouichefs_sliced_block *prev_block;
	sector_t iter_bno;
	int ret = 0;

	iter_bno = sbi->s_free_sliced_blocks;

	if (iter_bno == 0) {
		pr_err("Cant remove: partial list is empty");
		return -ENOENT;
	}

	if (iter_bno == block) {
		sbi->s_free_sliced_blocks =
			le32_to_cpu(s_block->header.next_partial_block);
		return 0;
	}

	bh_prev = sb_bread(sb, iter_bno);
	if (!bh_prev) {
		pr_err("EIO: Cant read head block of partial list");
		return -EIO;
	}

	prev_block = (struct ouichefs_sliced_block *)bh_prev->b_data;
	iter_bno = le32_to_cpu(prev_block->header.next_partial_block);

	while (iter_bno != block && iter_bno != 0) {
		brelse(bh_prev);
		bh_prev = sb_bread(sb, iter_bno);
		if (!bh_prev) {
			pr_err("EIO: Cant read head block of partial list");
			return -EIO;
		}
		prev_block = (struct ouichefs_sliced_block *)bh_prev->b_data;
		iter_bno = le32_to_cpu(prev_block->header.next_partial_block);
	}
	if (iter_bno == 0) {
		pr_err("Block to remove is not in partial list");
		ret = -ENOENT;
	} else {
		prev_block->header.next_partial_block =
			s_block->header.next_partial_block;
		mark_buffer_dirty(bh_prev);
	}
	brelse(bh_prev);
	return ret;
}

static inline void add_to_partial_list(struct ouichefs_sb_info *sbi,
				       struct ouichefs_sliced_block *s_block,
				       sector_t block)
{
	s_block->header.next_partial_block =
		cpu_to_le32(sbi->s_free_sliced_blocks);
	sbi->s_free_sliced_blocks = block;
}

int put_slices(struct super_block *sb, sector_t block, uint32_t starting_slice,
	       uint32_t nr_slices)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("Hit EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

	struct ouichefs_sliced_block *s_block =
		(struct ouichefs_sliced_block *)bh->b_data;
	unsigned long slice_bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	if (slice_bitmap == 0) {
		add_to_partial_list(sbi, s_block, block);
	}

	bitmap_set(&slice_bitmap, starting_slice, nr_slices);
	memset(s_block->slices[starting_slice], 0,
	       OUICHEFS_SLICE_SIZE * nr_slices);

	if (slice_bitmap == GENMASK((OUICHEFS_SLICES_PER_BLOCK - 1), 1)) {
		pr_info("block %llu is empty", block);
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
	pr_debug("%s:%d: freed %u slices, starting with %u in block %llu\n",
		 __func__, __LINE__, nr_slices, starting_slice, block);

	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

int free_sliced_file(struct inode *inode)
{
	sector_t block;
	uint32_t slice;
	int err;

	err = file_get_first_slice(inode, &block, &slice);
	if (err)
		return err;

	uint32_t nr_slices = idiv_ceil(inode->i_size, OUICHEFS_SLICE_SIZE);

	err = put_slices(inode->i_sb, block, slice, nr_slices);

	pr_info("unlink block nr: %llu, and slice %d", block, slice);
	return 0;
}
