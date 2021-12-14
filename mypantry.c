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

int pantryfs_check_dir(const struct inode *inode)
{
	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (inode->i_fop != &pantryfs_dir_ops)
		return -EINVAL;
	return 0;
}

struct pantryfs_root {
	struct super_block *vfs_sb;
	const struct pantryfs_sb_buffer_heads *buf_heads;
	const struct pantryfs_super_block *sb;
	struct pantryfs_inode *inodes;
};

struct pantryfs_root pantryfs_root_new(struct super_block *sb)
{
	const struct pantryfs_sb_buffer_heads *buf_heads =
		(const struct pantryfs_sb_buffer_heads *)sb->s_fs_info;
	return (struct pantryfs_root){
		.vfs_sb = sb,
		.buf_heads = buf_heads,
		.sb = (const struct pantryfs_super_block *)
			      buf_heads->sb_bh->b_data,
		.inodes =
			(struct pantryfs_inode *)buf_heads->i_store_bh->b_data,
	};
}

struct pantryfs_dir {
	const struct inode *vfs_inode;
	const struct pantryfs_inode *inode;
	struct pantryfs_root root;
	struct buffer_head *block;
	const struct pantryfs_dir_entry *dentries;
};

int pantryfs_dir_get(const struct inode *inode, struct pantryfs_dir *dir)
{
	int e;

	e = 0;
	e = pantryfs_check_dir(inode);
	if (e < 0)
		goto ret;
	dir->vfs_inode = inode;
	dir->inode = (const struct pantryfs_inode *)dir->vfs_inode->i_private;
	dir->root = pantryfs_root_new(dir->vfs_inode->i_sb);
	dir->block = sb_bread(dir->root.vfs_sb, dir->inode->data_block_number);
	if (!dir->block) {
		e = -ENOMEM;
		goto ret;
	}
	dir->dentries = (const struct pantryfs_dir_entry *)dir->block->b_data;

ret:
	return e;
}

struct qstr pantryfs_dentry_name(const struct pantryfs_dir_entry *dentry)
{
	return (struct qstr)QSTR_INIT(dentry->filename,
				      strnlen(dentry->filename,
					      sizeof(dentry->filename)));
}

bool pantryfs_dentry_eq_name(const struct dentry *dentry,
			     const struct pantryfs_dir_entry *pantry_dentry)
{
	struct qstr name1;
	struct qstr name2;

	if (!pantry_dentry->active)
		return false;
	name1 = dentry->d_name;
	name2 = pantryfs_dentry_name(pantry_dentry);
	return name1.len == name2.len &&
	       memcmp(name1.name, name2.name, name1.len) == 0;
}

struct pantryfs_inode *pantryfs_lookup_inode(const struct pantryfs_root *root,
					     uint64_t ino)
{
	size_t i;

	i = ino - 1;
	// check if valid first; don't want to read uninitialized memory
	if (i >= PFS_MAX_INODES || !IS_SET(root->sb->free_inodes, i))
		return ERR_PTR(-EIO);
	return &root->inodes[i];
}

struct inode *pantryfs_create_inode(const struct pantryfs_root *root,
				    uint64_t ino)
{
	int e;
	size_t inode_index;
	struct pantryfs_inode *pantry_inode;
	struct inode *inode;

	e = 0;
	inode_index = ino - 1;
	// check if valid first; don't want to read uninitialized memory
	if (inode_index >= PFS_MAX_INODES ||
	    !IS_SET(root->sb->free_inodes, inode_index))
		return ERR_PTR(-EIO);
	pantry_inode = &root->inodes[inode_index];

	inode = iget_locked(root->vfs_sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW)) {
		// already filled in and unlocked
		return inode;
	}
	inode->i_sb = root->vfs_sb;
	inode->i_op = &pantryfs_inode_ops;
	inode->i_fop = S_ISDIR(pantry_inode->mode) ? &pantryfs_dir_ops :
							   &pantryfs_file_ops;
	inode->i_private = pantry_inode;
	inode->i_mode = pantry_inode->mode;
	inode->i_uid = make_kuid(current_user_ns(), pantry_inode->uid);
	inode->i_gid = make_kgid(current_user_ns(), pantry_inode->gid);
	inode->i_atime = pantry_inode->i_atime;
	inode->i_mtime = pantry_inode->i_mtime;
	inode->i_ctime = pantry_inode->i_ctime;
	set_nlink(inode, pantry_inode->nlink);
	// file size <= PFS_BLOCK_SIZE = 4096
	// so we can safely cast this to signed
	inode->i_size = (loff_t)pantry_inode->file_size;
	unlock_new_inode(inode);
	return inode;
}

