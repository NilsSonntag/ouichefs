// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#define UNALLOCATED_BLOCK 1

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ouichefs.h"
#include "bitmap.h"

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
				   struct buffer_head *bh_result, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
		return -EFBIG;

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	/*
	 * Check if iblock is already allocated. If not and create is true,
	 * allocate it. Else, get the physical block number.
	 */
	if (index->blocks[iblock] == 0) {
		if (!create) {
			ret = UNALLOCATED_BLOCK;
			goto brelse_index;
		}
		bno = get_free_block(sbi);
		if (!bno) {
			ret = -ENOSPC;
			goto brelse_index;
		}
		index->blocks[iblock] = cpu_to_le32(bno);
		mark_buffer_dirty(bh_index);
	} else {
		bno = le32_to_cpu(index->blocks[iblock]);
	}

	/* Map the physical block to the given buffer_head */
	map_bh(bh_result, sb, bno);

brelse_index:
	brelse(bh_index);

	return ret;
}

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if (!(wronly || rdwr) || !trunc || !(inode->i_size != 0) ||
	    inode->i_size <= OUICHEFS_SLICE_SIZE)
		return 0;

	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct buffer_head *bh;
	sector_t block = ci->index_block;

	bh = sb_bread(sb, block);

	if (!bh)
		return -EIO;

	struct ouichefs_file_index_block *index =
		(struct ouichefs_file_index_block *)bh->b_data;

	for (sector_t iblock = 0; index->blocks[iblock] != 0; iblock++) {
		put_block(sbi, le32_to_cpu(index->blocks[iblock]));
		index->blocks[iblock] = 0;
	}
	inode->i_blocks = 1;
	inode->i_size = 0;

	mark_buffer_dirty(bh);
	brelse(bh);

	return 0;
}

static ssize_t ouichefs_read(struct file *file, char __user *buf, size_t count,
			     loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh_res;
	struct buffer_head *bh_read;
	loff_t pos = *offset;
	sector_t iblock = pos / OUICHEFS_BLOCK_SIZE;
	ssize_t ret = 0;
	int err = 0;

	struct buffer_head init;
	memset(&init, 0, sizeof(init));
	bh_res = &init;
	err = ouichefs_file_get_block(inode, iblock, bh_res, false);
	if (err == UNALLOCATED_BLOCK) {
		return 0;
	} else if (err < 0) {
		return err;
	}

	// read from disk into buffer_head
	bh_read = sb_bread(sb, bh_res->b_blocknr);
	if (!bh_read)
		return -EIO;

	// copy to user space
	loff_t block_offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_read =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - block_offset));
	if (bytes_to_read == 0)
		goto brelse_read;

	err = copy_to_user(buf, bh_read->b_data + block_offset, bytes_to_read);
	if (err) {
		ret = -EFAULT;
		goto brelse_read;
	}

	*offset += bytes_to_read;
	ret = bytes_to_read;

brelse_read:
	brelse(bh_read);
	return ret;
}

static int get_new_sliced_block(struct ouichefs_sb_info *sbi)
{
	struct buffer_head *bh;
	sector_t sliced_block_nr = get_free_block(sbi);

	if (!sliced_block_nr)
		return -ENOSPC;

	bh = sb_bread(sbi->sb, sliced_block_nr);
	if (!bh)
		return -EIO;

	struct ouichefs_sliced_block *s_block =
		(struct ouichefs_sliced_block *)bh->b_data;

	unsigned long slice_bitmap = GENMASK(31, 1);
	s_block->header.slice_bitmap = cpu_to_le32(slice_bitmap);
	s_block->header.next_partial_block = 0;
	brelse(bh);

	sbi->s_free_sliced_blocks = sliced_block_nr;
	sbi->nr_sliced_blocks++;

	return 0;
}

