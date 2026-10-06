/*
 * Diagnostic initramfs /init for the tissot ACK 6.12 port.
 *
 * Capture /dev/kmsg to the partition identified by PARTNAME=logdump, or
 * stream it through a CDC-ACM gadget if storage is unavailable. devtmpfs
 * provides /dev/<device>, not Android's /dev/block hierarchy.
 * Default mode never returns after setup; recovery requires a reboot.
 * Build with -DANDROID_CAPTURE to exec the original /system/bin/init as
 * PID 1 while a child records for 180 seconds in a private mount namespace.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define G "/sys/kernel/config/usb_gadget/g1"
#define POLL_TRIES 120 /* 60 seconds for storage, then 60 seconds for USB */
#ifdef HEARTBEAT_RECORDER
#define DUMP_SECONDS 300
#define DURABLE_CAPTURE 1
#else
#define DUMP_SECONDS 45
#define DURABLE_CAPTURE 0
#endif
static time_t monotonic_seconds(void);

static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t n = write(fd, buf, len);
		if (n > 0) {
			buf += n;
			len -= (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else if (n < 0 && errno == EAGAIN) {
			/* A closed ACM port reports POLLHUP on every wake, so
			 * a single poll() can never be trusted: keep polling
			 * until writable or the deadline really expires. */
			struct pollfd p = { .fd = fd, .events = POLLOUT };
			time_t left = 30;
			while (left > 0) {
				time_t before = monotonic_seconds();
				int r = poll(&p, 1, 1000);
				if (r > 0 && (p.revents & POLLOUT))
					break;
				left -= monotonic_seconds() - before;
			}
			if (left > 0)
				continue;
			return -1;
		} else {
			return -1;
		}
	}
	return 0;
}

static void say(int fd, const char *s)
{
	write_all(fd, s, strlen(s));
}

static int attribute(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	int ret;

	if (fd < 0)
		return -1;
	ret = write_all(fd, value, strlen(value));
	close(fd);
	return ret;
}

static int make_dir(const char *path)
{
	return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int setup_gadget(void)
{
	DIR *dir;
	struct dirent *entry;
	int ret = -1;

	if (make_dir(G) || make_dir(G "/strings/0x409") ||
	    make_dir(G "/configs/c.1") || make_dir(G "/functions/acm.gs0") ||
	    attribute(G "/idVendor", "0x18d1") ||
	    attribute(G "/idProduct", "0x9008") ||
	    attribute(G "/strings/0x409/serialnumber", "tissot-diag") ||
	    attribute(G "/strings/0x409/manufacturer", "Diagnostic") ||
	    attribute(G "/strings/0x409/product", "ACK kernel log"))
		return -1;
	if (symlink(G "/functions/acm.gs0", G "/configs/c.1/f1") &&
	    errno != EEXIST)
		return -1;
	dir = opendir("/sys/class/udc");
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.')
			continue;
		ret = attribute(G "/UDC", entry->d_name);
		if (!ret)
			break;
	}
	closedir(dir);
	return ret;
}

static int open_logdump(void)
{
	DIR *dir = opendir("/sys/class/block");
	struct dirent *entry;
	int out = -1;
	char path[512], line[512];

	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		FILE *file;
		int match = 0;

		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "/sys/class/block/%s/uevent",
			 entry->d_name);
		file = fopen(path, "r");
		if (!file)
			continue;
		while (fgets(line, sizeof line, file))
			if (!strcmp(line, "PARTNAME=logdump\n"))
				match = 1;
		fclose(file);
		if (!match)
			continue;
		snprintf(path, sizeof path, "/dev/%s", entry->d_name);
		out = open(path, O_WRONLY);
		if (out >= 0) {
			struct stat st;
			if (!fstat(out, &st) && S_ISBLK(st.st_mode))
				break;
			close(out);
			out = -1;
		}
	}
	closedir(dir);
	return out;
}

/* Block until the host has set the gadget configuration (i.e. actually
 * attached and opened the port); otherwise the ACM buffer fills up and the
 * stream is lost to a late host. The controller name comes from g1/UDC. */
static void wait_usb_configured(void)
{
	char name[64], path[128];
	char state[32];
	FILE *file;
	int n;

	file = fopen(G "/UDC", "r");
	if (!file || !fgets(name, sizeof name, file)) {
		if (file)
			fclose(file);
		return;
	}
	fclose(file);
	n = strlen(name);
	while (n && (name[n - 1] == '\n' || name[n - 1] == ' '))
		name[--n] = '\0';
	if (!n)
		return;
	snprintf(path, sizeof path, "/sys/class/udc/%s/state", name);
	for (n = 0; n < POLL_TRIES; n++) {
		file = fopen(path, "r");
		if (file && fgets(state, sizeof state, file) &&
		    !strncmp(state, "configured", 10)) {
			fclose(file);
			return;
		}
		if (file)
			fclose(file);
		usleep(500000);
	}
}

