#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

#include "ouichefs.h"
#include "slice.h"
#include "bitmap.h"

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

static int get_new_slice(struct inode *inode, sector_t *sliced_block_nr,
			 unsigned int *slice)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh;
	struct ouichefs_sliced_block *s_block;

	*sliced_block_nr = sbi->s_free_sliced_blocks;
	if (*sliced_block_nr == 0) {
		pr_info("alloc new sliced block");
		int err = get_new_sliced_block(sbi);
		if (err < 0)
			return err;
		*sliced_block_nr = sbi->s_free_sliced_blocks;
	}

	bh = sb_bread(sb, *sliced_block_nr);
	if (!bh) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

	s_block = (struct ouichefs_sliced_block *)bh->b_data;
	unsigned long bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	*slice = get_first_free_bit_and_clear(&bitmap, 32);

	if (*slice < 0) {
		brelse(bh);
		return -EFAULT;
	}

	s_block->header.slice_bitmap = cpu_to_le32(bitmap);

	if (bitmap == 0) {
		sbi->s_free_sliced_blocks =
			le32_to_cpu(s_block->header.next_partial_block);
	}

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

static int ouichefs_file_get_slice(struct inode *inode,
				   sector_t *sliced_block_nr, uint32_t *slice,
				   bool create)
{
	uint32_t index = OUICHEFS_INODE(inode)->index_block;
	if (index == 0) {
		pr_info("index 0 gg");
		if (!create)
			return -RETURN_UNALLOCATED;

		return get_new_slice(inode, sliced_block_nr, slice);
	}
	*sliced_block_nr = index & GENMASK(26, 0);
	*slice = (index >> 27) & GENMASK(4, 0);

	return 0;
}

int write_sliced_get_start(struct inode *inode, loff_t pos,
			   struct buffer_head **bh, char **start,
			   sector_t *new_index_block, bool create)
{
	pr_info("sliced_get_start called");
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sliced_block *s_block;
	sector_t sliced_block_nr;
	uint32_t slice;

	int err = ouichefs_file_get_slice(inode, &sliced_block_nr, &slice,
					  create);
	if (err)
		return err;

	// TODO: where to put this?
	/* WARN: block number is 27 bits max */
	*new_index_block = (slice << 27) | sliced_block_nr;

	pr_info("write to block no: %llu, at slice: %u", sliced_block_nr,
		slice);
	*bh = sb_bread(sb, sliced_block_nr);
	if (!(*bh)) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

	s_block = (struct ouichefs_sliced_block *)(*bh)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

// TODO: this is definitly bad, but where should we put new index block
int sliced_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		     char **start, bool create)
{
	sector_t index_block;
	return write_sliced_get_start(inode, pos, bh, start, &index_block,
				      create);
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
		pr_err("Cant read head block of partial list");
		return -EIO;
	}

	prev_block = (struct ouichefs_sliced_block *)bh_prev->b_data;
	iter_bno = le32_to_cpu(prev_block->header.next_partial_block);

	while (iter_bno != block && iter_bno != 0) {
		brelse(bh_prev);
		bh_prev = sb_bread(sb, iter_bno);
		if (!bh_prev) {
			pr_err("Cant read head block of partial list");
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
	if (!bh)
		return -EIO;

	struct ouichefs_sliced_block *s_block =
		(struct ouichefs_sliced_block *)bh->b_data;
	unsigned long slice_bitmap = le32_to_cpu(s_block->header.slice_bitmap);

	if (slice_bitmap == 0) {
		add_to_partial_list(sbi, s_block, block);
	}

	bitmap_set(&slice_bitmap, starting_slice, nr_slices);
	memset(s_block->slices[starting_slice], 0, OUICHEFS_SLICE_SIZE);

	if (slice_bitmap == GENMASK((OUICHEFS_SLICES_PER_BLOCK - 1), 1)) {
		pr_debug("block %llu is empty", block);
		memset(&s_block->header, 0, OUICHEFS_SLICE_SIZE);
		int err = remove_from_partial_list(sb, block, s_block);
		if (err)
			return err;

		sbi->nr_sliced_blocks--;
		put_block(sbi, block);
	} else {
		s_block->header.slice_bitmap = cpu_to_le32(slice_bitmap);
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

	err = ouichefs_file_get_slice(inode, &block, &slice, false);
	if (err)
		return err;

	uint32_t nr_slices = idiv_ceil(inode->i_size, OUICHEFS_SLICE_SIZE);

	err = put_slices(inode->i_sb, block, slice, nr_slices);

	pr_info("unlink block nr: %llu, and slice %d", block, slice);
	return 0;
}
