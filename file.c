// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>

#include "ouichefs.h"
#include "bitmap.h"

/**
 * check_write_space - Ensure enough space for write.
 */
static int check_write_space(struct inode *inode, size_t count, loff_t pos)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	uint32_t nr_allocs;

	if (pos + count > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;

	nr_allocs = max(pos + count, (unsigned long long)inode->i_size) /
		    OUICHEFS_BLOCK_SIZE;
	nr_allocs = (nr_allocs > inode->i_blocks - 1) ?
			    nr_allocs - (inode->i_blocks - 1) :
			    0;

	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	return 0;
}

/**
 * ouichefs_copy_from_user - Copy user data to kernel buffer.
 */
static ssize_t ouichefs_copy_from_user(char *dst, const char __user *src,
				       size_t count, loff_t offset)
{
	size_t n = min(count, (size_t)(OUICHEFS_BLOCK_SIZE - offset));
	if (!n)
		return 0;
	if (copy_from_user(dst + offset, src, n))
		return -EFAULT;

	return n;
}

/**
 * update_inode_metadata - Update inode size and timestamps.
 */
static void update_inode_metadata(struct file *file, loff_t new_size)
{
	struct inode *inode = file_inode(file);

	inode->i_size = new_size;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_blocks = nr_necessary_blocks(new_size);
	mark_inode_dirty(inode);
}

/* File Operations */

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if (!(wronly || rdwr) || !trunc || inode->i_size == 0)
		return 0;

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

	return 0;
}

static ssize_t ouichefs_read(struct file *file, char __user *buf, size_t count,
			     loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct buffer_head *bh_read = NULL;
	loff_t pos = *offset;
	char *start;
	int err;

	err = large_get_start(inode, pos, &bh_read, &start, false);

	if (err) {
		/* no error if there is no data in a file */
		if (err == -RETURN_UNALLOCATED)
			return 0;
		return err;
	}

	loff_t block_offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_read =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - block_offset));

	if (!bytes_to_read) {
		brelse(bh_read);
		return 0;
	}

	if (copy_to_user(buf, start + block_offset, bytes_to_read)) {
		brelse(bh_read);
		return -EFAULT;
	}
	brelse(bh_read);

	*offset += bytes_to_read;
	return bytes_to_read;
}

static ssize_t ouichefs_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct buffer_head *bh_write;
	loff_t pos = *ppos;
	loff_t offset;
	ssize_t written_bytes;
	char *start;
	int err;

	if (file->f_flags & O_APPEND)
		pos = inode->i_size;

	err = check_write_space(inode, count, pos);
	if (err)
		return err;

	err = large_get_start(inode, pos, &bh_write, &start, true);
	if (err)
		return err;

	offset = pos % OUICHEFS_BLOCK_SIZE;
	written_bytes = ouichefs_copy_from_user(start, buf, count, offset);
	if (written_bytes <= 0) {
		brelse(bh_write);
		return written_bytes;
	}

	mark_buffer_dirty(bh_write);
	sync_dirty_buffer(bh_write);
	brelse(bh_write);

	shrink_multiblock_file(file);
	update_inode_metadata(file, pos + written_bytes);

	*ppos = pos + written_bytes;
	return written_bytes;
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
};
