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
	uint8_t inode_offset;
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

bool p(bool ok, const char *message)
{
	printf("[%s] %s\n", ok ? " OK " : "FAIL", message);
	return ok;
}

int format_disk(const char *disk_image_path, const struct file_args *files)
{
	int e = 0;
	ssize_t n_written;

	const int fd = open(disk_image_path, O_RDWR);
	if (fd == -1) {
		e = -1;
		perror("Error opening the device");
		goto ret;
	}

	struct stat stats = { 0 };

	if (fstat(fd, &stats) == -1) {
		e = -1;
		goto ret;
	}

	struct pantryfs_super_block super_block = {
		.version = 1,
		.magic = PANTRYFS_MAGIC_NUMBER,
		.free_inodes = { 0 },
		.free_data_blocks = { 0 },
		.__padding__ = { 0 },
	};

	char inode_buf[PFS_BLOCK_SIZE] = { 0 };
	struct pantryfs_inode *inodes = (struct pantryfs_inode *)inode_buf;

	if (!p(lseek(fd, sizeof(super_block) + sizeof(inode_buf), SEEK_SET) !=
		       -1,
	       "Seek past super and inode blocks (writing them at the end)")) {
		e = -1;
		goto restore_file;
	}

	const struct file_args *file;
	size_t i;

	for (file = files, i = 0; file->mode; file++, i++) {
		mode_t perms = file->mode & 0777;
		mode_t type = file->mode & S_IFMT;
		unsigned int nlink;
		uint64_t file_size;
		char data_buf[PFS_BLOCK_SIZE] = { 0 };

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
						   dentry_arg->inode_offset;
				dentry->active = 1;
				strncpy(dentry->filename, dentry_arg->filename,
					sizeof(dentry->filename));
				if (S_ISDIR(files[dentry_arg->inode_offset]
						    .mode)) {
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
			fprintf(stderr,
				"unknown file type: not directory or regular file: %o, i = %zu\n",
				file->mode & S_IFMT, i);
			e = -1;
			goto restore_file;
		}
		}

		struct timespec current_time = { 0 };

		clock_gettime(CLOCK_REALTIME, &current_time);

		struct pantryfs_inode inode = {
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

		SETBIT(super_block.free_inodes, i);
		SETBIT(super_block.free_data_blocks, i);
		inodes[i] = inode;

		n_written = write(fd, data_buf, sizeof(data_buf));
		if (!p(n_written == sizeof(data_buf), "Write a data block")) {
			e = -1;
			goto restore_file;
		}
	}

	if (!p(lseek(fd, 0, SEEK_SET) != -1,
	       "Seek back to super and inode blocks")) {
		e = -1;
		goto restore_file;
	}
	n_written = write(fd, (void *)&super_block, sizeof(super_block));
	if (!p(n_written == PFS_BLOCK_SIZE, "Write superblock")) {
		e = -1;
		goto ret;
	}
	n_written = write(fd, (void *)&inode_buf, sizeof(inode_buf));
	if (!p(n_written == sizeof(inode_buf), "Write inode block")) {
		e = -1;
		goto ret;
	}

	if (!(p(fsync(fd) == 0, "Flush writes to disk"))) {
		e = -1;
		goto ret;
	}

	printf("Device [%s] formatted successfully.\n", disk_image_path);
	goto close;

restore_file:
	ftruncate(fd, stats.st_size);
close:
	close(fd);
ret:
	return e;
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
			.dir = {
				.dentries = {
					{
						.filename = "hello.txt",
						.inode_offset = 1,
					},
					{
						.filename = "members",
						.inode_offset = 2,
					},
				},
			},
		},
		{
			.mode = S_IFREG,
			.file = {
				.data = "Hello world\n",
			},
		},
		{
			.mode = S_IFDIR,
			.dir = {
				.dentries = {
					{
						.filename = "names.txt",
						.inode_offset = 3,
					},
				},
			},
		},
		{
			.mode = S_IFREG,
			.file = {
				.data = "Isabelle, Khyber, WenChing\n",
			},
		},
		{0},
	};
	if (format_disk(argv[1], files) == -1) {
		perror("");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
