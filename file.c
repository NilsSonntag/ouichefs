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
// static int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
// 				   struct buffer_head *bh_result, int create)
// {
// 	struct super_block *sb = inode->i_sb;
// 	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
// 	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
// 	struct ouichefs_file_index_block *index;
// 	struct buffer_head *bh_index;
// 	int ret = 0, bno;
//
// 	/* If block number exceeds filesize, fail */
// 	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2)
// 		return -EFBIG;
//
// 	/* Read index block from disk */
// 	bh_index = sb_bread(sb, ci->index_block);
// 	if (!bh_index)
// 		return -EIO;
// 	index = (struct ouichefs_file_index_block *)bh_index->b_data;
//
// 	/*
// 	 * Check if iblock is already allocated. If not and create is true,
// 	 * allocate it. Else, get the physical block number.
// 	 */
// 	if (index->blocks[iblock] == 0) {
// 		if (!create) {
// 			ret = 0;
// 			goto brelse_index;
// 		}
// 		bno = get_free_block(sbi);
// 		if (!bno) {
// 			ret = -ENOSPC;
// 			goto brelse_index;
// 		}
// 		index->blocks[iblock] = cpu_to_le32(bno);
// 		mark_buffer_dirty(bh_index);
// 	} else {
// 		bno = le32_to_cpu(index->blocks[iblock]);
// 	}
//
// 	/* Map the physical block to the given buffer_head */
// 	map_bh(bh_result, sb, bno);
//
// brelse_index:
// 	brelse(bh_index);
//
// 	return ret;
// }

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

int get_physical_block(struct inode *inode, sector_t iblock,
		       sector_t *phys_block, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	sector_t block;
	int ret = 0, bno;

	// get index block
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index)
		return -EIO;
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	// get block
	block = index->blocks[iblock];
	if (block == 0) {
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
		bno = le32_to_cpu(block);
	}

	*phys_block = bno;

brelse_index:
	brelse(bh_index);
	return ret;
}

static ssize_t ouichefs_read(struct file *file, char __user *buf, size_t count,
			     loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	loff_t pos = *offset;
	sector_t iblock = pos / OUICHEFS_BLOCK_SIZE;
	sector_t phys_block_to_read;
	ssize_t ret, err = 0;

	pr_info("pos: %llu", pos);
	pr_info("iblock: %llu", iblock);

	err = get_physical_block(inode, iblock, &phys_block_to_read, false);
	if (err == UNALLOCATED_BLOCK) {
		return 0;
	} else if (err < 0) {
		return err;
	}

	// read from disk into buffer_head
	struct buffer_head *bh_read;
	bh_read = sb_bread(sb, phys_block_to_read);
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
		ret = err;
		goto brelse_read;
	}

	*offset += bytes_to_read;
	ret = bytes_to_read;

brelse_read:
	brelse(bh_read);
	pr_info("return: %lu", ret);
	return ret;
}

{
	uint32_t nr_allocs = 0;

	/* Check if the write can be completed (enough space?) */
		return -ENOSPC;
	if (nr_allocs > file->f_inode->i_blocks - 1)
		nr_allocs -= file->f_inode->i_blocks - 1;
	else
		nr_allocs = 0;
	if (nr_allocs > sbi->nr_free_blocks)
		return -ENOSPC;



	}



		struct buffer_head *bh_index;

		bh_index = sb_bread(sb, ci->index_block);
		index = (struct ouichefs_file_index_block *)bh_index->b_data;

		}
		mark_buffer_dirty(bh_index);
		brelse(bh_index);
	}
}

const struct file_operations ouichefs_file_ops = {
	.owner = THIS_MODULE,
	.open = ouichefs_open,
	.llseek = generic_file_llseek,
	.read = ouichefs_read,
	.write = ouichefs_write,
	.fsync = generic_file_fsync,
};
