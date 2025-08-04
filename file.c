// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */

#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#define UNALLOCATED 1 // TODO: change to another value

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
				   sector_t *block, int create)
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
	pr_info("block nr: %u at file %s:%d", ci->index_block, __FILE_NAME__,
		__LINE__);
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
			ret = UNALLOCATED;
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

	*block = bno;

	pr_info("finished func without error");
brelse_index:
	brelse(bh_index);

	return ret;
}

static int ouichefs_get_buffer_head(struct inode *inode, loff_t pos,
				    struct buffer_head **bh_res, u32 *slice)
{
	sector_t iblock;
	sector_t block;

	if (inode->i_size > OUICHEFS_SMALL_FILE_SIZE) {
		/* find file data block for large file */
		iblock = pos / OUICHEFS_BLOCK_SIZE;
		int err = ouichefs_file_get_block(inode, iblock, &block, false);
		if (err)
			return err;

		*slice = 0;
	} else {
		/* find slice and block for small file */
		sector_t index = OUICHEFS_INODE(inode)->index_block;
		if (index == 0)
			return UNALLOCATED;

		*slice = (index >> 27) & GENMASK(4, 0);
		block = index & GENMASK(26, 0);
	}

	pr_info("block nr: %llu at file %s:%d", block, __FILE_NAME__, __LINE__);
	*bh_res = sb_bread(inode->i_sb, block);
	if (!*bh_res)
		return -EIO;

	return 0;
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
	s_block->header.next_partial_block = sbi->s_free_sliced_blocks;
	brelse(bh);

	sbi->s_free_sliced_blocks = sliced_block_nr;
	sbi->nr_sliced_blocks++;

	return 0;
}

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

	if (pos + count > OUICHEFS_MAX_FILESIZE)
		return -ENOSPC;

	nr_allocs = max(pos + count, (unsigned long long)inode->i_size) /
		    OUICHEFS_BLOCK_SIZE;
	if (nr_allocs > inode->i_blocks - 1)
		nr_allocs -= inode->i_blocks - 1;
	else
		nr_allocs = 0;

	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;

	return 0;
}

/*
 * If file is smaller than before, free unused blocks
 */
static void write_free_empty_blocks(struct file *file,
				    unsigned int nr_blocks_old)
{
	struct inode *inode = file_inode(file);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	int i;

	if (nr_blocks_old <= inode->i_blocks)
		return;

	/* Read index block to remove unused blocks */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index) {
		pr_err("failed truncating '%s'. we just lost %llu blocks\n",
		       file->f_path.dentry->d_name.name,
		       nr_blocks_old - inode->i_blocks);
		return;
	}
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	for (i = inode->i_blocks - 1; i < nr_blocks_old - 1; i++) {
		put_block(OUICHEFS_SB(sb), le32_to_cpu(index->blocks[i]));
		index->blocks[i] = 0;
	}
	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);
	brelse(bh_index);
}

static int write_large_get_start(struct inode *inode, loff_t pos,
				 struct buffer_head **bh_write, char **start)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	sector_t block;
	unsigned int iblock;
	int err;

	if (ci->index_block == 0) {
		sector_t bno = get_free_block(sbi);
		if (!bno)
			return -ENOSPC;

		struct buffer_head *bh = sb_bread(sb, bno);
		if (!bh)
			return -EIO;

		char *fblock = (char *)bh->b_data;
		memset(fblock, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		brelse(bh);

		ci->index_block = bno;
	}

	iblock = pos / OUICHEFS_BLOCK_SIZE;
	err = ouichefs_file_get_block(inode, iblock, &block, true);
	if (err)
		return err;

	pr_info("block nr: %llu at file %s:%d", block, __FILE_NAME__, __LINE__);
	*bh_write = sb_bread(sb, block);
	if (!(*bh_write)) {
		pr_info("cant open the new alloc block");
		return -EIO;
	}
	*start = (*bh_write)->b_data;
	return 0;
}

/* 
 * Desc
 */
static void write_null_terminator(char *start, size_t count, loff_t offset,
				  unsigned int storage_size)
{
	unsigned int file_end = (offset + count) % storage_size;
	if (file_end != storage_size - 1)
		*(start + offset + count) = '\0';
}

/*
 * Desc
 *
 * @return written bytes on success, -ERR otherwise
 */
static ssize_t write_data(char *start_at, const char *buf, size_t count,
			  loff_t offset)
{
	size_t bytes_to_write =
		min(count, (size_t)(OUICHEFS_BLOCK_SIZE - offset));

	if (bytes_to_write == 0)
		return 0;

	if (copy_from_user(start_at + offset, buf, bytes_to_write))
		return -EFAULT;

	return bytes_to_write;
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
	if (!bh)
		return -EIO;

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

	return 0;
}

