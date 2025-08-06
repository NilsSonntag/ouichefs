#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/buffer_head.h>

#include "ouichefs.h"

struct kobject *ouichefs_sysfs_dir;

/* Show functions for virtual file system */

/* Number of free blocks */
static ssize_t free_blocks_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	return snprintf(buf, PAGE_SIZE, "%u\n", sbi->nr_free_blocks);
}
/* Helper to count used blocks */
static unsigned int get_used_blocks(struct ouichefs_sb_info *sbi)
{
	unsigned int nr_data_blocks =
		sbi->nr_blocks - 1 /* Superblock */ - sbi->nr_istore_blocks -
		sbi->nr_ifree_blocks - sbi->nr_bfree_blocks;
	return nr_data_blocks - sbi->nr_free_blocks - 1 /* Root index block */;
}
/* Number of used blocks (sliced or not) */
static ssize_t used_blocks_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	return snprintf(buf, PAGE_SIZE, "%u\n", get_used_blocks(sbi));
}
/* Number of sliced blocks */
static ssize_t sliced_blocks_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	return snprintf(buf, PAGE_SIZE, "%u\n", sbi->nr_sliced_blocks);
}

/* The total number of free slices (in partially filled blocks) */
static ssize_t total_free_slices_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	struct super_block *sb = sbi->sb;
	struct buffer_head *bh;
	sector_t curr; /* Iterator */
	unsigned long total_free_slices = 0;

	curr = sbi->s_free_sliced_blocks;
	while (curr != 0) {
		/* Get free slices per block */
		bh = sb_bread(sb, curr);
		if (!bh)
			return -EIO;
		struct ouichefs_sliced_block *s_block =
			(struct ouichefs_sliced_block *)bh->b_data;
		struct ouichefs_sliced_block_header s_header = s_block->header;
		total_free_slices += hweight32(s_header.slice_bitmap);
		/* Increment iterator */
		curr = s_header.next_partial_block;
		brelse(bh);
	}

	return snprintf(buf, PAGE_SIZE, "%lu\n", total_free_slices);
}
/* Number of files */
static ssize_t files_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	unsigned int files =
		sbi->nr_inodes - sbi->nr_free_inodes - 1 /* Root inode */;
	return snprintf(buf, PAGE_SIZE, "%u\n", files);
}
/* Number of small files */
static ssize_t small_files_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	struct super_block *sb = sbi->sb;
	unsigned int nr_small_files = 0;
	struct inode *inode;

	for (unsigned int i = 1; i < sbi->nr_inodes; i++) {
		inode = ouichefs_iget(sb, i);
		if (!inode || IS_ERR(inode))
			continue;

		if (inode->i_nlink > 0 && S_ISREG(inode->i_mode) &&
		    inode->i_size > 0 &&
		    inode->i_size <= OUICHEFS_SMALL_FILE_SIZE)
			nr_small_files++;
		iput(inode);
	}
	return snprintf(buf, PAGE_SIZE, "%u\n", nr_small_files);
}
/* Helper to get the sum of all inode sizes */
static unsigned long long get_total_data_size(struct ouichefs_sb_info *sbi)
{
	struct super_block *sb = sbi->sb;
	unsigned long long total_data_size = 0;
	struct inode *inode;

	for (unsigned int i = 1; i < sbi->nr_inodes; i++) {
		inode = ouichefs_iget(sb, i);
		if (!inode || IS_ERR(inode))
			continue;
		if (S_ISREG(inode->i_mode) && inode->i_size > 0) {
			total_data_size += inode->i_size;
		}
		iput(inode);
	}
	return total_data_size;
}
/* Total size of the data in the file system (in bytes) */
static ssize_t total_data_size_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	return snprintf(buf, PAGE_SIZE, "%llu\n", get_total_data_size(sbi));
}
/* Total size of the used blocks (in bytes) */
static ssize_t total_used_size_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	unsigned long used_size = get_used_blocks(sbi) * OUICHEFS_BLOCK_SIZE;
	return snprintf(buf, PAGE_SIZE, "%lu\n", used_size);
}
/* Ratio of the total size of the data in the file system to the total size of the used blocks (in %) */
static ssize_t efficiency_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB_FROM_KOBJ(kobj);
	unsigned long data_size = get_total_data_size(sbi);
	unsigned long used_size = get_used_blocks(sbi) * OUICHEFS_BLOCK_SIZE;
	uint8_t efficiency = used_size ? (data_size * 100) / used_size : 0;
	return snprintf(buf, PAGE_SIZE, "%u\n", efficiency);
}

static struct kobj_attribute free_blocks_attr = __ATTR_RO(free_blocks);
static struct kobj_attribute used_blocks_attr = __ATTR_RO(used_blocks);
static struct kobj_attribute sliced_blocks_attr = __ATTR_RO(sliced_blocks);
static struct kobj_attribute total_free_slices_attr =
	__ATTR_RO(total_free_slices);
static struct kobj_attribute files_attr = __ATTR_RO(files);
static struct kobj_attribute small_files_attr = __ATTR_RO(small_files);
static struct kobj_attribute total_data_size_attr = __ATTR_RO(total_data_size);
static struct kobj_attribute total_used_size_attr = __ATTR_RO(total_used_size);
static struct kobj_attribute efficiency_attr = __ATTR_RO(efficiency);

static struct attribute *ouichefs_attrs[] = {
	&free_blocks_attr.attr,	    &used_blocks_attr.attr,
	&sliced_blocks_attr.attr,   &total_free_slices_attr.attr,
	&files_attr.attr,	    &small_files_attr.attr,
	&total_data_size_attr.attr, &total_used_size_attr.attr,
	&efficiency_attr.attr,	    NULL,
};

static struct attribute_group ouichefs_attr_grp = {
	.attrs = ouichefs_attrs,
};

static const struct attribute_group *ouichefs_default_groups[] = {
	&ouichefs_attr_grp,
	NULL,
};

static struct kobj_type ouichefs_ktype = {
	.sysfs_ops = &kobj_sysfs_ops,
	.default_groups = ouichefs_default_groups,
};

/* Initialize ouichefs sysfs entries for a superblock. */
int ouichefs_sysfs_init(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	if (kobject_init_and_add(&sbi->sysfs_kobj, &ouichefs_ktype,
				 ouichefs_sysfs_dir, "%s", sb->s_id)) {
		kobject_put(&sbi->sysfs_kobj);
		return -EFAULT;
	}
	return 0;
}

/* Remove ouichefs sysfs entries for a superblock. */
void ouichefs_sysfs_exit(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi)
		kobject_put(&sbi->sysfs_kobj);
}
