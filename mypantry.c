#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/writeback.h>

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

// make sure nothing weird happened to the inode
int pantryfs_file_soundness_check(const struct inode *inode)
{
	mode_t mode;

	mode = inode->i_mode;
	if (!(S_ISDIR(mode) ^ S_ISREG(mode))) {
		// only directories and regular files allowed
		return -EIO;
	}
	if ((inode->i_sb->s_op != &pantryfs_sb_ops) ||
	    (inode->i_op != &pantryfs_inode_ops) ||
	    (inode->i_fop !=
	     (S_ISDIR(mode) ? &pantryfs_dir_ops : &pantryfs_file_ops)))
		return -EIO;
	return 0;
}

int pantryfs_file_check(const struct inode *inode, bool expecting_dir)
{
	int e;
	bool is_dir;

	e = pantryfs_file_soundness_check(inode);
	if (e < 0)
		return e;
	is_dir = S_ISDIR(inode->i_mode);
	if (expecting_dir) {
		if (!is_dir)
			return -ENOTDIR;
	} else {
		if (is_dir)
			return -EISDIR;
	}
	return 0;
}

int pantryfs_dir_check(const struct inode *inode)
{
	return pantryfs_file_check(inode, /* is_dir */ true);
}

int pantryfs_reg_file_check(const struct inode *inode)
{
	return pantryfs_file_check(inode, /* is_dir */ false);
}

struct pantryfs_root {
	struct super_block *vfs_sb;
	const struct pantryfs_sb_buffer_heads *buf_heads;
	struct pantryfs_super_block *sb;
	struct pantryfs_inode *inodes;
};

struct pantryfs_root pantryfs_root_get(struct super_block *sb)
{
	const struct pantryfs_sb_buffer_heads *buf_heads =
		(const struct pantryfs_sb_buffer_heads *)sb->s_fs_info;
	return (struct pantryfs_root){
		.vfs_sb = sb,
		.buf_heads = buf_heads,
		.sb = (struct pantryfs_super_block *)buf_heads->sb_bh->b_data,
		.inodes =
			(struct pantryfs_inode *)buf_heads->i_store_bh->b_data,
	};
}

struct pantryfs_file {
	struct inode *vfs_inode;
	struct pantryfs_inode *inode;
	struct pantryfs_root root;
	struct buffer_head *block;
};

int pantryfs_file_get(struct inode *inode, struct pantryfs_file *file)
{
	file->vfs_inode = inode;
	file->inode = (struct pantryfs_inode *)file->vfs_inode->i_private;
	file->root = pantryfs_root_get(file->vfs_inode->i_sb);
	if (!IS_SET(file->root.sb->free_data_blocks,
		    file->inode->data_block_number))
		return -ENOENT;
	file->block =
		sb_bread(file->root.vfs_sb, file->inode->data_block_number);
	if (!file->block)
		return -ENOMEM;
	// don't mark inode dirty, like in relatime
	inode->i_atime = current_time(inode);
	return 0;
}

struct pantryfs_dir {
	struct pantryfs_file file;
	struct pantryfs_dir_entry *dentries;
};

int pantryfs_dir_get(struct inode *inode, struct pantryfs_dir *dir)
{
	int e;

	e = 0;
	e = pantryfs_file_get(inode, &dir->file);
	if (e < 0)
		goto ret;
	dir->dentries = (struct pantryfs_dir_entry *)dir->file.block->b_data;

ret:
	return e;
}

struct pantryfs_reg_file {
	struct pantryfs_file file;
	char *data;
};