int pantryfs_iterate(struct file *file, struct dir_context *ctx)
{
	static const size_t num_dots = 2; // 2 for `.` and `..`
	int e;
	struct pantryfs_dir dir;
	size_t i;

	e = 0;
	if (ctx->pos < 0 || (size_t)ctx->pos >= PFS_MAX_CHILDREN + num_dots) {
		// quick check before running/allocating/reading anything
		// negative pos is wrong
		// don't want to segfault or overflow,
		// so return nothing in that case
		goto ret;
	}
	if (!dir_emit_dots(file, ctx)) {
		// no space, try again next time
		goto ret;
	}
	e = pantryfs_dir_get(file_inode(file), &dir);
	if (e < 0)
		goto ret;
	for (i = (size_t)ctx->pos - num_dots; i < PFS_MAX_CHILDREN; i++) {
		const struct pantryfs_dir_entry *dentry;
		const struct pantryfs_inode *dentry_inode;
		struct qstr name;

		dentry = &dir.dentries[i];
		if (!dentry->active)
			continue;
		// need to look up inode to get file type
		// since that's not stored in the dentry
		dentry_inode =
			pantryfs_lookup_inode(&dir.root, dentry->inode_no);
		if (IS_ERR(dentry_inode)) {
			e = PTR_ERR(dentry_inode);
			i++; // skip over bad dentry
			break;
		}
		name = pantryfs_dentry_name(dentry);
		if (!dir_emit(ctx, name.name, name.len, dentry->inode_no,
			      S_DT(dentry_inode->mode))) {
			// ran out of space to write dirents
			// so don't ctx->pos++ so we can repeat this dentry
			break;
		}
	}
	ctx->pos = (loff_t)(i + num_dots);
	brelse(dir.block);

ret:
	return e;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

ssize_t pantryfs_read(struct file *file, char __user *buf, size_t len,
		      loff_t *ppos)
{
	return -ENOSYS;
}

loff_t pantryfs_llseek(struct file *file, loff_t offset, int whence)
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

int pantryfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return -ENOSYS;
}

ssize_t pantryfs_write(struct file *file, const char __user *buf, size_t len,
		       loff_t *ppos)
{
	return -ENOSYS;
}

#pragma GCC diagnostic pop // "-Wunused-parameter"

struct dentry *pantryfs_lookup(struct inode *parent,
			       struct dentry *child_dentry,
			       unsigned int flags __always_unused)
{
	int e;
	struct pantryfs_dir dir;
	size_t i;
	struct inode *inode;

	e = 0;
	if (child_dentry->d_name.len > PANTRYFS_MAX_FILENAME_LENGTH) {
		e = -ENAMETOOLONG;
		goto ret;
	}
	e = pantryfs_dir_get(parent, &dir);
	if (e < 0)
		goto ret;
	inode = NULL;
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		const struct pantryfs_dir_entry *dentry;

		dentry = &dir.dentries[i];
		if (pantryfs_dentry_eq_name(child_dentry, dentry)) {
			inode = pantryfs_create_inode(&dir.root,
						      dentry->inode_no);
			break;
		}
	}
	if (IS_ERR(inode)) {
		e = PTR_ERR(inode);
		goto free_block;
	}
	d_add(child_dentry, inode);
	dget(child_dentry);
	goto free_block;

free_block:
	brelse(dir.block);
ret:
	if (e < 0)
		return ERR_PTR(e);
	return child_dentry;
}

int pantryfs_fill_super(struct super_block *sb, void *data __always_unused,
			int silent)
{
	int e;
	const char *fs_name;
	const char *dev_name;
	struct pantryfs_sb_buffer_heads *buf_heads;
	struct pantryfs_super_block *pantry_sb;
	struct pantryfs_root root;
	struct inode *root_inode;
	mode_t mode;

	e = 0;

	sb_set_blocksize(sb, PFS_BLOCK_SIZE);
	sb->s_maxbytes = PFS_BLOCK_SIZE;
	sb->s_magic = PANTRYFS_MAGIC_NUMBER;
	sb->s_op = &pantryfs_sb_ops;

	fs_name = sb->s_type->name;
	dev_name = sb->s_id;

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

	sb->s_fs_info = buf_heads;
	root = pantryfs_root_new(sb);
	root_inode = pantryfs_create_inode(&root, PANTRYFS_ROOT_INODE_NUMBER);
	if (IS_ERR(root_inode)) {
		e = PTR_ERR(root_inode);
		goto free_sb_bh;
	}
	mode = root_inode->i_mode;
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
