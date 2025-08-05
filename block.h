#ifndef _BLOCK_H
#define _BLOCK_H

#include "ouichefs.h"

int ouichefs_file_get_block(struct inode *inode, sector_t iblock,
			    sector_t *block, int create);

int large_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		    char **start, bool create);

void shrink_multiblock_file(struct file *file);

#endif /* _BLOCK_H */