static int open_usb_sink(void)
{
	int out, n, bound = 0;
	for (n = 0; n < POLL_TRIES; n++) {
		if (!bound)
			bound = setup_gadget() == 0;
		out = bound ? open("/dev/ttyGS0", O_WRONLY | O_NONBLOCK |
				  O_NOCTTY) : -1;
		if (out >= 0) {
			struct termios tty;
			if (tcgetattr(out, &tty) == 0) {
				cfmakeraw(&tty);
				if (tcsetattr(out, TCSANOW, &tty) == 0) {
					wait_usb_configured();
					return out;
				}
			}
			close(out);
		}
		usleep(500000);
	}
	return -1;
}

static int open_sink(int console, int *usb)
{
	int out, n;

	for (n = 0; n < POLL_TRIES; n++) {
		out = open_logdump();
		if (out >= 0)
			return out;
		usleep(500000);
	}
	say(console, "DIAG: no logdump after 60s; trying USB\n");
	out = open_usb_sink();
	if (out >= 0)
		*usb = 1;
	return out;
}

static time_t monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return now.tv_sec;
}

static int dump_log(int in, int out, unsigned seconds, int durable)
{
	time_t deadline = monotonic_seconds() + seconds;
	char buf[8192];

	do {
		ssize_t got = read(in, buf, sizeof buf);
		if (got > 0) {
			if (write_all(out, buf, (size_t)got) ||
			    (durable && fsync(out)))
				return -1;
		} else if (got < 0 && errno == EPIPE) {
			say(out, "\nDIAG: kmsg overrun; some records lost\n");
		} else if (got < 0 && errno != EAGAIN && errno != EINTR) {
			return -1;
		} else {
			usleep(200000);
		}
	} while (monotonic_seconds() <= deadline);
	return 0;
}

#ifdef ANDROID_CAPTURE
static void report_block_path(int log, const char *path)
{
	struct stat st, resolved;
	char target[256] = "-", label[256], message[1024];
	ssize_t n;
	int label_error = 0, resolve_error = 0;

	if (lstat(path, &st)) {
		snprintf(message, sizeof message,
			 "DIAG: block path=%s lstat_errno=%d\n", path, errno);
		say(log, message);
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		n = readlink(path, target, sizeof target - 1);
		if (n < 0)
			snprintf(target, sizeof target, "readlink_errno=%d", errno);
		else
			target[n] = '\0';
	}
	if (stat(path, &resolved))
		resolve_error = errno;
	n = lgetxattr(path, "security.selinux", label, sizeof label - 1);
	if (n < 0) {
		label_error = errno;
		label[0] = '\0';
	} else {
		label[n] = '\0';
	}
	snprintf(message, sizeof message,
		 "DIAG: block path=%s mode=%o dev=%u:%u target=%s"
		 " stat_errno=%d label=%s label_errno=%d\n",
		 path, (unsigned)st.st_mode, major(st.st_rdev), minor(st.st_rdev),
		 target, resolve_error, label, label_error);
	say(log, message);
}

static void snapshot_block_dir(int log, const char *path)
{
	DIR *dir;
	struct dirent *entry;
	char child[512], message[640];
	int n, error;

	report_block_path(log, path);
	dir = opendir(path);
	if (!dir) {
		snprintf(message, sizeof message,
			 "DIAG: block directory=%s opendir_errno=%d\n", path, errno);
		say(log, message);
		return;
	}
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (!entry) {
			error = errno;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		n = snprintf(child, sizeof child, "%s/%s", path, entry->d_name);
		if (n < 0 || (size_t)n >= sizeof child) {
			say(log, "DIAG: block entry path exceeds buffer\n");
			continue;
		}
		report_block_path(log, child);
	}
	if (error) {
		snprintf(message, sizeof message,
			 "DIAG: block directory=%s readdir_errno=%d\n", path, error);
		say(log, message);
	}
	closedir(dir);
}

/* Only this observer enters Android's namespace/root; it never changes
 * Android's mounts, links, labels or enforcement. Log through an inherited fd. */
