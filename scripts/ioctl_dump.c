#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define OUICHEFS_BLOCK_SIZE 4096
#define OUICHEFS_SLICES_PER_BLOCK 32
#define DUMP_BLOCK \
	_IOR('D', 1, char[OUICHEFS_BLOCK_SIZE + OUICHEFS_SLICES_PER_BLOCK + 1])

int main(int argc, char *argv[])
{
	char buf[OUICHEFS_BLOCK_SIZE + OUICHEFS_SLICES_PER_BLOCK + 1];
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (ioctl(fd, DUMP_BLOCK, buf) < 0) {
		perror("ioctl");
		return 1;
	}
	printf("%s", buf);
	// // Print 32 lines of 128 chars each
	// for (int i=0; i<OUICHEFS_SLICES_PER_BLOCK; ++i)
	//     fwrite(buf + i*128, 1, 128, stdout), putchar('\n');
	close(fd);
	return 0;
}
