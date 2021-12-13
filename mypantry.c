#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>

#include "pantryfs_inode.h"
#include "pantryfs_inode_ops.h"
#include "pantryfs_file.h"
#include "pantryfs_file_ops.h"
#include "pantryfs_sb.h"
#include "pantryfs_sb_ops.h"

int pantryfs_iterate(struct file *filp, struct dir_context *ctx)
{
	return -ENOSYS;
}

ssize_t pantryfs_read(struct file *filp, char __user *buf, size_t len,
		      loff_t *ppos)
{
	return -ENOSYS;
}

loff_t pantryfs_llseek(struct file *filp, loff_t offset, int whence)
{
	return -ENOSYS;
}

int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode,
		    bool excl)
{
	return -ENOSYS;
}

int pantryfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return -ENOSYS;
}

int pantryfs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return -ENOSYS;
}

void pantryfs_evict_inode(struct inode *inode)
{
	/* Required to be called by VFS. If not called, evict() will BUG out.*/
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

int pantryfs_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
	return -ENOSYS;
}

ssize_t pantryfs_write(struct file *filp, const char __user *buf, size_t len,
		       loff_t *ppos)
{
	return -ENOSYS;
}

struct dentry *pantryfs_lookup(struct inode *parent,
			       struct dentry *child_dentry, unsigned int flags)
{
	return NULL;
}

#pragma GCC diagnostic pop // "-Wunused-parameter"

static const char *file_type_name(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFSOCK:
		return "socket";
	case S_IFLNK:
		return "link";
	case S_IFREG:
		return "regular file";
	case S_IFBLK:
		return "block device";
	case S_IFDIR:
		return "directory";
	case S_IFCHR:
		return "character device";
	case S_IFIFO:
		return "fifo";
	default:
		return "unknown";
	}
}

int pantryfs_fill_super(struct super_block *sb, void *data __always_unused,
			int silent)
{
	int e;
	const char *fs_name;
	const char *dev_name;
	struct pantryfs_sb_buffer_heads *buf_heads;
	struct pantryfs_super_block *pantry_sb;
	struct pantryfs_inode *pantry_inodes;
	size_t inode_index;
	struct pantryfs_inode *pantry_root_inode;
	mode_t mode;
	struct inode *root_inode;

	e = 0;

	sb_set_blocksize(sb, PFS_BLOCK_SIZE);
	sb->s_maxbytes = PFS_BLOCK_SIZE;
	sb->s_magic = PANTRYFS_MAGIC_NUMBER;
	sb->s_op = &pantryfs_sb_ops;

	fs_name = sb->s_type->name;
	dev_name = sb->s_bdev->bd_disk->disk_name;

	buf_heads = kmalloc(sizeof(*buf_heads), GFP_KERNEL);
	if (!buf_heads) {
		e = -ENOMEM;
		goto ret;
	}

	buf_heads->sb_bh = sb_bread(sb, PANTRYFS_SUPERBLOCK_DATABLOCK_NUMBER);
	if (!buf_heads->sb_bh) {
		e = -ENOMEM;
		goto free_buf_heads;
	}
	pantry_sb = (struct pantryfs_super_block *)buf_heads->sb_bh->b_data;
	if (pantry_sb->magic != sb->s_magic) {
		e = -EINVAL;
		if (!silent)
			pr_err("/dev/%s is not formatted as %s, so could not mount it: found magic %llx instead of %lx",
			       dev_name, fs_name, pantry_sb->magic,
			       sb->s_magic);
		goto free_sb_bh;
	}

	buf_heads->i_store_bh =
		sb_bread(sb, PANTRYFS_INODE_STORE_DATABLOCK_NUMBER);
	if (!buf_heads->i_store_bh) {
		e = -ENOMEM;
		goto free_sb_bh;
	}
	pantry_inodes = (struct pantryfs_inode *)buf_heads->i_store_bh->b_data;
	inode_index = PANTRYFS_ROOT_INODE_NUMBER - 1; // 1-indexed
	pantry_root_inode = &pantry_inodes[inode_index];
	mode = pantry_root_inode->mode;
	if (mode != (S_IFDIR | 0777)) {
		// part2 instructions said to do this
		mode = S_IFDIR | 0777;
	}
	if (!S_ISDIR(mode)) {
		// this is also checked and handled fine later on,
		// so just print the warning here
		// e = -ENOTDIR;
		if (!silent)
			pr_err("root inode is a %s, not a %s: /dev/%s is incorrectly formatted for %s\n",
			       file_type_name(mode), file_type_name(S_IFDIR),
			       dev_name, fs_name);
		// goto free_i_store_bh;
	}

	sb->s_fs_info = buf_heads;

	root_inode = iget_locked(sb, PANTRYFS_ROOT_INODE_NUMBER);
	if (!root_inode) {
		e = -ENOMEM;
		goto free_i_store_bh;
	}
	// part2 instructions said to do only set mode
	// root_inode->i_op = &pantryfs_inode_ops;
	// root_inode->i_fop = &pantryfs_dir_ops;
	root_inode->i_mode = mode;
	// root_inode->i_uid =
	// 	make_kuid(current_user_ns(), pantry_root_inode->uid);
	// root_inode->i_gid =
	// 	make_kgid(current_user_ns(), pantry_root_inode->gid);
	// root_inode->i_atime = pantry_root_inode->i_atime;
	// root_inode->i_mtime = pantry_root_inode->i_mtime;
	// root_inode->i_ctime = pantry_root_inode->i_ctime;
	// set_nlink(root_inode, pantry_root_inode->nlink);
	// file size <= PFS_BLOCK_SIZE = 4096
	// so we can safely cast this to signed
	// root_inode->i_size = (loff_t)pantry_root_inode->file_size;
	unlock_new_inode(root_inode);

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		e = -ENOMEM;
		// `d_make_root` frees the inode if the dentry allocation fails
		goto free_i_store_bh;
	}
	goto ret;

