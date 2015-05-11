#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <linux/fcntl.h>

/* Bypass glibc */
static int openat_(int dirfd, const char *pathname, int flags)
{
	return syscall(__NR_openat, dirfd, pathname, flags);
}

static int openat_or_die(int dfd, const char *path, int flags)
{
	int fd = openat_(dfd, path, flags);

	if (fd < 0) {
		printf("Failed to openat(%d, '%s'); "
			"check prerequisites are available\n", dfd, path);
		exit(1);
	}
	return fd;
}

static int check_openat(int dfd, const char *path, int flags)
{
	int rc;
	int fd;
	struct stat info;
	char buffer[4];

	errno = 0;
	printf("Check success of openat(%d, '%s', %x)... ",
	       dfd, path?:"(null)", flags);
	fd = openat_(dfd, path, flags);
	if (fd < 0) {
		printf("[FAIL]: openat() failed, rc=%d errno=%d (%s)\n",
			fd, errno, strerror(errno));
		return 1;
	}
	if (fstat(fd, &info) != 0) {
		printf("[FAIL]: fstat() failed, rc=%d errno=%d (%s)\n",
			fd, errno, strerror(errno));
		return 1;
	}
	if (!S_ISDIR(info.st_mode)) {
		errno = 0;
		rc = read(fd, buffer, sizeof(buffer));
		if (rc < 0) {
			printf("[FAIL]: read() failed, rc=%d errno=%d (%s)\n",
				rc, errno, strerror(errno));
			return 1;
		}
	}
	close(fd);
	printf("[OK]\n");
	return 0;
}

