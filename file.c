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
#include "slice.h"
#include "block.h"
#include "bitmap.h"

/*
 * Check if the write can be completed (enough space?) 
 *
 * @param count
 * @param pos
 * @return 0 if enough space, error number otherwise
 */
static int write_check_space(struct inode *inode, size_t count, loff_t pos)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(inode->i_sb);
	uint32_t nr_allocs = 0;

	if (pos + count > OUICHEFS_MAX_FILESIZE) {
		pr_err("ENOSPC at %s:%d", __FILE_NAME__, __LINE__);
		return -ENOSPC;
	}

	nr_allocs = max(pos + count, (unsigned long long)inode->i_size) /
		    OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > inode->i_blocks - 1)
		nr_allocs -= inode->i_blocks - 1;
	else
		nr_allocs = 0;

	if (nr_allocs > sbi->nr_free_blocks) {
		pr_err("ENOSPC at %s:%d", __FILE_NAME__, __LINE__);
		return -ENOSPC;
	}

	return 0;
}

/* 
 * Desc
 */
// static void write_null_terminator(char *start, size_t count, loff_t offset,
// 				  bool large_file)
// {
// 	uint32_t limit = large_file ?
// 				 OUICHEFS_BLOCK_SIZE :
// 				 roundup(offset + count, OUICHEFS_SLICE_SIZE);
// 	uint32_t file_end = (offset + count) % limit;
// 	if (file_end != limit - 1)
// 		*(start + offset + count) = '\0';
// }

/*
 * Desc
 *
 * @return written bytes on success, -ERR otherwise
 */
static ssize_t write_user_data(char *start_at, const char __user *buf,
			       size_t count, loff_t offset)
{
	size_t bytes_to_write =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - offset));

	if (bytes_to_write == 0)
		return 0;

	if (copy_from_user(start_at + offset, buf, bytes_to_write))
		return -EFAULT;

	return bytes_to_write;
}

/*
 * new_index_block works as flag for small file as it is 0 for large files
 */
void update_inode_metadata(struct file *file, loff_t new_size, bool large_file)
{
	struct inode *inode = file_inode(file);

	inode->i_size = new_size;
	inode->i_mtime = inode->i_ctime = current_time(inode);
	if (large_file) {
		shrink_multiblock_file(file);
	}
	mark_inode_dirty(inode);
}

static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	pr_info("call open");

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
	struct buffer_head *bh;
	sector_t block = ci->index_block;

	bh = sb_bread(sb, block);

	pr_info("why are we in open???");
	if (!bh) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

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
	struct buffer_head *bh_read = NULL;
	loff_t pos = *offset;
	char *start;
	int err = 0;

	pr_info("call read");

	bool large_file = is_large_file(inode->i_size);

	if (large_file) {
		err = large_get_start(inode, pos, &bh_read, &start, false);
	} else {
		err = sliced_get_start(inode, pos, &bh_read, &start);
	}
	if (err) {
		if (err == -RETURN_UNALLOCATED) {
			return 0;
		}
		return err;
	}

	pr_info("got start");

	loff_t block_offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_read =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - block_offset));
	// ,
	// 	     (size_t)(inode->i_size - pos));
	if (bytes_to_read == 0) {
		pr_info("bytes 0, because count: %lu, block_offset: %llu, isize: %llu, and pos: %llu",
			count, block_offset, inode->i_size, pos);
		brelse(bh_read);
		return 0;
	}

	pr_info("now copy");

	if (large_file) {
		err = copy_to_user(buf, start + block_offset, bytes_to_read);
	} else {
		err = copy_to_user(buf, start + block_offset, bytes_to_read);
	}
	brelse(bh_read);

	*offset += bytes_to_read;

	pr_info("ende read");
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
	char *buf_for_expansion;
	int err = 0;
	bool expand_sliced_file = false;
	bool large_file;

	pr_info("called write with size: %llu", pos + count);

	// Handle O_APPEND
	if (file->f_flags & O_APPEND)
		pos = inode->i_size;

	err = write_check_space(inode, count, pos);
	if (err)
		return err;

	large_file = is_large_file(pos + count);

	if (large_file) {
		if (!is_large_file(inode->i_size)) {
			err = free_sliced_file(inode);
			if (err && err != -RETURN_UNALLOCATED)
				pr_err("could not free sliced file because %d",
				       err);
		}
		pr_info("write large file");
		err = large_get_start(inode, pos, &bh_write, &start, true);
	} else {
		uint32_t current_slices =
			idiv_ceil(inode->i_size, OUICHEFS_SLICE_SIZE);
		uint32_t needed_slices =
			idiv_ceil(pos + count, OUICHEFS_SLICE_SIZE);

		pr_info("current_slices: %u, needed_slices: %u", current_slices,
			needed_slices);
		if (current_slices > 0 && needed_slices > current_slices) {
			expand_sliced_file = true;
			buf_for_expansion =
				kmalloc(OUICHEFS_SMALL_FILE_SIZE, GFP_KERNEL);
			if (!buf_for_expansion) {
				pr_err("ENOMEM at %s:%d", __FILE_NAME__,
				       __LINE__);
				return -ENOMEM;
			}

			/* get start of old file data */
			err = sliced_get_start(inode, pos, &bh_write, &start);
			if (err) {
				kfree(buf_for_expansion);
				return err;
			}

			memcpy(buf_for_expansion, start, pos);
			brelse(bh_write);
		}

		err = write_sliced_get_start(inode, pos, count, &bh_write,
					     &start, &new_index_block);
	}
	if (err)
		return err;

	if (expand_sliced_file) {
		memcpy(start, buf_for_expansion, pos);
		kfree(buf_for_expansion);
	}

	pr_info("next is write user data");
	offset = pos % OUICHEFS_BLOCK_SIZE;
	written_bytes = write_user_data(start, buf, count, offset);
	if (written_bytes <= 0) {
		brelse(bh_write);
		return written_bytes;
	}
	// write_null_terminator(start, count, offset, large_file);
	pr_info("mark buffer");
	mark_buffer_dirty(bh_write);
	sync_dirty_buffer(bh_write);
	brelse(bh_write);

	*ppos = pos + written_bytes;

	pr_info("update metadata");
	update_inode_metadata(file, pos + written_bytes, large_file);
	if (!large_file) {
		OUICHEFS_INODE(inode)->index_block = new_index_block;
	}

	pr_info("ende");

	return written_bytes;
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

	if (inode->i_size > OUICHEFS_SLICE_SIZE)
		return -EFBIG;

	block = ci->index_block & GENMASK(26, 0);
	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("Hit EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}

	buffer_to_print =
		kmalloc(OUICHEFS_BLOCK_SIZE + OUICHEFS_SLICES_PER_BLOCK + 1,
			GFP_KERNEL);
	if (!buffer_to_print) {
		BUG();
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