static int ouichefs_file_get_slice(struct inode *inode,
				   sector_t *sliced_block_nr,
				   unsigned int *slice, bool create)
{
	uint32_t index = OUICHEFS_INODE(inode)->index_block;
	if (index == 0) {
		if (!create)
			return UNALLOCATED;

		return get_new_slice(inode, sliced_block_nr, slice);
	}
	*sliced_block_nr = index & GENMASK(26, 0);
	*slice = (index >> 27) & GENMASK(4, 0);

	return 0;
}

static int write_small_get_start(struct inode *inode, loff_t pos,
				 struct buffer_head **bh_write, char **start,
				 sector_t *new_index_block)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sliced_block *s_block;
	sector_t sliced_block_nr;
	unsigned int slice;

	int err =
		ouichefs_file_get_slice(inode, &sliced_block_nr, &slice, true);
	if (err)
		return err;

	/* WARN: block number is 27 bits max */
	*new_index_block = (slice << 27) | sliced_block_nr;
	pr_info("write to block nr: %llu, and slice %u", sliced_block_nr,
		slice);

	*bh_write = sb_bread(sb, sliced_block_nr);
	if (!(*bh_write))
		return -EIO;

	s_block = (struct ouichefs_sliced_block *)(*bh_write)->b_data;
	*start = s_block->slices[slice];

	return 0;
}

/*
 * new_index_block works as flag for small file as it is 0 for large files
 */
void update_inode_metadata(struct file *file, loff_t new_size, bool large_file)
{
	struct inode *inode = file_inode(file);

	inode->i_size = new_size;
	pr_info("isize after write: %lld", inode->i_size);
	inode->i_mtime = inode->i_ctime = current_time(inode);
	if (large_file) {
		unsigned int nr_blocks_old = inode->i_blocks;
		inode->i_blocks = (roundup(inode->i_size, OUICHEFS_BLOCK_SIZE) /
				   OUICHEFS_BLOCK_SIZE) +
				  1;
		write_free_empty_blocks(file, nr_blocks_old);
	}
	mark_inode_dirty(inode);
}

// TODO: 1.10
static int ouichefs_open(struct inode *inode, struct file *file)
{
	bool wronly = (file->f_flags & O_WRONLY) != 0;
	bool rdwr = (file->f_flags & O_RDWR) != 0;
	bool trunc = (file->f_flags & O_TRUNC) != 0;

	if (!(wronly || rdwr) || !trunc || inode->i_size == 0 ||
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
	struct buffer_head *bh_read = NULL;
	loff_t pos = *offset;
	unsigned int slice = 0;
	ssize_t ret = 0;
	int err = 0;

	err = ouichefs_get_buffer_head(inode, pos, &bh_read, &slice);
	if (err == UNALLOCATED)
		return 0;
	if (err < 0)
		return err;

	loff_t block_offset = pos % OUICHEFS_BLOCK_SIZE;
	size_t bytes_to_read =
		min3(count, (size_t)(OUICHEFS_BLOCK_SIZE - block_offset),
		     (size_t)(inode->i_size - pos));
	if (bytes_to_read == 0)
		goto brelse_read;

	if (slice == 0) {
		err = copy_to_user(buf, bh_read->b_data + block_offset,
				   bytes_to_read);
	} else {
		struct ouichefs_sliced_block *s_block =
			(struct ouichefs_sliced_block *)bh_read->b_data;
		err = copy_to_user(buf, s_block->slices[slice] + block_offset,
				   bytes_to_read);
	}
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
	int err = 0;
	bool large_file;

	err = write_check_space(inode, count, pos);
	if (err)
		return err;

	/* set large_file flag */
	large_file = pos + count > OUICHEFS_BLOCK_SIZE - OUICHEFS_SLICE_SIZE;

	if (large_file) {
		err = write_large_get_start(inode, pos, &bh_write, &start);
	} else {
		err = write_small_get_start(inode, pos, &bh_write, &start,
					    &new_index_block);
	}
	if (err)
		return err;

	offset = pos % OUICHEFS_BLOCK_SIZE;
	written_bytes = write_data(start, buf, count, offset);
	if (written_bytes <= 0) {
		brelse(bh_write);
		return written_bytes;
	}
	write_null_terminator(start, count, offset,
			      large_file ? OUICHEFS_BLOCK_SIZE :
					   OUICHEFS_SLICE_SIZE);
	mark_buffer_dirty(bh_write);
	sync_dirty_buffer(bh_write);
	brelse(bh_write);

	*ppos += written_bytes;

	update_inode_metadata(file, pos + written_bytes, large_file);
	if (!large_file) {
		OUICHEFS_INODE(inode)->index_block = new_index_block;
	}

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
