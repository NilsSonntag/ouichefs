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

	if ((wronly || rdwr) && trunc && (inode->i_size != 0)) {
		struct super_block *sb = inode->i_sb;
		struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
		struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
		struct ouichefs_file_index_block *index;
		struct buffer_head *bh_index;
		sector_t iblock;

		/* Read index block from disk */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index)
			return -EIO;
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		for (iblock = 0; index->blocks[iblock] != 0; iblock++) {
			put_block(sbi, le32_to_cpu(index->blocks[iblock]));
			index->blocks[iblock] = 0;
		}
		inode->i_size = 0;
		inode->i_blocks = 1;

		mark_buffer_dirty(bh_index);
		brelse(bh_index);
	}

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
	ssize_t ret, err = 0;

	pr_info("pos: %llu", pos);
	pr_info("iblock: %llu", iblock);

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
	pr_info("count: %lu", count);
	pr_info("block offset: %lld", block_offset);
	pr_info("bytes_to_read: %lu", bytes_to_read);
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
	pr_info("return: %lu", ret);
	return ret;
}

static ssize_t ouichefs_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *offset)
{
	// This part checks if the write will be able to complete and allocates the necessary blocks through block_write_begin().
	struct inode *inode = file_inode(file);
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	loff_t pos = *offset;
	sector_t iblock = pos / OUICHEFS_BLOCK_SIZE;
	uint32_t nr_allocs = 0;
	// sector_t phys_block_to_write;
	struct buffer_head *bh_write;
	struct buffer_head *bh_res;
	struct super_block *sb = inode->i_sb;
	int ret, err = 0;

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

	// new write process
	struct buffer_head init;
	memset(&init, 0, sizeof(init));
	bh_res = &init;
	err = ouichefs_file_get_block(inode, iblock, bh_res, true);
	if (err < 0)
		return err;

	bh_write = sb_bread(sb, bh_res->b_blocknr);

	loff_t block_offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_write =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - block_offset));
	pr_info("count: %lu", count);
	pr_info("block offset: %lld", block_offset);
	pr_info("bytes_to_write: %lu", bytes_to_write);
	if (bytes_to_write == 0) {
		brelse(bh_write);
		goto end;
	}
	if (copy_from_user(bh_write->b_data + block_offset, buf,
			   bytes_to_write)) {
		brelse(bh_write);
		ret = -EFAULT;
		goto end;
	}
	mark_buffer_dirty(bh_write);
	sync_dirty_buffer(bh_write);
	brelse(bh_write);

	*offset += bytes_to_write;
	ret = bytes_to_write;

	// This part updates inode metadata and truncates the file if necessary.

	uint32_t nr_blocks_old = inode->i_blocks;

	/* Update inode metadata */
	inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) /
			   OUICHEFS_BLOCK_SIZE) +
			  1;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_size = pos + bytes_to_write;
	mark_inode_dirty(inode);

	/* If file is smaller than before, free unused blocks */
	if (nr_blocks_old > inode->i_blocks) {
		int i;
		struct buffer_head *bh_index;
		struct ouichefs_file_index_block *index;

		/* Read index block to remove unused blocks */
		bh_index = sb_bread(sb, ci->index_block);
		if (!bh_index) {
			pr_err("failed truncating '%s'. we just lost %llu blocks\n",
			       file->f_path.dentry->d_name.name,
			       nr_blocks_old - inode->i_blocks);
			goto end;
		}
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		for (i = inode->i_blocks - 1; i < nr_blocks_old - 1; i++) {
			put_block(OUICHEFS_SB(sb),
				  le32_to_cpu(index->blocks[i]));
			index->blocks[i] = 0;
		}
		mark_buffer_dirty(bh_index);
		sync_dirty_buffer(bh_index);
		brelse(bh_index);
	}
end:
	return ret;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
};