static ssize_t ouichefs_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	// This part checks if the write will be able to complete and allocates the necessary blocks
	struct inode *inode = file_inode(file);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_write;
	struct ouichefs_sliced_block *s_block;
	loff_t pos = *ppos;
	sector_t sliced_block_nr;
	int slice_to_write;
	uint32_t nr_allocs = 0;
	ssize_t ret = 0;
	int err = 0;

	// TODO: update for 1.8
	if (pos + count > 128)
		return -EFBIG;

	/* Check if the write can be completed (enough space?) */
	if (pos + count > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;

	nr_allocs =
		max(pos + count, (unsigned long long)file->f_inode->i_size) /
		OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	// struct buffer_head *bh_res;
	// sector_t iblock = pos / OUICHEFS_BLOCK_SIZE;
	// struct buffer_head init;
	// memset(&init, 0, sizeof(init));
	// bh_res = &init;
	// err = ouichefs_file_get_block(inode, iblock, bh_res, true);
	// if (err < 0)
	// 	return err;

	/* check if there is already a slice */
	uint32_t index = ci->index_block;
	if (index != 0) {
		slice_to_write = (index >> 27) & GENMASK(4, 0);
		sliced_block_nr = index & GENMASK(26, 0);
		bh_write = sb_bread(sb, sliced_block_nr);
		if (!bh_write)
			return -EIO;

		s_block = (struct ouichefs_sliced_block *)bh_write->b_data;
	} else {
		sliced_block_nr = sbi->s_free_sliced_blocks;
		if (sliced_block_nr == 0) {
			err = get_new_sliced_block(sbi);
			if (err < 0)
				return err;
			sliced_block_nr = sbi->s_free_sliced_blocks;
		}

		bh_write = sb_bread(sb, sliced_block_nr);
		if (!bh_write)
			return -EIO;

		s_block = (struct ouichefs_sliced_block *)bh_write->b_data;
		unsigned long slice_bitmap =
			le32_to_cpu(s_block->header.slice_bitmap);

		slice_to_write =
			get_first_free_bit_and_clear(&slice_bitmap, 32);

		if (slice_to_write < 0) {
			brelse(bh_write);
			return -EFAULT;
		}

		s_block->header.slice_bitmap = cpu_to_le32(slice_bitmap);

		if (slice_bitmap == 0) {
			sbi->s_free_sliced_blocks =
				le32_to_cpu(s_block->header.next_partial_block);
		}
	}
	loff_t offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_write =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - offset));
	if (bytes_to_write == 0) {
		brelse(bh_write);
		return 0;
	}
	if (copy_from_user(s_block->slices[slice_to_write] + offset, buf,
			   bytes_to_write)) {
		brelse(bh_write);
		return -EFAULT;
	}
	mark_buffer_dirty(bh_write);
	sync_dirty_buffer(bh_write);
	brelse(bh_write);

	*ppos += bytes_to_write;
	ret = bytes_to_write;

	// This part updates inode metadata and truncates the file if necessary.

	// uint32_t nr_blocks_old = inode->i_blocks;

	/* Update inode metadata */
	inode->i_size = pos + bytes_to_write;
	ci->index_block = (slice_to_write << 27) |
			  sliced_block_nr; /* has 27 bits at max */

	// inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) /
	// 		   OUICHEFS_BLOCK_SIZE) +
	// 		  1;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);

	// /* If file is smaller than before, free unused blocks */
	// if (nr_blocks_old > inode->i_blocks) {
	// 	int i;
	// 	struct buffer_head *bh_index;
	// 	struct ouichefs_file_index_block *index;
	//
	// 	/* Read index block to remove unused blocks */
	// 	bh_index = sb_bread(sb, ci->index_block);
	// 	if (!bh_index) {
	// 		pr_err("failed truncating '%s'. we just lost %llu blocks\n",
	// 		       file->f_path.dentry->d_name.name,
	// 		       nr_blocks_old - inode->i_blocks);
	// 		return ret;
	// 	}
	// 	index = (struct ouichefs_file_index_block *)bh_index->b_data;
	//
	// 	for (i = inode->i_blocks - 1; i < nr_blocks_old - 1; i++) {
	// 		put_block(OUICHEFS_SB(sb),
	// 			  le32_to_cpu(index->blocks[i]));
	// 		index->blocks[i] = 0;
	// 	}
	// 	mark_buffer_dirty(bh_index);
	// 	sync_dirty_buffer(bh_index);
	// 	brelse(bh_index);
	// }
	return ret;
}

static long ouichefs_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	if (cmd != DUMP_BLOCK)
		return -ENOIOCTLCMD;

	struct inode *inode = file_inode(file);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	sector_t block;
	char *buffer_to_print;
	char *current_slice;
	unsigned int buffer_offset = 0;
	int written;
	int ret = 0;

	if (inode->i_size > 128)
		return -EFBIG;

	block = ci->index_block & GENMASK(26, 0);
	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	buffer_to_print =
		kmalloc(OUICHEFS_BLOCK_SIZE + OUICHEFS_SLICES_PER_BLOCK + 1,
			GFP_KERNEL);
	if (!buffer_to_print) {
		ret = -ENOMEM;
		goto brelse_bh;
	}

	/* print bitmap */
	current_slice = bh->b_data;
	for (int i = 3; i >= 0; --i) {
		written = snprintf(buffer_to_print + buffer_offset, 4, "%02x",
				   current_slice[i]);
		if (written < 0) {
			ret = -EFAULT;
			goto cleanup;
		}
		buffer_offset += written;
	}
	// buffer_to_print[buffer_offset++] = '\n';

	for (unsigned int slice = 1; slice < OUICHEFS_SLICES_PER_BLOCK;
	     ++slice) {
		current_slice = bh->b_data + OUICHEFS_SLICE_SIZE * slice;
		written = snprintf(buffer_to_print + buffer_offset,
				   OUICHEFS_SLICE_SIZE + 2, "%.*s\n",
				   OUICHEFS_SLICE_SIZE, current_slice);
		if (written < 0) {
			ret = -EFAULT;
			goto cleanup;
		}
		buffer_offset += written;
	}
	buffer_to_print[buffer_offset] = '\0';
	if (copy_to_user((void __user *)arg, buffer_to_print,
			 buffer_offset + 1)) {
		ret = -EFAULT;
		goto cleanup;
	}

cleanup:
	kfree(buffer_to_print);
brelse_bh:
	brelse(bh);
	return ret;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
	.unlocked_ioctl = ouichefs_ioctl,
};