static void capture_block_state(int log)
{
	static const char *const paths[] = {
		"/dev/block", "/dev/block/by-name", "/dev/block/platform",
		"/dev/block/platform/soc", "/dev/block/platform/soc@0",
		"/dev/block/platform/soc@0/7824900.mmc",
		"/dev/block/platform/soc@0/7824900.mmc/by-name",
		"/dev/block/bootdevice", "/dev/block/bootdevice/by-name",
	};
	int proc = -1, ns = -1, root = -1, sample;
	size_t i;
	char message[160];
	const char *step = "open procfs";

	/* Keep procfs reachable even if init removes our old root's /proc path. */
	proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (proc < 0)
		goto failed;
	while (monotonic_seconds() < 75)
		sleep(1);
	step = "open mount namespace";
	ns = openat(proc, "1/ns/mnt", O_RDONLY | O_CLOEXEC);
	if (ns < 0)
		goto failed;
	step = "open root";
	root = openat(proc, "1/root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root < 0)
		goto failed;
	step = "setns";
	if (setns(ns, CLONE_NEWNS))
		goto failed;
	step = "fchdir root";
	if (fchdir(root))
		goto failed;
	step = "chroot";
	if (chroot("."))
		goto failed;
	step = "chdir";
	if (chdir("/"))
		goto failed;
	close(ns);
	close(root);
	close(proc);
	for (sample = 0; sample < 2; sample++) {
		while (monotonic_seconds() < 75 + sample * 30)
			sleep(1);
		snprintf(message, sizeof message, "DIAG: block snapshot begin t=%ld\n",
			 (long)monotonic_seconds());
		say(log, message);
		for (i = 0; i < sizeof paths / sizeof paths[0]; i++)
			snapshot_block_dir(log, paths[i]);
		say(log, "DIAG: block snapshot end\n");
	}
	_exit(0);
failed:
	snprintf(message, sizeof message,
		 "DIAG: block observer step=%s failed errno=%d\n", step, errno);
	say(log, message);
	if (ns >= 0)
		close(ns);
	if (root >= 0)
		close(root);
	if (proc >= 0)
		close(proc);
	_exit(1);
}

/* kmsg fds are opened by the parent at t~=1s (pre-enforcing, where opens
 * provably succeed) and inherited across the fork; opening them here later
 * fails with ENOENT once Android init has switch-rooted around us. */
static void capture_usb(int kin, int klog, int kerr)
{
	int out;
	char msg[160];

	mkdir("/sys/kernel/config", 0755);
	mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
	out = open_usb_sink();
	if (out < 0) {
		if (klog >= 0)
			say(klog, "DIAG: Android USB capture unavailable; storage continues\n");
		_exit(0);
	}
	/* printk.devkmsg=on keeps the full ring from second 0, so a late
	 * host attach still receives the whole boot log. */
	if (klog >= 0)
		say(klog, "DIAG: independent Android USB capture ready\n");
	if (kin < 0) {
		snprintf(msg, sizeof msg,
			 "DIAG: USB kmsg unavailable errno=%d\n", kerr);
		say(out, msg);
	} else if (dump_log(kin, out, 300, 0)) {
		snprintf(msg, sizeof msg,
			 "DIAG: USB dump failed errno=%d\n", errno);
		say(out, msg);
	} else {
		say(out, "\nDIAG: EOF Android USB kmsg dump complete\n");
	}
	_exit(0);
}

static int launch_android(char **argv)
{
	int ready[2] = { -1, -1 };
	pid_t child;
	char status = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ready))
		goto exec_init;
	child = fork();
	if (child == 0) {
		int in = -1, out = -1, kin = -1, klog = -1, kerr = 0, n;
		static const char marker[] =
			"DIAG: Android capture ready; handing PID 1 to Android init\n";

		close(ready[0]);
		/* Never expose the logger's mounts to Android's first-stage init. */
		if (unshare(CLONE_NEWNS) ||
		    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
		    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) ||
		    mount("proc", "/proc", "proc", 0, NULL) ||
		    mount("sysfs", "/sys", "sysfs", 0, NULL))
			goto child_failed;
		in = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
		if (in < 0)
			goto child_failed;
		/* A second reader plus a writer for the USB child, opened now:
		 * late opens fail once init has switch-rooted around us. */
		kin = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
		kerr = kin < 0 ? errno : 0;
		klog = open("/dev/kmsg", O_WRONLY);
		if (klog < 0)
			kerr = errno;
		for (n = 0; n < POLL_TRIES; n++) {
			out = open_logdump();
			if (out >= 0)
				break;
			usleep(500000);
		}
		if (out < 0 || write_all(out, marker, sizeof marker - 1) ||
		    fsync(out))
			goto child_failed;
		if (fork() == 0) {
			close(ready[1]);
			close(in);
			close(out);
			capture_usb(kin, klog, kerr);
		}
		if (klog >= 0 && fork() == 0) {
			close(ready[1]);
			close(in);
			close(out);
			if (kin >= 0)
				close(kin);
			capture_block_state(klog);
		}
		status = 1;
		write_all(ready[1], &status, 1);
		{
			int error = 0;
			ssize_t received;
			char message[128];
			do {
				received = recv(ready[1], &error, sizeof error, MSG_WAITALL);
			} while (received < 0 && errno == EINTR);
			if (received == 0)
				say(out, "DIAG: Android init exec succeeded\n");
			else {
				snprintf(message, sizeof message,
					 "DIAG: Android init exec failed or handshake lost: errno=%d\n",
					 error);
				say(out, message);
			}
			fsync(out);
		}
		close(ready[1]);
		if (dump_log(in, out, 300, 1))
			say(out, "\nDIAG: Android capture failed\n");
		else
			say(out, "\nDIAG: EOF Android kmsg dump complete\n");
		fsync(out);
		close(out);
		close(in);
		_exit(0);
