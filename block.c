#include "ouichefs.h"
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>

#include "block.h"
#include "bitmap.h"

/*
 * Map the buffer_head passed in argument with the iblock-th block of the file
 * represented by inode. If the requested block is not allocated and create is
 * true, allocate a new block on disk and map it.
 */
int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
			    sector_t *block, int create)
{
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct ouichefs_file_index_block *index;
	struct buffer_head *bh_index;
	int ret = 0, bno;

	/* If block number exceeds filesize, fail */
	if (iblock >= OUICHEFS_BLOCK_SIZE >> 2) {
		pr_err("EFBIG at %s:%d", __FILE_NAME__, __LINE__);
		return -EFBIG;
	}

	/* Read index block from disk */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index) {
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}
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

int large_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		    char **start, bool create)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	sector_t block;
	unsigned int iblock;
	int err;

	if (ci->index_block == 0 || !is_large_file(inode->i_size)) {
		if (!create)
			return -RETURN_UNALLOCATED;

		sector_t bno = get_free_block(sbi);
		if (!bno) {
			pr_err("ENOSPC at %s:%d", __FILE_NAME__, __LINE__);
			return -ENOSPC;
		}

		struct buffer_head *bh = sb_bread(sb, bno);
		if (!bh) {
			pr_err("We hit this EIO at %s:%d", __FILE_NAME__,
			       __LINE__);
			return -EIO;
		}

		// HACK:? put bh->b_data direct in memset
		// char *fblock = (char *)bh->b_data;
		memset((char *)bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		brelse(bh);

		ci->index_block = bno;
	}

	iblock = pos / OUICHEFS_BLOCK_SIZE;
	err = ouichefs_file_get_block(inode, iblock, &block, true);
	if (err)
		return err;

	*bh = sb_bread(sb, block);
	if (!(*bh)) {
		pr_info("cant open the new alloc block");
		pr_err("We hit this EIO at %s:%d", __FILE_NAME__, __LINE__);
		return -EIO;
	}
	*start = (*bh)->b_data;
	return 0;
}

/*
 * If file is smaller than before, free unused blocks
 * Uses inode->i_blocks so has to be called before updating inode metadata
 */
void shrink_multiblock_file(struct file *file)
{
	struct inode *inode = file_inode(file);
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh_index;
	struct ouichefs_file_index_block *index;
	int i;

	uint32_t old_nr_blocks = inode->i_blocks;
	uint32_t new_nr_blocks = nr_necessary_blocks(inode->i_size);

	if (!new_nr_blocks)
		pr_info("iblocks is 0");

	if (old_nr_blocks <= new_nr_blocks)
		return;

	/* Read index block to remove unused blocks */
	bh_index = sb_bread(sb, ci->index_block);
	if (!bh_index) {
		pr_err("failed truncating '%s'. we just lost %llu blocks\n",
		       file->f_path.dentry->d_name.name,
		       old_nr_blocks - new_nr_blocks);
		return;
	}
	index = (struct ouichefs_file_index_block *)bh_index->b_data;

	for (i = new_nr_blocks - 1; i < old_nr_blocks - 1; i++) {
		uint32_t bno = le32_to_cpu(index->blocks[i]);
		if (bno) {
			put_block(OUICHEFS_SB(sb), bno);
		}
		index->blocks[i] = 0;
	}
	mark_buffer_dirty(bh_index);
	sync_dirty_buffer(bh_index);
	brelse(bh_index);
}