#define check_openat_fail(dfd, path, flags, errno)	\
	_check_openat_fail(dfd, path, flags, errno, #errno)
static int _check_openat_fail(int dfd, const char *path, int flags,
			      int expected_errno, const char *errno_str)
{
	int rc;

	errno = 0;
	printf("Check failure of openat(%d, '%s', %x) with %s... ",
		dfd, path?:"(null)", flags, errno_str);
	rc = openat_(dfd, path, flags);
	if (rc > 0) {
		printf("[FAIL] (unexpected success from openat(2))\n");
		close(rc);
		return 1;
	}
	if (errno != expected_errno) {
		printf("[FAIL] (expected errno %d (%s) not %d (%s)\n",
			expected_errno, strerror(expected_errno),
			errno, strerror(errno));
		return 1;
	}
	printf("[OK]\n");
	return 0;
}

int check_proc(void)
{
	int root_dfd = openat_(AT_FDCWD, "/", O_RDONLY);
	int proc_dfd = openat_(AT_FDCWD, "/proc/self", O_RDONLY);
	int fail = 0;

	if (proc_dfd < 0) {
		printf("'/proc/self' unavailable (errno=%d '%s'), skipping\n",
			errno, strerror(errno));
		return 0;
	}
	fail += check_openat(proc_dfd, "root/etc/passwd", O_RDONLY);
	fail += check_openat(root_dfd, "proc/self/root/etc/passwd", O_RDONLY);
#ifdef O_BENEATH
	fail += check_openat_fail(proc_dfd, "root/etc/passwd",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(root_dfd, "proc/self/root/etc/passwd",
				O_RDONLY|O_BENEATH, EPERM);
#endif
	return fail;
}

int main(int argc, char *argv[])
{
	int fail = 0;
	int dot_dfd = openat_or_die(AT_FDCWD, ".", O_RDONLY);
	int subdir_dfd = openat_or_die(AT_FDCWD, "subdir", O_RDONLY);
	int file_fd = openat_or_die(AT_FDCWD, "topfile", O_RDONLY);

	/* Sanity check normal behavior */
	fail += check_openat(AT_FDCWD, "topfile", O_RDONLY);
	fail += check_openat(AT_FDCWD, "subdir/bottomfile", O_RDONLY);

	fail += check_openat(dot_dfd, "topfile", O_RDONLY);
	fail += check_openat(dot_dfd, "subdir/bottomfile", O_RDONLY);
	fail += check_openat(dot_dfd, "subdir/../topfile", O_RDONLY);

	fail += check_openat(subdir_dfd, "../topfile", O_RDONLY);
	fail += check_openat(subdir_dfd, "bottomfile", O_RDONLY);
	fail += check_openat(subdir_dfd, "../subdir/bottomfile", O_RDONLY);
	fail += check_openat(subdir_dfd, "symlinkup", O_RDONLY);
	fail += check_openat(subdir_dfd, "symlinkout", O_RDONLY);

	fail += check_openat(AT_FDCWD, "/etc/passwd", O_RDONLY);
	fail += check_openat(dot_dfd, "/etc/passwd", O_RDONLY);
	fail += check_openat(subdir_dfd, "/etc/passwd", O_RDONLY);

	fail += check_openat_fail(AT_FDCWD, "bogus", O_RDONLY, ENOENT);
	fail += check_openat_fail(dot_dfd, "bogus", O_RDONLY, ENOENT);
	fail += check_openat_fail(999, "bogus", O_RDONLY, EBADF);
	fail += check_openat_fail(file_fd, "bogus", O_RDONLY, ENOTDIR);

#ifdef O_BENEATH
	/* Test out O_BENEATH */
	fail += check_openat(AT_FDCWD, "topfile", O_RDONLY|O_BENEATH);
	fail += check_openat(AT_FDCWD, "subdir/bottomfile",
			     O_RDONLY|O_BENEATH);

	fail += check_openat(dot_dfd, "topfile", O_RDONLY|O_BENEATH);
	fail += check_openat(dot_dfd, "subdir/bottomfile",
			     O_RDONLY|O_BENEATH);
	fail += check_openat(subdir_dfd, "bottomfile", O_RDONLY|O_BENEATH);
	fail += check_openat(subdir_dfd, ".", O_RDONLY|O_BENEATH);

	/* Symlinks without .. or leading / are OK */
	fail += check_openat(dot_dfd, "symlinkdown", O_RDONLY|O_BENEATH);
	fail += check_openat(dot_dfd, "subdir/symlinkin", O_RDONLY|O_BENEATH);
	fail += check_openat(subdir_dfd, "symlinkin", O_RDONLY|O_BENEATH);
	/* ... unless of course we specify O_NOFOLLOW */
	fail += check_openat_fail(dot_dfd, "symlinkdown",
				  O_RDONLY|O_BENEATH|O_NOFOLLOW, ELOOP);
	fail += check_openat_fail(dot_dfd, "subdir/symlinkin",
				  O_RDONLY|O_BENEATH|O_NOFOLLOW, ELOOP);
	fail += check_openat_fail(subdir_dfd, "symlinkin",
				  O_RDONLY|O_BENEATH|O_NOFOLLOW, ELOOP);

	/* Can't open paths with ".." in them */
	fail += check_openat_fail(dot_dfd, "subdir/../topfile",
				O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "../topfile",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "../subdir/bottomfile",
				O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "..", O_RDONLY|O_BENEATH, EPERM);

	/* Can't open paths starting with "/" */
	fail += check_openat_fail(AT_FDCWD, "/etc/passwd",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(dot_dfd, "/etc/passwd",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "/etc/passwd",
				  O_RDONLY|O_BENEATH, EPERM);
	/* Can't sneak around constraints with symlinks */
	fail += check_openat_fail(subdir_dfd, "symlinkup",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "symlinkout",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(subdir_dfd, "../symlinkdown",
				  O_RDONLY|O_BENEATH, EPERM);
	fail += check_openat_fail(dot_dfd, "subdir/symlinkup",
				O_RDONLY|O_BENEATH, EPERM);
#else
	printf("Skipping O_BENEATH tests due to missing #define\n");
#endif
	fail += check_proc();

	if (fail > 0)
		printf("%d tests failed\n", fail);
	return fail;
}