free_i_store_bh:
	brelse(buf_heads->i_store_bh);
free_sb_bh:
	brelse(buf_heads->sb_bh);
free_buf_heads:
	kfree(buf_heads);
ret:
	return e;
}

struct file_system_type pantryfs_fs_type;

static struct dentry *pantryfs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	if (fs_type != &pantryfs_fs_type)
		return ERR_PTR(-EINVAL);

	/* `mount_bdev` is "mount block device". */
	ret = mount_bdev(fs_type, flags, dev_name, data, pantryfs_fill_super);

	if (IS_ERR(ret))
		pr_err("Error mounting mypantryfs");
	else
		pr_info("Mounted mypantryfs on [%s]\n", dev_name);

	return ret;
}

static void pantryfs_kill_superblock(struct super_block *sb)
{
	struct pantryfs_sb_buffer_heads *buf_heads;

	buf_heads = (struct pantryfs_sb_buffer_heads *)sb->s_fs_info;
	brelse(buf_heads->i_store_bh);
	brelse(buf_heads->sb_bh);
	kfree(buf_heads);
	kill_block_super(sb);
	pr_info("mypantryfs superblock destroyed. Unmount successful.\n");
}

struct file_system_type pantryfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "mypantryfs",
	.mount = pantryfs_mount,
	.kill_sb = pantryfs_kill_superblock,
};

static int pantryfs_init(void)
{
	int ret;

	ret = register_filesystem(&pantryfs_fs_type);
	if (likely(ret == 0))
		pr_info("Successfully registered mypantryfs\n");
	else
		pr_err("Failed to register mypantryfs. Error:[%d]", ret);

	return ret;
}

static void pantryfs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&pantryfs_fs_type);

	if (likely(ret == 0))
		pr_info("Successfully unregistered mypantryfs\n");
	else
		pr_err("Failed to unregister mypantryfs. Error:[%d]", ret);
}

module_init(pantryfs_init);
module_exit(pantryfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Group FireFerrises");
