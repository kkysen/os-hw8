#ifndef __PANTRY_FS_INODE_OPS_H__
#define __PANTRY_FS_INODE_OPS_H__
struct dentry *pantryfs_lookup(struct inode *parent, struct dentry *child_dentry,
			unsigned int flags);
int pantryfs_create(struct inode *parent, struct dentry *dentry, umode_t mode, bool excl);
int pantryfs_unlink(struct inode *dir, struct dentry *dentry);

const struct inode_operations pantryfs_inode_ops = {
	.lookup = pantryfs_lookup,
	.create = pantryfs_create,
	.unlink = pantryfs_unlink,
};
#endif /* ifndef __PANTRY_FS_INODE_OPS_H__ */
