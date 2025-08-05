#ifndef _SLICE_H
#define _SLICE_H

#include "linux/buffer_head.h"
#include "ouichefs.h"

int write_sliced_get_start(struct inode *inode, loff_t pos,
			   struct buffer_head **bh, char **start,
			   sector_t *new_index_block, bool create);

int sliced_get_start(struct inode *inode, loff_t pos, struct buffer_head **bh,
		     char **start, bool create);

int put_slices(struct super_block *sb, sector_t block, uint32_t starting_slice,
	       uint32_t nr_slices);

int free_sliced_file(struct inode *inode);

#endif /* _SLICE_H */
