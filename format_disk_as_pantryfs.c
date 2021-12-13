#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

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

void passert(int condition, const char *message)
{
	printf("[%s] %s\n", condition ? " OK " : "FAIL", message);
	if (!condition)
		exit(1);
}

#define write_block(fd, buf, msg)                                              \
	passert(write(fd, (void *)&buf, sizeof(buf)) == sizeof(buf),           \
		"Write " msg " block");
#define seek_block(fd, n, msg)                                                 \
	passert(lseek(fd, (n) * PFS_BLOCK_SIZE, SEEK_SET) != -1, "Seek " msg)

void format_disk(const char *disk_image_path, const struct file_args *files)
{
	int fd;
	const struct file_args *file;
	size_t i;
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
	passert(fd != -1, "Error opening the device");
	seek_block(fd, 2, "past super and inode blocks");

	for (file = files, i = 0; file->mode; file++, i++) {
		mode_t perms = file->mode & 0777;
		unsigned int nlink;
		uint64_t file_size;
		char data_buf[PFS_BLOCK_SIZE] = { 0 };
		struct timespec current_time = { 0 };

		if (S_ISDIR(file->mode)) {
			struct pantryfs_dir_entry *dentry;
			const struct dentry_args *arg;

			if (!perms)
				perms = 0777;
			nlink = 2; // start with 2 for '.' and link from parent
			for (arg = file->dir.dentries,
			    dentry = (struct pantryfs_dir_entry *)data_buf;
			     arg->filename; arg++, dentry++) {
				dentry->inode_no =
					PANTRYFS_ROOT_INODE_NUMBER + arg->index;
				dentry->active = 1;
				strncpy(dentry->filename, arg->filename,
					sizeof(dentry->filename));
				// add 1 for each subdirectory '..' link
				if (S_ISDIR(files[arg->index].mode))
					nlink++;
			}
			file_size = 0;
		} else if (S_ISREG(file->mode)) {
			if (!perms)
				perms = 0666;
			nlink = 1;
			file_size = strlen(file->file.data);
			memcpy(data_buf, file->file.data, file_size);
		} else {
			passert(0, "file type not directory or regular file");
		}
		clock_gettime(CLOCK_REALTIME, &current_time);

		SETBIT(super_block.free_inodes, i);
		SETBIT(super_block.free_data_blocks, i);
		inodes[i] = (struct pantryfs_inode){
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
		write_block(fd, data_buf, "a data");
	}

	seek_block(fd, 0, "back to super and inode blocks");
	write_block(fd, super_block, "super");
	write_block(fd, inode_buf, "inode");
	passert(fsync(fd) == 0, "Flush writes to disk");
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
			.dir = {.dentries = {
				{.filename = "hello.txt", .index = 1},
				{.filename = "members", .index = 2},
				{.filename = "3", .index = 3},
				{.filename = "4", .index = 4},
				{.filename = "5", .index = 5},
				{.filename = "6", .index = 6},
				{.filename = "7", .index = 7},
				{.filename = "8", .index = 8},
				{.filename = "9", .index = 9},
				{0},
			}},
		},
		{
			.mode = S_IFREG,
			.file = {.data = "Hello world!\n"},
		},
		{
			.mode = S_IFDIR,
			.dir = {.dentries = {
				{.filename = "names.txt", .index = 3},
				{0},
			}},
		},
		{
			.mode = S_IFREG,
			.file = {.data = "Isabelle\nKhyber\nWenChing\n"},
		},
		{.mode = S_IFREG, .file = {.data = "3"}},
		{.mode = S_IFREG, .file = {.data = "4"}},
		{.mode = S_IFREG, .file = {.data = "5"}},
		{.mode = S_IFREG, .file = {.data = "6"}},
		{.mode = S_IFREG, .file = {.data = "7"}},
		{.mode = S_IFREG, .file = {.data = "8"}},
		{.mode = S_IFREG, .file = {.data = "9"}},
		{0},
	};
	format_disk(argv[1], files);
	return EXIT_SUCCESS;
}
