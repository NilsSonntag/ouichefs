/* SPDX-License-Identifier: GPL-2.0 */
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

#include "ouichefs.h"
#include "bitmap.h"

/**
 * ouichefs_file_get_block - Get the physical block number for a file block.
 * @inode: The inode of the file.
 * @iblock: The logical block number (index) in the file.
 * @block: Pointer to store the physical block number.
 * @create: If true, allocate a new block if the logical block is not allocated.
 *
 * Return: 0 on success, -RETURN_UNALLOCATED if the logical block is not allocated and should not be created,
 *         -ENOSPC if no free blocks are available, or -EIO on I/O error.
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
			ret = -RETURN_UNALLOCATED;
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

brelse_index:
	brelse(bh_index);
	return ret;
}

/* Creates and scrubs index block for given file */
static int create_index_block(struct inode *inode)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	sector_t bno;
	struct buffer_head *bh;

	bno = get_free_block(sbi);
	if (!bno)
		return -ENOSPC;

	bh = sb_bread(sb, bno);
	if (!bh)
		return -EIO;

	memset((char *)bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	brelse(bh);

	ci->index_block = bno;
	return 0;
}

/**
 * large_get_start - Get the start of a large file block.
 * @inode: The inode of the file.
 * @pos: The position in the file.
 * @bh: Pointer to store the buffer head for the block.
 * @start: Pointer to store the start address of the block data.
 * @create: If true, create a new block if necessary.
 *
 * Return: 0 on success, -RETURN_UNALLOCATED if the block is not allocated and should not be created,
 *         -ENOSPC if no free blocks are available, or -EIO on I/O error.
 */
int large_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		    char **start, bool create)
{
	sector_t block;
	uint32_t iblock;
	int err;

	/* Check for existing index block, otherwise return UNALLOCATED or create one */
	if (!OUICHEFS_INODE(inode)->index_block) {
		if (!create)
			return -RETURN_UNALLOCATED;

		err = create_index_block(inode);
		if (err)
			return err;
	}

	iblock = pos / OUICHEFS_BLOCK_SIZE;
	err = ouichefs_file_get_block(inode, iblock, &block, true);
	if (err)
		return err;

	*bh = sb_bread(inode->i_sb, block);
	if (!(*bh)) {
		pr_err("Something went wrong, can not open the freshly allocated block");
		return -EIO;
	}
	*start = (*bh)->b_data;
	return 0;
}

/**
 * shrink_multiblock_file - Shrink a file by removing unused blocks.
 * @file: The file to shrink.
 *
 * This function reduces the number of blocks allocated for a file to the minimum
 * necessary based on its size, freeing any excess blocks. The given file must be
 * spanning across multiple blocks for this function to be effective.
 */
void shrink_multiblock_file(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	int i;

	uint32_t old_nr_blocks = inode->i_blocks;
	uint32_t new_nr_blocks = nr_necessary_blocks(inode->i_size);

	if (!new_nr_blocks || old_nr_blocks <= new_nr_blocks)
		return;

	bh_index = sb_bread(sb, OUICHEFS_INODE(inode)->index_block);
	if (!bh_index) {
		pr_err("failed truncating '%s'. we just lost %u blocks\n",
		       file->f_path.dentry->d_name.name,
		       old_nr_blocks - new_nr_blocks);
		return;
	}
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	for (i = new_nr_blocks - 1; i < old_nr_blocks - 1; i++) {
		uint32_t bno = le32_to_cpu(index->blocks[i]);
		if (bno)
			put_block(OUICHEFS_SB(sb), bno);
		index->blocks[i] = 0;
	}
	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);
	brelse(bh_index);
}