int pantryfs_reg_file_get(struct inode *inode, struct pantryfs_reg_file *file)
{
	int e;

	e = 0;
	e = pantryfs_file_get(inode, &file->file);
	if (e < 0)
		goto ret;
	file->data = file->file.block->b_data;

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
	bool is_dir;

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
	is_dir = S_ISDIR(pantry_inode->mode);
	inode->i_sb = root->vfs_sb;
	inode->i_op = &pantryfs_inode_ops;
	inode->i_fop = is_dir ? &pantryfs_dir_ops : &pantryfs_file_ops;
	inode->i_private = (void *)pantry_inode;
	inode->i_mode = pantry_inode->mode;
	i_uid_write(inode, pantry_inode->uid);
	i_gid_write(inode, pantry_inode->gid);
	inode->i_atime = pantry_inode->i_atime;
	inode->i_mtime = pantry_inode->i_mtime;
	inode->i_ctime = pantry_inode->i_ctime;
	set_nlink(inode, pantry_inode->nlink);
	// file size <= PFS_BLOCK_SIZE = 4096
	// so we can safely cast this to signed
	inode->i_size = (loff_t)pantry_inode->file_size;
	if (is_dir) {
		// Tal said to override
		inode->i_size = PFS_BLOCK_SIZE;
	}
	inode->i_blocks = PFS_BLOCK_SIZE / 512 + (PFS_BLOCK_SIZE % 512 != 0);
	unlock_new_inode(inode);
	return inode;
}

ssize_t pantryfs_read_or_write(struct file *file, char __user *buf, size_t len,
			       loff_t *ppos, bool write)
{
	int e;
	struct inode *inode;
	size_t size;
	size_t max_size;
	size_t start;
	size_t size_remaining;
	size_t end;
	struct pantryfs_reg_file reg_file;
	struct buffer_head *block;
	char *ptr;
	unsigned long bytes_left;

	e = 0;
	start = *ppos;
	if (*ppos < 0) {
		e = -EIO;
		goto ret;
	}
	start = (size_t)*ppos;
	inode = file_inode(file);
	if (inode->i_size < 0) {
		e = -EIO;
		goto ret;
	}
	size = (size_t)inode->i_size;
	if (write)
		max_size = PFS_BLOCK_SIZE;
	else
		max_size = min_t(size_t, size, PFS_BLOCK_SIZE);
	if (len == 0 || start > max_size) {
		len = 0;
		goto ret;
	}
	if (!buf) {
		e = -EFAULT;
		goto ret;
	}
	// can't underflow b/c checked start > size
	size_remaining = max_size - start;
	// avoid potential overflows
	len = min_t(size_t, len, size_remaining);

	e = pantryfs_reg_file_check(inode);
	if (e < 0)
		goto ret;
	e = pantryfs_reg_file_get(inode, &reg_file);
	if (e < 0)
		goto ret;
	block = reg_file.file.block;
	ptr = &reg_file.data[start];
	if (write)
		bytes_left = copy_from_user(ptr, buf, len);
	else
		bytes_left = copy_to_user(buf, ptr, len);
	if (bytes_left > 0) {
		e = -EFAULT;
		goto free_block;
	}
	end = start + len;
	if (write) {
		if (end > size)
			inode->i_size = (loff_t)end;
		inode->i_mtime = inode->i_atime;
		mark_inode_dirty(inode);
		mark_buffer_dirty(block);
	}
	*ppos = (loff_t)end;
free_block:
	brelse(block);
ret:
	if (e < 0)
		return (ssize_t)e;
	return (ssize_t)len;
}

struct inode *pantryfs_create_new_inode(struct pantryfs_dir *dir,
					struct qstr name, umode_t mode)
{
	int e;
	struct pantryfs_super_block *sb;
	size_t inode_index;
	size_t data_block_index;
	size_t dentry_index;
	uint64_t ino;
	bool is_dir;
	struct pantryfs_dir_entry *pantry_dentry;
	struct pantryfs_inode *pantry_inode;
	struct inode *inode;

	e = 0;
	if (!S_ISREG(mode)) {
		e = -ENOSYS;
		goto ret;
	}
	sb = dir->file.root.sb;

#define find_index(i, n, check)                                                \
	do {                                                                   \
		for (i = 0; i < (n); i++) {                                    \
			if ((check))                                           \
				break;                                         \
		}                                                              \
		if (i >= (n)) {                                                \
			e = -ENOSPC;                                           \
		}                                                              \
	} while (false)

	find_index(inode_index, PFS_MAX_INODES,
		   !IS_SET(sb->free_inodes, inode_index));
	if (e < 0)
		goto ret;
	find_index(data_block_index, PFS_MAX_INODES,
		   !IS_SET(sb->free_data_blocks, data_block_index));
	if (e < 0)
		goto ret;
	find_index(dentry_index, PFS_MAX_INODES,
		   !dir->dentries[dentry_index].active);
	if (e < 0)
		goto ret;

#undef find_index

