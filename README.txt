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
TODO


### Part 4
TODO


### Part 5
TODO


### Part 6
TODO


### Part 7

