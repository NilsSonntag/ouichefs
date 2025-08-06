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
 * update_inode_metadata - Update inode size and timestamps. Also i_blocks for large files.
 */
static void update_inode_metadata(struct file *file, loff_t new_size,
				  bool large_file)
{
	struct inode *inode = file_inode(file);

	inode->i_size = new_size;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	if (large_file)
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

	if (!is_large_file(inode->i_size)) {
		int err = free_sliced_file(inode);

		if (err)
			return err;
		inode->i_size = 0;
		return 0;
	}

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
	bool large_file = is_large_file(inode->i_size);

	if (large_file)
		err = large_get_start(inode, pos, &bh_read, &start, false);
	else
		err = read_sliced_get_start(inode, pos, &bh_read, &start);

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
	sector_t new_index_block = 0;
	loff_t pos = *ppos;
	loff_t offset;
	ssize_t written_bytes;
	char *start;
	int err;
	bool large_file;

	if (file->f_flags & O_APPEND)
		pos = inode->i_size;

	err = check_write_space(inode, count, pos);
	if (err)
		return err;

	large_file = is_large_file(pos + count);

	if (large_file) {
		if (!is_large_file(inode->i_size)) {
			/* Regain space if file was sliced before, do not fail on error as space loss is not critical */
			err = free_sliced_file(inode);
			if (err && err != -RETURN_UNALLOCATED)
				pr_err("failed freeing to extend '%s'. we just lost a slice\n",
				       file->f_path.dentry->d_name.name);
		}

		err = large_get_start(inode, pos, &bh_write, &start, true);
	} else {
		err = write_sliced_get_start(inode, pos, count, &bh_write,
					     &start, &new_index_block);
	}
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

	if (large_file) {
		shrink_multiblock_file(file);
	} else {
		OUICHEFS_INODE(inode)->index_block = new_index_block;
	}
	update_inode_metadata(file, pos + written_bytes, large_file);

	*ppos = pos + written_bytes;
	return written_bytes;
}

/* Only supports DUMP_BLOCK */
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

	if (inode->i_size > OUICHEFS_SMALL_FILE_SIZE)
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
		goto out_brelse;
	}

	/* First slice is bitmap, print as hex */
	current_slice = bh->b_data;
	for (int i = 3; i >= 0; --i) {
		written = snprintf(buffer_to_print + buffer_offset, 4, "%02x",
				   current_slice[i]);
		if (written < 0) {
			ret = -EFAULT;
			goto out_free;
		}
		buffer_offset += written;
	}

	for (unsigned int slice = 1; slice < OUICHEFS_SLICES_PER_BLOCK;
	     ++slice) {
		current_slice = bh->b_data + OUICHEFS_SLICE_SIZE * slice;
		written = snprintf(buffer_to_print + buffer_offset,
				   OUICHEFS_SLICE_SIZE + 2, "%.*s\n",
				   OUICHEFS_SLICE_SIZE, current_slice);
		if (written < 0) {
			ret = -EFAULT;
			goto out_free;
		}
		buffer_offset += written;
	}
	buffer_to_print[buffer_offset] = '\0';
	if (copy_to_user((void __user *)arg, buffer_to_print,
			 buffer_offset + 1)) {
		ret = -EFAULT;
		goto out_free;
	}

out_free:
	kfree(buffer_to_print);
out_brelse:
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