	ino = inode_index + 1;
	inode = iget_locked(dir->file.root.vfs_sb, ino);
	if (!inode) {
		e = -ENOMEM;
		goto ret;
	}

	pantry_dentry = &dir->dentries[dentry_index];
	pantry_inode = &dir->file.root.inodes[inode_index];
	SETBIT(sb->free_inodes, inode_index);
	SETBIT(sb->free_data_blocks, data_block_index);
	pantry_dentry->active = true;
	pantry_dentry->inode_no = ino;
	memcpy(pantry_dentry->filename, name.name, name.len);
	pantry_dentry->filename[name.len] = 0;

	is_dir = S_ISDIR(mode);
	inode->i_sb = dir->file.root.vfs_sb;
	inode->i_op = &pantryfs_inode_ops;
	inode->i_fop = is_dir ? &pantryfs_dir_ops : &pantryfs_file_ops;
	inode->i_private = (void *)pantry_inode;
	inode->i_mode = mode;
	inode->i_uid = current_cred()->uid;
	inode->i_gid = current_cred()->gid;
	inode->i_atime = current_time(inode);
	inode->i_mtime = inode->i_atime;
	inode->i_ctime = inode->i_atime;
	set_nlink(inode, 1);
	if (is_dir)
		inode->i_size = PFS_BLOCK_SIZE;
	else
		inode->i_size = 0;
	inode->i_blocks = PFS_BLOCK_SIZE / 512 + (PFS_BLOCK_SIZE % 512 != 0);
	mark_inode_dirty(inode);
	unlock_new_inode(inode);

ret:
	if (e < 0)
		return ERR_PTR(e);
	return inode;
}

int pantryfs_lookup_or_create(struct inode *parent, struct dentry *child_dentry,
			      bool create, umode_t mode, bool excl)
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
	e = pantryfs_dir_check(parent);
	if (e < 0)
		goto ret;
	e = pantryfs_dir_get(parent, &dir);
	if (e < 0)
		goto ret;
	inode = NULL;
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		const struct pantryfs_dir_entry *dentry;

		dentry = &dir.dentries[i];
		if (pantryfs_dentry_eq_name(child_dentry, dentry)) {
			inode = pantryfs_create_inode(&dir.file.root,
						      dentry->inode_no);
			break;
		}
	}
	if (IS_ERR(inode)) {
		e = PTR_ERR(inode);
		goto free_block;
	}
	if (create) {
		if (excl && inode) {
			e = -EEXIST;
			goto free_block;
		}
		if (!inode) {
			inode = pantryfs_create_new_inode(
				&dir, child_dentry->d_name, mode);
			WARN_ON(!inode);
			if (IS_ERR(inode)) {
				e = PTR_ERR(inode);
				goto free_block;
			}
		}
	}
	d_add(child_dentry, inode);
	if (!create)
		dget(child_dentry);
	goto free_block;

free_block:
	brelse(dir.file.block);
ret:
	return e;
}

int pantryfs_iterate(struct file *file, struct dir_context *ctx)
{
	static const size_t num_dots = 2; // 2 for `.` and `..`
	int e;
	struct inode *inode;
	struct pantryfs_dir dir;
	size_t i;

	e = 0;
	inode = file_inode(file);
	e = pantryfs_dir_check(inode);
	if (e < 0)
		goto ret;
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
	e = pantryfs_dir_get(inode, &dir);
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
			pantryfs_lookup_inode(&dir.file.root, dentry->inode_no);
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
	brelse(dir.file.block);

ret:
	return e;
}

ssize_t pantryfs_read(struct file *file, char __user *buf, size_t len,
		      loff_t *ppos)
{
	return pantryfs_read_or_write(file, buf, len, ppos, /* write */ false);
}

loff_t pantryfs_llseek(struct file *file, loff_t offset, int whence)
{
	return generic_file_llseek(file, offset, whence);
}

int pantryfs_create(struct inode *parent, struct dentry *child_dentry,
		    umode_t mode, bool excl)
{
	return pantryfs_lookup_or_create(parent, child_dentry,
					 /* create */ true, mode, excl);
}

