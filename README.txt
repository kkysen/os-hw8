# hw8

## Authors
Khyber Sen, ks3343
Isabelle Arevalo, ia2422
WenChing Li, wl2795

## Instructions

This file should contain:

- Your name & UNI (or those of all group members for group assignments)
- Homework assignment number
- Description for each part

The description should indicate whether your solution for the part is working
or not. You may also want to include anything else you would like to
communicate to the grader, such as extra functionality you implemented or how
you tried to fix your non-working code.

## Part Descriptions

### Part 1
This part is working.

We refactored `format_disk_as_pantry.c` so that all the blocks
would be created at once given a `struct file_args[]`,
each of while `struct file_args` is either a:
* `struct directory_args`: an array of `struct dentry_args`,
    which are just the filename and inode (offset).
* `struct regular_file_args`: the file data
This is done in the `format_disk` function.

This made writing all the blocks far simpler,
since there is so much shared between all the blocks,
and we feel this better shows the core of writing the blocks,
since there's much less hardcoding.

For the algorithm,
we first allocate the super and inode blocks,
but then immediately seek past them.
We loop through the `struct file_args`s,
constructing the inode for each.
We then write the data block,
and set the inode in the inode block buffer,
as well as the bits in the super block buffer.
Then at the end, we seek back to the beginning
and write the super and inode blocks to disk.

For constructing each inode,
we have a `char data_buf[PFS_BLOCK_SIZE] = {0}` zeroed out for writing,
and then if it's a regular file, we write the data to the beginning,
and if it's a directory, we cast it to a dentries array
and then set each dentry.

We calculate the number of links for a directory
by starting with 2 for the main link from the parent
and the `.` self link, and then 1 for each `..` link
for sub directories, which can look up directly in
the `struct file_args[]` array instead of hardcoding everything.


### Part 2
This part is working.

Originally we were copying all the root inode metadata (read from disk)
to the VFS inode, but the instructions said to only set the mode,
and to set it to `drwxrwxrwx` aka `S_IFDIR | 0777`,
so we did that and commented out the full inode setting code.


### Part 3
This part is working.

In `pantryfs_fill_super`, we also copied the other metadata fields
from the `struct pantryfs_inode` to the `struct inode`.
Not sure if that's necessary, but it let us `stat` the root directory.
This also means that we set whatever mode
is set for the root directory on disk;
of course this still has to be a directory,
but the permissions could not be `0777`.
`format_disk_as_pantryfs` sets `0777`, though.

Speaking of `format_disk_as_pantryfs`, we modified it
to add a bunch of other files for testing.

In `pantryfs_iterate`, we also check if any dentry `inode_no`
is invalid, i.e. if it's out of bounds
or points to a free inode in `sb->free_inodes`.
If it is, we skip that dentry and return `EIO`.
This way we avoid invalid reads if the disk image
is corrupted/ill-formatted.

We also update the access time since ext4 appears to do this,
at least without `noatime` (it's defaulting to `relatime`).
However, since we haven't implemented `pantryfs_write_inode` yet,
this atime never gets written back to disk.


### Part 4
This part is working.

Since there was starting to be a lot code in common between
`pantryfs_{iterate,lookup,fill_super}`,
we refactored a bunch of it to a few `struct`s and
functions at the top of `mypantry.c`.
For example, `struct pantryfs_root` conveniently stores the data
accessible through the `struct super_block`'s `s_fs_info` field.
And `struct pantryfs_dir` is all the dir related data
looked up from a `struct inode` that's a directory.

### Part 5
This part is working.

With the previous refactorings, `pantryfs_read` was pretty simple.
We also checked as many error/zero read conditions as soon as possible
before doing any possible disk reads.
We also checked carefully to avoid overflows,
especially because `len` is from the user and it's mixed with `loff_t`s,
and signed integer overflow is UB.

Like in part 3, we also update the access time when reading a file,
though again, we don't flush the new time to disk.


### Part 6
This part is working.

We had already copied the inode metadata in the previous parts,
so this part was pretty simple.
We just fixed some small things (like setting the 512-byte block count).
We mainly just added more testing for this part.

Note that for the block count, we use 512-byte blocks,
since this is what the documentation for `stat(2)` says to do,
and this is what is done by other filesystems like ext4.

We also override the size of a directory to always be `PFS_BLOCK_SIZE`.
This is what the reference implementation seems to do,
and is what Tal said we should do.
By default, the pantryfs disk formatter sets the size of 0 for directories,
which is why the correction is needed.


### Part 7
This part is working.

For `pantryfs_write`, since so much of it's very similar to `pantryfs_read`,
like all the position handling and loading the data block,
we refactored them into `pantryfs_read_or_write`,
which just does a few things different between read and write,
like using `copy_to_user` vs. `copy_from_user`,
potentially increasing the size for write,
updating the mtime for write,
and marking the buffer and inode (for mtime) dirty for write.

For `pantryfs_fsync`, we just called `generic_file_fsync`.
This is what ext2 does, and it seems to work perfectly fine for us, too.


### Part 8
This part is mostly working.

Sometimes on unmount, it says target is busy.
Then when we forcefully unmount it (`umount -l`),
it doesn't save the changes to disk.

Since `pantryfs_lookup` and `pantryfs_create` would be quite similar,
we refactored them both into `pantryfs_lookup_or_create`, which can do either.


### Part 9
TODO