child_failed:
		write_all(ready[1], &status, 1);
		_exit(1);
	}
	close(ready[1]);
	if (child > 0) {
		ssize_t n;
		do {
			n = read(ready[0], &status, 1);
		} while (n < 0 && errno == EINTR);
	}
	if (!status) {
		close(ready[0]);
		ready[0] = -1;
	}
	if (!status)
		say(STDERR_FILENO, "DIAG: recorder failed; continuing Android init\n");
exec_init:
	execv("/system/bin/init", argv);
	/* Report exec failure through the recorder's already-open log sink. */
	{
		int error = errno;
		if (ready[0] >= 0) {
			send(ready[0], &error, sizeof error, MSG_NOSIGNAL);
			close(ready[0]);
		}
		dprintf(STDERR_FILENO, "DIAG: Android init exec failed: errno=%d\n",
			error);
	}
	for (;;)
		pause();
}
#endif

int main(int argc, char **argv)
{
	int in = -1, out = -1, usb = 0, console, n;

	(void)argc;
	(void)argv;
#ifdef ANDROID_CAPTURE
	return launch_android(argv);
#endif
#ifdef HEARTBEAT_RECORDER
	if (argc == 2 && !strcmp(argv[1], "--usb-setup")) {
		/* PID 1 owns /dev, /proc and /sys; do not hide its later mounts. */
		mkdir("/sys/kernel/config", 0755);
		mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
		for (n = 0; n < POLL_TRIES; n++) {
			if (!setup_gadget()) {
				dprintf(STDERR_FILENO, "HB: USB serial gadget bound\n");
				return 0;
			}
			usleep(500000);
		}
		dprintf(STDERR_FILENO, "HB: USB serial unavailable; storage capture continues\n");
		return 1;
	}
#endif

	mkdir("/dev", 0755);
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mkdir("/sys/kernel/config", 0755);
	mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
	console = open("/dev/console", O_WRONLY);
	say(console, "DIAG: init running\n");

	in = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (in < 0) {
		say(console, "DIAG: cannot open /dev/kmsg\n");
		goto spin;
	}
	out = open_sink(console, &usb);
	if (out < 0) {
		say(console, "DIAG: no storage or USB sink\n");
		goto spin;
	}
	/* Allow time for the host to configure USB before consuming kmsg. */
	for (n = 0; n < (usb ? POLL_TRIES : 1); n++) {
		static const char marker[] = "DIAG: init running; kernel log follows\n";
		if (!write_all(out, marker, sizeof marker - 1))
			break;
		usleep(500000);
	}
	if (n == (usb ? POLL_TRIES : 1)) {
		say(console, "DIAG: sink write failed\n");
		goto spin;
	}
	if (dump_log(in, out, DUMP_SECONDS, DURABLE_CAPTURE && !usb)) {
		say(console, "DIAG: kernel log capture failed\n");
		goto spin;
	}
	say(out, "\nDIAG: EOF kmsg dump complete\n");
	if (!usb && fsync(out))
		say(console, "DIAG: logdump fsync failed\n");
	say(console, "DIAG: dump finished\n");

spin:
	/* Stay alive even if no sink works; absence of a reset alone is not proof. */
	say(console, "DIAG: looping forever\n");
	for (;;)
		pause();
	return 0;
}