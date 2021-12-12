#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>

/* These are the same on a 64-bit architecture */
#define timespec64 timespec

#include "pantryfs_inode.h"
#include "pantryfs_file.h"
#include "pantryfs_sb.h"

struct dentry_args {
	const char *filename; // cstring
	uint8_t index;
};

struct directory_file_args {
	const struct dentry_args dentries[PFS_MAX_CHILDREN];
};

struct regular_file_args {
	const char *data; // cstring
};

struct file_args {
	// 0 means end-of-files and 0 permissions mean use default
	mode_t mode;
	union {
		struct directory_file_args dir;
		struct regular_file_args file;
	};
};

void p(bool condition, const char *message)
{
	printf("[%s] %s\n", condition ? " OK " : "FAIL", message);
	if (!condition)
		exit(1);
}

void format_disk(const char *disk_image_path, const struct file_args *files)
{
	int fd;
	const struct file_args *file;
	size_t i;
	ssize_t n_written;
	struct pantryfs_super_block super_block = {
		.version = 1,
		.magic = PANTRYFS_MAGIC_NUMBER,
		.free_inodes = { 0 },
		.free_data_blocks = { 0 },
		.__padding__ = { 0 },
	};
	char inode_buf[PFS_BLOCK_SIZE] = { 0 };
	struct pantryfs_inode *inodes = (struct pantryfs_inode *)inode_buf;

	fd = open(disk_image_path, O_RDWR);
	p(fd != -1, "Error opening the device");
	p(lseek(fd, sizeof(super_block) + sizeof(inode_buf), SEEK_SET) != -1,
	  "Seek past super and inode blocks (writing them at the end)");

	for (file = files, i = 0; file->mode; file++, i++) {
		mode_t perms = file->mode & 0777;
		mode_t type = file->mode & S_IFMT;
		unsigned int nlink;
		uint64_t file_size;
		char data_buf[PFS_BLOCK_SIZE] = { 0 };
		struct timespec current_time = { 0 };

		switch (type) {
		case __S_IFDIR: {
			struct pantryfs_dir_entry *dentries;
			struct pantryfs_dir_entry *dentry;
			const struct dentry_args *dentry_arg;
			size_t j;

			if (!perms)
				perms = 0777;
			nlink = 2; // start with 2 for '.' and link from parent
			dentries = (struct pantryfs_dir_entry *)data_buf;
			for (dentry_arg = file->dir.dentries, j = 0;
			     dentry_arg->filename; dentry_arg++, j++) {
				dentry = &dentries[j];
				dentry->inode_no = PANTRYFS_ROOT_INODE_NUMBER +
						   dentry_arg->index;
				dentry->active = 1;
				strncpy(dentry->filename, dentry_arg->filename,
					sizeof(dentry->filename));
				if (S_ISDIR(files[dentry_arg->index].mode)) {
					// add 1 for each subdirectory '..' link
					nlink++;
				}
			}
			file_size = 0;
			break;
		}
		case __S_IFREG: {
			if (!perms)
				perms = 0666;
			nlink = 1;
			file_size = strlen(file->file.data);
			memcpy(data_buf, file->file.data, file_size);
			break;
		}
		default: {
			p(false, "file type not directory or regular file");
			break;
		}
		}
		clock_gettime(CLOCK_REALTIME, &current_time);

		SETBIT(super_block.free_inodes, i);
		SETBIT(super_block.free_data_blocks, i);
		inodes[i] = (struct pantryfs_inode) {
			.mode = file->mode | perms,
			.uid = getuid(),
			.gid = getgid(),
			.i_atime = current_time,
			.i_mtime = current_time,
			.i_ctime = current_time,
			.nlink = nlink,
			.data_block_number = PANTRYFS_ROOT_DATABLOCK_NUMBER + i,
			.file_size = file_size,
		};
		n_written = write(fd, data_buf, sizeof(data_buf));
		p(n_written == sizeof(data_buf), "Write a data block");
	}

	p(lseek(fd, 0, SEEK_SET) != -1, "Seek back to super and inode blocks");
	n_written = write(fd, (void *)&super_block, sizeof(super_block));
	p(n_written == PFS_BLOCK_SIZE, "Write superblock");
	n_written = write(fd, (void *)&inode_buf, sizeof(inode_buf));
	p(n_written == sizeof(inode_buf), "Write inode block");

	p(fsync(fd) == 0, "Flush writes to disk");

	printf("Device [%s] formatted successfully.\n", disk_image_path);
	close(fd);
}

int main(int argc, char *argv[])
{
	if (argc != 2) {
		printf("Usage: ./%s DEVICE_NAME\n", argv[0]);
		return EXIT_FAILURE;
	}
	const struct file_args files[] = {
		{
			.mode = S_IFDIR,
			.dir = { .dentries = {
				{.filename = "hello.txt", .index = 1},
				{.filename = "members", .index = 2},
			}},
		},
		{
			.mode = S_IFREG,
			.file = { .data = "Hello world\n" },
		},
		{
			.mode = S_IFDIR,
			.dir = { .dentries = {
				{.filename = "names.txt", .index = 3},
			}},
		},
		{
			.mode = S_IFREG,
			.file = { .data = "Isabelle, Khyber, WenChing\n" },
		},
		{0},
	};
	format_disk(argv[1], files);
	return EXIT_SUCCESS;
}
