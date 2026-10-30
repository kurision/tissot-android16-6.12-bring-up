#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct stat st;
	void *policy;
	int in, out, result = 1;
	ssize_t written;

	if (argc != 2) {
		fprintf(stderr, "usage: %s POLICY\n", argv[0]);
		return 2;
	}
	in = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		perror("policy open");
		return 1;
	}
	if (fstat(in, &st)) {
		perror("policy stat");
		close(in);
		return 1;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uintmax_t)st.st_size > SIZE_MAX / 2) {
		fprintf(stderr, "policy must be a nonempty regular file of representable size\n");
		close(in);
		return 1;
	}
	policy = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, in, 0);
	close(in);
	if (policy == MAP_FAILED) {
		perror("policy mmap");
		return 1;
	}
	out = open("/sys/fs/selinux/load", O_WRONLY | O_CLOEXEC);
	if (out < 0) {
		perror("selinux load open");
		goto unmap;
	}
	/* selinuxfs parses one complete policy, not a sequence of chunks. */
	do {
		written = write(out, policy, (size_t)st.st_size);
	} while (written < 0 && errno == EINTR);
	if (written < 0)
		perror("policy load");
	else if (written != st.st_size)
		fprintf(stderr, "policy load returned %zd, expected %jd bytes\n",
			written, (intmax_t)st.st_size);
	else
		result = 0;
	close(out);
unmap:
	munmap(policy, (size_t)st.st_size);
	return result;
}