int pantryfs_unlink(struct inode *parent, struct dentry *dentry)
{
	int e;
	struct inode *inode;
	struct pantryfs_dir dir;
	struct buffer_head *block;
	size_t i;

	e = 0;
	e = pantryfs_dir_check(parent);
	if (e < 0)
		goto ret;
	// don't support removing directories yet
	// i.e. `unlink` w/ `AT_REMOVEDIR` or `rmdir`
	inode = d_inode(dentry);
	e = pantryfs_reg_file_check(inode);
	if (e < 0)
		goto ret;
	e = pantryfs_dir_get(parent, &dir);
	if (e < 0)
		goto ret;
	block = dir.file.block;
	for (i = 0; i < PFS_MAX_CHILDREN; i++) {
		struct pantryfs_dir_entry *raw_dentry;

		raw_dentry = &dir.dentries[i];
		if (pantryfs_dentry_eq_name(dentry, raw_dentry)) {
			raw_dentry->active = false;
			break;
		}
	}
	if (i >= PFS_MAX_CHILDREN) {
		e = -ENOENT;
		goto free_block;
	}
	inode_dec_link_count(inode);
	mark_buffer_dirty(block);

free_block:
	brelse(block);
ret:
	return e;
}

int pantryfs_write_inode(struct inode *mut_inode, struct writeback_control *wbc)
{
	int e;
	const struct inode *inode;
	struct pantryfs_inode *pantry_inode;
	struct pantryfs_root root;
	struct buffer_head *block;

	e = 0;
	inode = mut_inode;
	e = pantryfs_file_soundness_check(inode);
	if (e < 0)
		goto ret;
	if (inode->i_size < 0 || inode->i_size > PFS_BLOCK_SIZE) {
		e = -EIO;
		goto ret;
	}
	pantry_inode = (struct pantryfs_inode *)inode->i_private;
	root = pantryfs_root_get(inode->i_sb);

	pantry_inode->mode = inode->i_mode;
	pantry_inode->uid = i_uid_read(inode);
	pantry_inode->gid = i_gid_read(inode);
	pantry_inode->i_atime = inode->i_atime;
	pantry_inode->i_mtime = inode->i_mtime;
	pantry_inode->i_ctime = inode->i_ctime;
	pantry_inode->nlink = inode->i_nlink;
	pantry_inode->file_size = (uint64_t)inode->i_size; // checked

	block = root.buf_heads->i_store_bh;
	mark_buffer_dirty(block);
	if (wbc->sync_mode == WB_SYNC_ALL)
		sync_dirty_buffer(block);

ret:
	return e;
}

void pantryfs_evict_inode(struct inode *inode)
{
	struct pantryfs_root root;
	size_t i;

	root = pantryfs_root_get(inode->i_sb);
	i = inode->i_ino - 1;
	// can't do any error handling here (void return)
	// but still should avoid UB
	if (i < PFS_MAX_INODES) {
		CLEARBIT(root.sb->free_inodes, i);
		CLEARBIT(root.sb->free_data_blocks, i);
	} else {
		pr_err("%s out-of-bounds: %zu/%zu\n", __func__, i,
		       PFS_MAX_INODES);
	}
	mark_buffer_dirty(root.buf_heads->sb_bh);

	/* Required to be called by VFS. If not called, evict() will BUG out.*/
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

int pantryfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	return generic_file_fsync(file, start, end, datasync);
}

ssize_t pantryfs_write(struct file *file, const char __user *buf, size_t len,
		       loff_t *ppos)
{
	// buf cast is safe b/c only accessed as const when write = true
	return pantryfs_read_or_write(file, (char __user *)buf, len, ppos,
				      /* write */ true);
}

struct dentry *pantryfs_lookup(struct inode *parent,
			       struct dentry *child_dentry,
			       unsigned int flags __always_unused)
{
	int e;

	e = pantryfs_lookup_or_create(parent, child_dentry,
				      /* create */ false, /* mode */ 0,
				      /* excl */ false);
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

	sb->s_fs_info = (void *)buf_heads;
	root = pantryfs_root_get(sb);
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
	kill_block_super(sb);
	sb->s_fs_info = NULL;
	brelse(buf_heads->i_store_bh);
	brelse(buf_heads->sb_bh);
	kfree(buf_heads);
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
