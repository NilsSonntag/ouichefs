/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#ifndef _OUICHEFS_H
#define _OUICHEFS_H

#include <linux/fs.h>

#define OUICHEFS_MAGIC 0x48434957

#define OUICHEFS_SB_BLOCK_NR 0

#define OUICHEFS_BLOCK_SIZE (1 << 12) /* 4 KiB */
#define OUICHEFS_MAX_FILESIZE (1 << 22) /* 4 MiB */
#define OUICHEFS_FILENAME_LEN 28
#define OUICHEFS_MAX_SUBFILES 128

#define OUICHEFS_SLICES_PER_BLOCK 32
#define OUICHEFS_SLICE_SIZE (OUICHEFS_BLOCK_SIZE / OUICHEFS_SLICES_PER_BLOCK)

/*
 * ouiche_fs partition layout
 *
 * +---------------+
 * |  superblock   |  1 block
 * +---------------+
 * |  inode store  |  sb->nr_istore_blocks blocks
 * +---------------+
 * | ifree bitmap  |  sb->nr_ifree_blocks blocks
 * +---------------+
 * | bfree bitmap  |  sb->nr_bfree_blocks blocks
 * +---------------+
 * |    data       |
 * |      blocks   |  rest of the blocks
 * +---------------+
 *
 */

struct ouichefs_inode {
	__le32 i_mode; /* File mode */
	__le32 i_uid; /* Owner id */
	__le32 i_gid; /* Group id */
	__le32 i_size; /* Size in bytes */
	__le32 i_ctime; /* Inode change time (sec)*/
	__le64 i_nctime; /* Inode change time (nsec) */
	__le32 i_atime; /* Access time (sec) */
	__le64 i_natime; /* Access time (nsec) */
	__le32 i_mtime; /* Modification time (sec) */
	__le64 i_nmtime; /* Modification time (nsec) */
	__le32 i_blocks; /* Block count */
	__le32 i_nlink; /* Hard links count */
	__le32 index_block; /* 27 LSB store block number containing slice, 5 MSB store slice number in block */
};

struct ouichefs_inode_info {
	uint32_t index_block; /* 27 LSB store block number containing slice, 5 MSB store slice number in block */
	struct inode vfs_inode;
};

#define OUICHEFS_INODES_PER_BLOCK \
	(OUICHEFS_BLOCK_SIZE / sizeof(struct ouichefs_inode))

struct ouichefs_sb_info {
	uint32_t magic; /* Magic number */

	uint32_t nr_blocks; /* Total number of blocks (incl sb & inodes) */
	uint32_t nr_inodes; /* Total number of inodes */

	uint32_t nr_istore_blocks; /* Number of inode store blocks */
	uint32_t nr_ifree_blocks; /* Number of inode free bitmap blocks */
	uint32_t nr_bfree_blocks; /* Number of block free bitmap blocks */

	uint32_t nr_free_inodes; /* Number of free inodes */
	uint32_t nr_free_blocks; /* Number of free blocks */

	uint32_t nr_sliced_blocks; /* Total number of sliced blocks */
	sector_t s_free_sliced_blocks; /* Number of the first block in list of partially filled blocks, 0 = empty */

	unsigned long *ifree_bitmap; /* In-memory free inodes bitmap */
	unsigned long *bfree_bitmap; /* In-memory free blocks bitmap */

	struct super_block *sb;
	struct kobject sysfs_kobj;
};

struct ouichefs_file_index_block {
	__le32 blocks[OUICHEFS_BLOCK_SIZE >> 2];
};

struct ouichefs_dir_block {
	struct ouichefs_file {
		__le32 inode;
		char filename[OUICHEFS_FILENAME_LEN];
	} files[OUICHEFS_MAX_SUBFILES];
};

struct ouichefs_sliced_block_header {
	__le32 slice_bitmap; /* Availibility of slice (1 for free, 0 for occupied) */
	__le32 next_partial_block; /* Block number of next partially filled block, 0 = last */
	char reserved[120];
} __attribute__((packed));

struct ouichefs_sliced_block {
	struct ouichefs_sliced_block_header header;
	char slices[31][128];
} __attribute__((packed));

/* superblock functions */
int ouichefs_fill_super(struct super_block *sb, void *data, int silent);

/* inode functions */
int ouichefs_init_inode_cache(void);
void ouichefs_destroy_inode_cache(void);
struct inode *ouichefs_iget(struct super_block *sb, unsigned long ino);

/* sysfs directory */
extern struct kobject *ouichefs_sysfs_dir;

/* sysfs functions */
int ouichefs_sysfs_init(struct super_block *sb);
void ouichefs_sysfs_exit(struct super_block *sb);

/* file functions */
extern const struct file_operations ouichefs_file_ops;
extern const struct file_operations ouichefs_dir_ops;

/* ioctl commmands */
#define DUMP_BLOCK \
	_IOR('D', 1, char[OUICHEFS_BLOCK_SIZE + OUICHEFS_SLICES_PER_BLOCK + 1])

/* Getters for superbock and inode */
#define OUICHEFS_SB(sb) (sb->s_fs_info)
#define OUICHEFS_INODE(inode) \
	(container_of(inode, struct ouichefs_inode_info, vfs_inode))

/* Other inline helpers */
static inline struct ouichefs_sb_info *
OUICHEFS_SB_FROM_KOBJ(struct kobject *kobj)
{
	return container_of(kobj, struct ouichefs_sb_info, sysfs_kobj);
}
#endif /* _OUICHEFS_H */
