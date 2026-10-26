/*
 * Diagnostic initramfs /init for the tissot ACK 6.12 port.
 *
 * Capture /dev/kmsg to the partition identified by PARTNAME=logdump, or
 * stream it through a CDC-ACM gadget if storage is unavailable. devtmpfs
 * provides /dev/<device>, not Android's /dev/block hierarchy.
 * Default mode never returns after setup; recovery requires a reboot.
 * Build with -DANDROID_CAPTURE to exec the original /system/bin/init as
 * PID 1 while a child records for 900 seconds in a private mount namespace,
 * and, when built with -DBOOT_DEADLINE_SECONDS=<n> (n > 0), a watchdog
 * child reboots the phone if Android has not reached sys.boot_completed=1
 * within n seconds of kernel start. Off by default: since snapshot60 run31
 * the 6.12 boot reaches adb, so the host ends a run with `adb reboot`.
 * Add -DANDROID_EARLY_LOGCAT for an init-managed logcat collector starting
 * at post-fs-data, writing bounded logs in /data/local/tmp without ADB.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(ANDROID_CAPTURE) && \
	(defined(ANDROID_GUARD_TEST) || defined(ANDROID_RAMOOPS_TEST))
#include <assert.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/resource.h>
#endif

#define G "/sys/kernel/config/usb_gadget/g1"
#define POLL_TRIES 120 /* 60 seconds for storage, then 60 seconds for USB */

/*
 * Snapshot60 run100/101: the SD card can wedge its MMC controller at init
 * ("Card stuck being busy", -110), stalling mount_all --early for 100 s
 * and cascading into the HAL storm. logdump (kernel-side, always on)
 * carried every verdict, so card logging is opt-in now: define
 * ANDROID_SD_MIRROR to restore the SD mirror behavior.
 */
#ifndef ANDROID_SD_MIRROR
#define POLL_SD_TRIES 1
#else
#define POLL_SD_TRIES POLL_TRIES
#endif
#ifdef HEARTBEAT_RECORDER
#define DUMP_SECONDS 300
#define DURABLE_CAPTURE 1
#else
#define DUMP_SECONDS 45
#define DURABLE_CAPTURE 0
#endif
#ifndef BOOT_DEADLINE_SECONDS
#define BOOT_DEADLINE_SECONDS 0
#endif
#if defined(RESET_PATH_DIAG) && defined(ANDROID_CAPTURE)
#error RESET_PATH_DIAG must not launch Android before retained RAM is recovered
#endif
#if defined(RESET_PANIC_TEST) && !defined(RESET_PATH_DIAG)
#error RESET_PANIC_TEST requires the standalone RESET_PATH_DIAG init
#endif
#define LOGDUMP_TEXT_LIMIT	(60UL * 1024 * 1024)
static time_t monotonic_seconds(void);
struct ramoops_sampler;

static int write_all_limit(int fd, const char *buf, size_t len,
			   size_t *remaining)
{
	while (len) {
		size_t count = len;
		ssize_t n;

		if (remaining) {
			if (!*remaining) {
				errno = ENOSPC;
				return -1;
			}
			if (count > *remaining)
				count = *remaining;
		}
		n = write(fd, buf, count);
		if (n > 0) {
			buf += n;
			len -= (size_t)n;
			if (remaining)
				*remaining -= (size_t)n;
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

static int write_all(int fd, const char *buf, size_t len)
{
	return write_all_limit(fd, buf, len, NULL);
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

/*
 * The diag configuration is c.2: configfs refuses a second configuration
 * with the same bConfigurationValue (usb_add_config_only, -EBUSY), and
 * Android creates b.1 in this same gadget (snapshot47 run1).
 *
 * Creating g1 is unconditional; binding it to a UDC is not, and that split is
 * the whole point.
 *
 *   - bind_udc=0 (default build): g1 exists for the vendor stack to add c.1
 *     to, but the UDC is left alone so adbd can bind it and adb enumerates.
 *   - bind_udc=1 (ANDROID_USB_CONSOLE): the ACM console works too, but this
 *     process then holds the board's only UDC and adbd's bind can never
 *     succeed.
 *
 * 24bcdc3ed put this whole function behind #ifdef ANDROID_USB_CONSOLE, so in
 * the default build g1 was never created at all: run234b reached adb but never
 * reached the ROM, while run234 (same kernel, old init) reached the ROM and
 * had no adb. Wrapping the function rather than the bind removed the gadget
 * Android needs, which is why the function has to compile unconditionally.
 */
static int setup_gadget(int bind_udc)
{
	DIR *dir;
	struct dirent *entry;
	int ret = 0;

	if (make_dir(G) || make_dir(G "/strings/0x409") ||
	    make_dir(G "/configs/c.2") || make_dir(G "/functions/acm.gs0") ||
	    attribute(G "/idVendor", "0x18d1") ||
	    attribute(G "/idProduct", "0x9008") ||
	    attribute(G "/strings/0x409/serialnumber", "tissot-diag") ||
	    attribute(G "/strings/0x409/manufacturer", "Diagnostic") ||
	    attribute(G "/strings/0x409/product", "ACK kernel log"))
		return -1;
	if (symlink(G "/functions/acm.gs0", G "/configs/c.2/f1") &&
	    errno != EEXIST)
		return -1;
	if (!bind_udc)
		return 0;
	dir = opendir("/sys/class/udc");
	if (!dir)
		return -1;
	ret = -1;
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
#ifdef ANDROID_USB_CONSOLE
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
			bound = setup_gadget(1) == 0;
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
#else
static int open_usb_sink(void)
{
	/* UDC is reserved for adbd; see the setup_gadget() comment. */
	return -1;
}
#endif /* ANDROID_USB_CONSOLE */

static int open_sink(int console, int *usb)
{
	int out, n;

	for (n = 0; n < POLL_TRIES; n++) {
		out = open_logdump();
		if (out >= 0)
			return out;
		usleep(500000);
	}
#ifdef ANDROID_USB_CONSOLE
	say(console, "DIAG: no logdump after 60s; trying USB\n");
#else
	say(console, "DIAG: no logdump after 60s; USB console off,"
			" UDC left for adbd\n");
#endif
	out = open_usb_sink();
	if (out >= 0)
		*usb = 1;
	return out;
}

/*
 * CLOCK_BOOTTIME, not CLOCK_MONOTONIC: the capture deadlines are wall-clock
 * budgets, and CLOCK_MONOTONIC stops while the phone is suspended, so a
 * 15-minute capture outlived 20 suspends and kept fsync()ing kmsg into
 * logdump 22 minutes after boot.
 */
static time_t monotonic_seconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_BOOTTIME, &now);
	return now.tv_sec;
}

#ifdef ANDROID_CAPTURE
struct ramoops_sampler {
	int input;
	int output[2];
	int log;
	unsigned int slot;
	unsigned long long sample;
	size_t bytes;
	time_t next;
};

static void ramoops_sampler_close(struct ramoops_sampler *s)
{
	unsigned int i;

	if (s->input >= 0)
		close(s->input);
	s->input = -1;
	for (i = 0; i < 2; i++) {
		if (s->output[i] >= 0)
			close(s->output[i]);
		s->output[i] = -1;
	}
	/* log is borrowed from the durable recorder. */
}

static int ramoops_snapshot(struct ramoops_sampler *s)
{
	char buf[8192];
	int out = s->output[s->slot], ret;

	s->bytes = 0;
	if (ftruncate(out, 0) || lseek(out, 0, SEEK_SET) < 0)
		return -1;
	while (s->bytes < 1048576) {
		size_t chunk = 1048576 - s->bytes;
		ssize_t got;

		if (chunk > sizeof buf)
			chunk = sizeof buf;
		got = pread(s->input, buf, chunk, (off_t)s->bytes);

		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			if (!got)
				errno = EIO;
			return -1;
		}
		if (write_all(out, buf, (size_t)got))
			return -1;
		s->bytes += (size_t)got;
	}
	do {
		ret = fsync(out);
	} while (ret < 0 && errno == EINTR);
	if (ret)
		return -1;
	s->slot ^= 1;
	s->sample++;
	return 0;
}

static void ramoops_sampler_tick(struct ramoops_sampler *s)
{
	char message[192];
	char slot;

	if (!s || s->input < 0 || monotonic_seconds() < s->next)
		return;
	slot = (char)('a' + s->slot);
	if (!ramoops_snapshot(s)) {
		snprintf(message, sizeof message,
			 "DIAG: ramoops sample=%llu slot=%c bytes=1048576 t=%ld complete\n",
			 s->sample, slot, (long)monotonic_seconds());
		say(s->log, message);
	} else {
		int error = errno;

		snprintf(message, sizeof message,
			 "DIAG: ramoops slot=%c completed_bytes=%zu errno=%d disabled; primary capture continues\n",
			 slot, s->bytes, error);
		say(s->log, message);
		ramoops_sampler_close(s);
		errno = error;
	}
	/* Skip missed ticks rather than replaying a backlog. */
	s->next = monotonic_seconds() + 2;
}

static void ramoops_sampler_open(struct ramoops_sampler *s)
{
	struct stat st;
	int directory = -1, error, ret;
	char message[128];

	s->input = open("/sys/kernel/ramoops_raw", O_RDONLY | O_CLOEXEC);
	if (s->input < 0 || fstat(s->input, &st))
		goto failed;
	if (st.st_size != 1048576 || (st.st_mode & 07777) != 0400) {
		errno = EINVAL;
		goto failed;
	}
	s->output[0] = open("/sdlog/ramoops-a.bin",
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (s->output[0] < 0)
		goto failed;
	s->output[1] = open("/sdlog/ramoops-b.bin",
			    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (s->output[1] < 0)
		goto failed;
	directory = open("/sdlog", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0)
		goto failed;
	do {
		ret = fsync(directory);
	} while (ret < 0 && errno == EINTR);
	if (ret)
		goto failed;
	close(directory);
	say(s->log, "DIAG: ramoops sampler ready size=1048576\n");
	return;
failed:
	error = errno;
	if (directory >= 0)
		close(directory);
	ramoops_sampler_close(s);
	snprintf(message, sizeof message,
		 "DIAG: ramoops sampler unavailable errno=%d; primary capture continues\n",
		 error);
	say(s->log, message);
	errno = error;
}
#endif

/*
 * Sinks are independent and reported separately: bit 0 set means the primary
 * took the record, bit 1 the mirror. A USB gadget that unbinds ACM
 * mid-boot clears bit 0 and costs the USB capture only, not the whole run.
 * fd < 0 means "sink retired".
 */
static int write_capture(int out, int *mirror, const char *buf, size_t len,
			 int durable, size_t *remaining)
{
	int taken = 0;

	if (out >= 0) {
		if (!write_all_limit(out, buf, len, remaining) &&
		    !(durable && fsync(out)))
			taken |= 1;
		else if (mirror && *mirror >= 0)
			say(*mirror,
			    "\nDIAG: primary sink unavailable or full; SD capture continues\n");
	}
	if (mirror && *mirror >= 0) {
		if (!write_all(*mirror, buf, len) && !fsync(*mirror)) {
			taken |= 2;
		} else {
			close(*mirror);
			*mirror = -1;
			if (out >= 0) {
				static const char message[] =
					"\nDIAG: SD mirror failed; primary capture continues\n";

				write_all_limit(out, message, sizeof(message) - 1,
						remaining);
			}
		}
	}
	return taken;
}

/* Every sink gone: nothing left to record, stop burning the deadline. */
static int sinks_dead(int out, int *mirror)
{
	return out < 0 && (!mirror || *mirror < 0);
}

static int dump_log(int in, int out, unsigned seconds, int durable, int *mirror,
		    struct ramoops_sampler *sampler, size_t *remaining)
{
	time_t deadline = monotonic_seconds() + seconds;
	char buf[8192];

#ifndef ANDROID_CAPTURE
	(void)sampler;
#endif

	do {
		ssize_t got;
#ifdef ANDROID_CAPTURE
		/* ponytail: SD read/write/fsync makes 2s best-effort; use a
		 * separate collector only if measured latency warrants it. */
		ramoops_sampler_tick(sampler);
#endif
		got = read(in, buf, sizeof buf);
		if (got > 0) {
			int taken = write_capture(out, mirror, buf, (size_t)got,
						  durable, remaining);
			if (!(taken & 1))
				out = -1;
		} else if (got < 0 && errno == EPIPE) {
			static const char lost[] =
				"\nDIAG: kmsg overrun; some records lost\n";
			int taken = write_capture(out, mirror, lost,
						  sizeof(lost) - 1, durable, remaining);
			if (!(taken & 1))
				out = -1;
		} else if (got < 0 && (errno == EACCES || errno == EPERM)) {
			/*
			 * Enforcing SELinux takes the kmsg fd away from this
			 * pre-policy child; every retry is another avc line and
			 * no further record can ever arrive. Stop reading but
			 * keep the sinks open to the deadline so the marker is
			 * still written. This is what ended every capture at
			 * ~35 s: "DIAG: Android capture failed" in run25-sd.log.
			 */
			write_capture(out, mirror,
				"\nDIAG: kmsg read denied; capture idle to deadline\n",
				sizeof("\nDIAG: kmsg read denied; capture idle to deadline\n") - 1,
				durable, remaining);
			while (monotonic_seconds() <= deadline) {
				usleep(500000);
#ifdef ANDROID_CAPTURE
				ramoops_sampler_tick(sampler);
#endif
				if (sinks_dead(out, mirror))
					return -1;
			}
			break;
		} else if (got < 0 && errno != EAGAIN && errno != EINTR) {
			return -1;
		} else {
			usleep(200000);
		}
		if (sinks_dead(out, mirror))
			return -1;
	} while (monotonic_seconds() <= deadline);
	return 0;
}

#ifdef ANDROID_CAPTURE
/* mmcblk numbering can swap between stock and ACK: never guess eMMC vs SD. */
static int find_sd_partition(char *path, size_t size)
{
	DIR *dir = opendir("/sys/class/block");
	struct dirent *entry;
	int found = 0;

	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		char type_path[512], type[16];
		FILE *file;
		size_t digits;

		if (strncmp(entry->d_name, "mmcblk", 6))
			continue;
		digits = strspn(entry->d_name + 6, "0123456789");
		if (!digits || entry->d_name[6 + digits])
			continue;
		snprintf(type_path, sizeof type_path,
			 "/sys/class/block/%s/device/type", entry->d_name);
		file = fopen(type_path, "r");
		if (!file)
			continue;
		found = fgets(type, sizeof type, file) && !strcmp(type, "SD\n");
		fclose(file);
		if (found) {
			snprintf(path, size, "/dev/%sp1", entry->d_name);
			break;
		}
	}
	closedir(dir);
	return found;
}

static int open_sdlog(void)
{
	struct stat st;
	char path[512];
	int n;

	for (n = 0; n < POLL_SD_TRIES; n++) {
		if (find_sd_partition(path, sizeof path) &&
		    !stat(path, &st) && S_ISBLK(st.st_mode))
			break;
		usleep(500000);
	}
	if (n == POLL_SD_TRIES || make_dir("/sdlog") ||
	    mount(path, "/sdlog", "vfat", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))
		return -1;
	return open("/sdlog/ack-kmsg.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
}

#ifdef ANDROID_RAMOOPS_TEST
/* Catch slot advancement on partial input and a sampler failure aborting kmsg. */
static int ramoops_selftest(void)
{
	char directory[] = "/tmp/ack-ramoops-check-XXXXXX";
	char pattern[8192], actual[8192], captured[256], whole[1024];
	static const char records[] =
		"6,1,1000,-;selfcheck primary record one\n"
		"6,2,2000,-;selfcheck primary record two\n";
	struct ramoops_sampler s = {
		.input = -1, .output = { -1, -1 }, .log = -1,
	};
	struct stat st;
	size_t i, offset;
	int dir, source, completed_b, kmsg, primary, log;
	ssize_t got;

	assert(mkdtemp(directory));
	dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0);
	source = openat(dir, "source", O_RDWR | O_CREAT | O_EXCL, 0600);
	assert(source >= 0);
	for (i = 0; i < sizeof pattern; i++)
		pattern[i] = (char)((i * 37 + i / 251) & 255);
	for (offset = 0; offset < 1048576; offset += sizeof pattern)
		assert(!write_all(source, pattern, sizeof pattern));
	s.input = openat(dir, "source", O_RDONLY | O_CLOEXEC);
	s.output[0] = openat(dir, "a", O_WRONLY | O_CREAT | O_EXCL, 0600);
	s.output[1] = openat(dir, "b", O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(s.input >= 0 && s.output[0] >= 0 && s.output[1] >= 0);
	for (i = 0; i < 2; i++) {
		int check;
		assert(!ramoops_snapshot(&s));
		assert(s.slot == (i == 0 ? 1U : 0U) && s.sample == i + 1);
		assert(s.bytes == 1048576);
		check = openat(dir, i == 0 ? "a" : "b", O_RDONLY);
		assert(check >= 0 && !fstat(check, &st) && st.st_size == 1048576);
		for (offset = 0; offset < 1048576; offset += sizeof actual) {
			assert(pread(check, actual, sizeof actual, (off_t)offset) ==
			       (ssize_t)sizeof actual);
			assert(!memcmp(actual, pattern, sizeof actual));
		}
		close(check);
	}
	assert(!ftruncate(source, 12345));
	assert(ramoops_snapshot(&s) == -1 && errno == EIO);
	assert(s.slot == 0 && s.sample == 2 && s.bytes == 12345);
	completed_b = openat(dir, "b", O_RDONLY);
	assert(completed_b >= 0 && !fstat(completed_b, &st) &&
	       st.st_size == 1048576);
	kmsg = openat(dir, "kmsg", O_RDWR | O_CREAT | O_EXCL, 0600);
	primary = openat(dir, "primary", O_RDWR | O_CREAT | O_EXCL, 0600);
	log = openat(dir, "log", O_RDWR | O_CREAT | O_EXCL, 0600);
	assert(kmsg >= 0 && primary >= 0 && log >= 0);
	assert(!write_all(kmsg, records, sizeof records - 1));
	assert(lseek(kmsg, 0, SEEK_SET) == 0);
	s.log = log;
	s.next = 0;
	assert(!dump_log(kmsg, primary, 0, 1, NULL, &s, NULL));
	assert(s.input == -1 && s.output[0] == -1 && s.output[1] == -1);
	assert(s.slot == 0 && s.sample == 2);
	assert(fcntl(log, F_GETFD) >= 0);
	/* The daemon probe ticks inside dump_log, so probe lines interleave with
	 * the kmsg records. Their position is timing-dependent; the records must
	 * still arrive byte-for-byte and the probe must have run. */
	assert(!fstat(primary, &st));
	assert(st.st_size > (off_t)0 && st.st_size < (off_t)sizeof whole);
	got = pread(primary, whole, (size_t)st.st_size, 0);
	assert(got == st.st_size);
	whole[got] = '\0';
	assert(strstr(whole, records));
	assert(strstr(whole, "DIAG: procdir path="));
	assert(strstr(whole, "DIAG: daemon name=keystore2"));
	got = pread(log, captured, sizeof captured - 1, 0);
	assert(got > 0);
	captured[got] = '\0';
	assert(strstr(captured, "completed_bytes=12345 errno=") &&
	       strstr(captured, "disabled; primary capture continues"));
	for (offset = 0; offset < 1048576; offset += sizeof actual) {
		assert(pread(completed_b, actual, sizeof actual, (off_t)offset) ==
		       (ssize_t)sizeof actual);
		assert(!memcmp(actual, pattern, sizeof actual));
	}
	ramoops_sampler_close(&s);
	close(completed_b);
	close(source);
	close(kmsg);
	close(primary);
	close(log);
	assert(!unlinkat(dir, "source", 0));
	assert(!unlinkat(dir, "a", 0));
	assert(!unlinkat(dir, "b", 0));
	assert(!unlinkat(dir, "kmsg", 0));
	assert(!unlinkat(dir, "primary", 0));
	assert(!unlinkat(dir, "log", 0));
	close(dir);
	assert(!rmdir(directory));
	puts("Ramoops alternating snapshot and capture continuation checks PASS");
	return 0;
}

/* A dead USB sink must not end the capture while the SD mirror is open. */
static int sink_retirement_selftest(void)
{
	char directory[] = "/tmp/ack-sink-check-XXXXXX";
	static const char records[] = "6,1,1000,-;sink one\n6,2,2000,-;sink two\n";
	static const char more[] = "6,3,3000,-;sink three\n";
	struct stat st;
	int dir, kmsg, primary, mirror;
	char whole[512];
	off_t before;
	ssize_t got;

	assert(mkdtemp(directory));
	dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0);
	kmsg = openat(dir, "kmsg", O_RDWR | O_CREAT | O_EXCL, 0600);
	primary = openat(dir, "primary", O_RDWR | O_CREAT | O_EXCL, 0600);
	mirror = openat(dir, "mirror", O_RDWR | O_CREAT | O_EXCL, 0600);
	assert(kmsg >= 0 && primary >= 0 && mirror >= 0);
	/* Write a record the primary will refuse, then retire its fd. */
	assert(!write_all(kmsg, records, sizeof records - 1));
	assert(lseek(kmsg, 0, SEEK_SET) == 0);
	close(primary);
	primary = openat(dir, "read-only", O_RDONLY | O_CREAT | O_EXCL, 0600);
	assert(primary >= 0);
	assert(!dump_log(kmsg, primary, 0, 1, &mirror, NULL, NULL));
	assert(!fstat(mirror, &st) && st.st_size > (off_t)0);
	got = pread(mirror, whole, sizeof whole - 1, 0);
	assert(got > 0);
	whole[got] = '\0';
	assert(strstr(whole, records));
	/* With the primary retired, later records must still reach the mirror. */
	assert(!fstat(mirror, &st));
	before = st.st_size;
	assert(!write_all(kmsg, more, sizeof more - 1));
	assert(lseek(kmsg, 0, SEEK_SET) == 0);
	assert(!dump_log(kmsg, -1, 0, 1, &mirror, NULL, NULL));
	assert(!fstat(mirror, &st) && st.st_size > before);
	got = pread(mirror, whole, sizeof whole - 1, 0);
	assert(got > 0);
	whole[got] = '\0';
	assert(strstr(whole, more));
	/* Both sinks gone: capture must stop rather than spin to deadline. */
	close(mirror);
	mirror = -1;
	assert(dump_log(kmsg, -1, 0, 1, &mirror, NULL, NULL) == -1);
	close(primary);
	close(kmsg);
	close(mirror);
	assert(!unlinkat(dir, "kmsg", 0));
	assert(!unlinkat(dir, "primary", 0));
	assert(!unlinkat(dir, "read-only", 0));
	assert(!unlinkat(dir, "mirror", 0));
	close(dir);
	assert(!rmdir(directory));
	puts("Sink retirement and mirror continuation checks PASS");
	return 0;
}

/* The raw text prefix must not consume the reserved diagnostic tail. */
static int prefix_budget_selftest(void)
{
	static const char first[] = "6,1,1000,-;first record\n";
	static const char second[] = "6,2,2000,-;second record\n";
	static const char sentinel[] = "KEEP";
	static const char partial[] = "partial write";
	struct rlimit saved, limited;
	struct stat st;
	void (*handler)(int signal);
	char actual[sizeof(second)];
	char path[64];
	size_t cap = sizeof(first) - 1 + 3;
	size_t remaining = cap;
	size_t failed_budget = 3;
	size_t partial_budget = sizeof(partial) - 1;
	int primary, mirror, readonly, short_fd;

	primary = memfd_create("ack-prefix-primary", MFD_CLOEXEC);
	mirror = memfd_create("ack-prefix-mirror", MFD_CLOEXEC);
	short_fd = memfd_create("ack-prefix-short", MFD_CLOEXEC);
	assert(primary >= 0 && mirror >= 0 && short_fd >= 0);
	assert(pwrite(primary, sentinel, sizeof(sentinel) - 1, cap) ==
	       (ssize_t)sizeof(sentinel) - 1);
	assert(write_capture(primary, &mirror, first, sizeof(first) - 1,
			     0, &remaining) == 3);
	assert(remaining == 3);
	assert(write_capture(primary, &mirror, second, sizeof(second) - 1,
			     0, &remaining) == 2);
	assert(remaining == 0);
	assert(pread(primary, actual, sizeof(first) - 1, 0) ==
	       (ssize_t)sizeof(first) - 1);
	assert(!memcmp(actual, first, sizeof(first) - 1));
	assert(pread(primary, actual, 3, sizeof(first) - 1) == 3);
	assert(!memcmp(actual, second, 3));
	assert(pread(primary, actual, sizeof(sentinel) - 1, cap) ==
	       (ssize_t)sizeof(sentinel) - 1);
	assert(!memcmp(actual, sentinel, sizeof(sentinel) - 1));
	assert(!fstat(mirror, &st));
	assert(pread(mirror, actual, sizeof(second) - 1,
		     st.st_size - (sizeof(second) - 1)) ==
	       (ssize_t)sizeof(second) - 1);
	assert(!memcmp(actual, second, sizeof(second) - 1));

	snprintf(path, sizeof(path), "/proc/self/fd/%d", primary);
	readonly = open(path, O_RDONLY);
	assert(readonly >= 0);
	assert(write_all_limit(readonly, first, 3, &failed_budget) == -1);
	assert(errno == EBADF && failed_budget == 3);

	/* A short successful write followed by failure consumes only its bytes. */
	assert(!getrlimit(RLIMIT_FSIZE, &saved));
	limited = saved;
	limited.rlim_cur = 3;
	handler = signal(SIGXFSZ, SIG_IGN);
	assert(handler != SIG_ERR);
	assert(!setrlimit(RLIMIT_FSIZE, &limited));
	assert(write_all_limit(short_fd, partial, sizeof(partial) - 1,
			       &partial_budget) == -1);
	assert(errno == EFBIG && partial_budget == sizeof(partial) - 1 - 3);
	assert(!setrlimit(RLIMIT_FSIZE, &saved));
	assert(signal(SIGXFSZ, handler) != SIG_ERR);
	assert(pread(short_fd, actual, 3, 0) == 3);
	assert(!memcmp(actual, partial, 3));

	close(readonly);
	close(short_fd);
	close(mirror);
	close(primary);
	puts("Logdump prefix budget, mirror continuation and partial writes PASS");
	return 0;
}
#endif

#define GUARDED_FSTAB "/dev/block/ack-protected-fstab.qcom"
#define GUARDED_CONTEXTS "/dev/block/ack-vendor-file-contexts"
#define BOOT_CONTROLLER "/dev/block/platform/soc@0/7824900.sdhci"

static int guard_pre_policy(int proc)
{
	char context[64];
	ssize_t n;
	int fd = openat(proc, "self/attr/current", O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	do {
		n = read(fd, context, sizeof context);
	} while (n < 0 && errno == EINTR);
	{
		int error = errno;
		close(fd);
		errno = error;
	}
	/* Before initialization SELinux returns the initial, NUL-terminated SID.
	 * A loaded policy's u:r:kernel:s0 is not permission to install overlays. */
	if (n == (ssize_t)sizeof("kernel") &&
	    !memcmp(context, "kernel", sizeof("kernel")))
		return 0;
	if (n >= 0)
		errno = EPERM;
	return -1;
}

static int validate_guard_fstab(int fd)
{
	char buf[8192], *line, *lines;
	struct stat st;
	ssize_t n;
	int userdata = 0;

	if (fstat(fd, &st))
		return -1;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size >= (off_t)sizeof buf) {
		errno = EINVAL;
		return -1;
	}
	do {
		n = pread(fd, buf, sizeof buf - 1, 0);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return -1;
	if (n != st.st_size || memchr(buf, '\0', (size_t)n)) {
		errno = EINVAL;
		return -1;
	}
	buf[n] = '\0';
	for (line = strtok_r(buf, "\n", &lines); line;
	     line = strtok_r(NULL, "\n", &lines)) {
		char *field[5], *word, *words, *comment = strchr(line, '#');
		int fields = 0;

		if (comment)
			*comment = '\0';
		for (word = strtok_r(line, " \t\r", &words); word;
		     word = strtok_r(NULL, " \t\r", &words)) {
			if (fields == 5)
				goto invalid;
			field[fields++] = word;
		}
		if (!fields)
			continue;
		if (fields != 5)
			goto invalid;
		for (word = strtok_r(field[4], ",", &words); word;
		     word = strtok_r(NULL, ",", &words)) {
			if (!strncmp(word, "formattable", 11) &&
			    (!word[11] || word[11] == '=')) {
				errno = EPERM;
				return -1;
			}
		}
		if (!strcmp(field[1], "/data")) {
			if (strcmp(field[0],
				   "/dev/block/bootdevice/by-name/userdata") ||
			    strcmp(field[2], "f2fs") || ++userdata != 1)
				goto invalid;
		}
	}
	if (userdata == 1)
		return 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int guard_bind_config(int proc, int in, const char *source,
			     const char *target)
{
	char buf[8192];
	struct stat input, staged, bound;
	struct statvfs fs;
	off_t offset = 0;
	int out, error;

	if (guard_pre_policy(proc) || fstat(in, &input))
		return -1;
	if (!S_ISREG(input.st_mode) || input.st_size <= 0 ||
	    input.st_size > 128 * 1024) {
		errno = EINVAL;
		return -1;
	}
	out = open(source, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
		   0444);
	if (out < 0)
		return -1;
	while (offset < input.st_size) {
		ssize_t n = pread(in, buf, sizeof buf, offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0 || write_all(out, buf, (size_t)n)) {
			if (!n)
				errno = EIO;
			goto failed;
		}
		offset += n;
	}
	if (fstat(out, &staged))
		goto failed;
	if (close(out)) {
		out = -1;
		goto failed;
	}
	out = -1;
	if (guard_pre_policy(proc) ||
	    mount(source, target, NULL, MS_BIND, NULL))
		goto failed;
	if (mount(NULL, target, NULL,
		  MS_BIND | MS_REMOUNT | MS_RDONLY | MS_NOSUID | MS_NODEV |
		  MS_NOEXEC, NULL)) {
		error = errno;
		umount(target);
		errno = error;
		goto failed;
	}
	if (stat(target, &bound) || statvfs(target, &fs))
		return -1;
	if (bound.st_dev != staged.st_dev || bound.st_ino != staged.st_ino ||
	    !(fs.f_flag & ST_RDONLY)) {
		errno = EIO;
		return -1;
	}
	return guard_pre_policy(proc);
failed:
	error = errno;
	if (out >= 0)
		close(out);
	unlink(source);
	errno = error;
	return -1;
}

/*
 * USB handoff to adb. The diag ACM function holds gadget g1 bound to the
 * only UDC, so Android's adb bind fails with EBUSY, and this process cannot
 * unbind it after policy load (kernel domain has no configfs access). Append
 * an action to the vendor USB rc so init itself releases the UDC when the
 * vendor stack switches to configfs ("on boot", before adbd starts). SD
 * and logdump capture are unaffected; the USB serial stream ends there.
 * This vendor stack sets sys.usb.configfs=2, never =1 (run124), so install
 * both triggers; the kmsg marker records which one fired in the run log.
 */
#define USB_RC "/vendor/etc/init/hw/init.qcom.usb.rc"

#ifdef ANDROID_USB_CONSOLE
static void guard_usb_handoff(int proc, int log)
{
	static const char action[] =
		"\n# ACK diag: hand the UDC from the diag ACM gadget to adb.\n"
		"on property:sys.usb.configfs=1\n"
		"    write /dev/kmsg \"ACK diag: USB handoff configfs=1\"\n"
		"    write /config/usb_gadget/g1/UDC \"none\"\n"
		"    rm /config/usb_gadget/g1/configs/c.2/f1\n"
		"    rmdir /config/usb_gadget/g1/configs/c.2\n"
		"on property:sys.usb.configfs=2\n"
		"    write /dev/kmsg \"ACK diag: USB handoff configfs=2\"\n"
		"    write /config/usb_gadget/g1/UDC \"none\"\n"
		"    rm /config/usb_gadget/g1/configs/c.2/f1\n"
		"    rmdir /config/usb_gadget/g1/configs/c.2\n";
	char buf[8192], message[160];
	int in = open(USB_RC, O_RDONLY | O_CLOEXEC);
	int fd = memfd_create("ack-usb-rc", MFD_CLOEXEC);
	int error = 0;
	ssize_t n;

	if (in < 0 || fd < 0)
		goto failed;
	for (;;) {
		n = read(in, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto failed;
		if (!n)
			break;
		if (write_all(fd, buf, (size_t)n))
			goto failed;
	}
	if (write_all(fd, action, sizeof(action) - 1) ||
	    guard_bind_config(proc, fd, "/dev/block/ack-usb-handoff.rc", USB_RC))
		goto failed;
	goto done;
failed:
	error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: USB handoff rc appended to %s errno=%d\n", USB_RC, error);
	say(log, message);
}
#endif /* ANDROID_USB_CONSOLE */

#ifdef ANDROID_EARLY_LOGCAT
/*
 * Android init owns the collector, so it sees Android's mounts and logd
 * sockets. The existing shell domain can execute logcat and write
 * shell_data_file; do not run logcat in the pre-policy kernel domain.
 * /data must be mounted first. Earlier init messages remain in kmsg.
 * Flush an initial logd backlog before streaming: an early reset can precede
 * the independent sync loop's next iteration. Streaming may repeat backlog
 * records; retaining them avoids losing messages during the initial flush.
 */
static void guard_early_logcat(int proc, int log)
{
	static const char target[] = "/system/etc/init/logd.rc";
	static const char action[] =
		"\n# ACK diagnostic: persistent Android logs before adb.\n"
		"service ack-early-logcat /system/bin/sh -c \"/system/bin/logcat -d -b all -v threadtime -f /data/local/tmp/ack-early-logcat.log -r 1024 -n 4 && sync && exec /system/bin/logcat -b all -v threadtime -f /data/local/tmp/ack-early-logcat.log -r 1024 -n 4\"\n"
		"    class core\n"
		"    disabled\n"
		"    user root\n"
		"    group log shell readproc\n"
		"    seclabel u:r:shell:s0\n"
		"    restart_period 5\n"
		"service ack-kmsg-dump /system/bin/sh -c \"exec /system/bin/cat /dev/kmsg >> /data/local/tmp/ack-kmsg.log\"\n"
		"    class core\n"
		"    disabled\n"
		"    user root\n"
		"    group shell readproc\n"
		"    restart_period 5\n"
		"service ack-sync-loop /system/bin/sh -c \"i=0; while true; do i=$$((i+1)); echo $$i $$(cat /proc/uptime) >> /data/local/tmp/ack-sync-probe.log; sync; sleep 5; done\"\n"
		"    class core\n"
		"    disabled\n"
		"    user root\n"
		"    group shell readproc\n"
		"    seclabel u:r:shell:s0\n"
		"    restart_period 5\n"
		"\non post-fs-data\n"
		"    start ack-early-logcat\n"
		"    start ack-sync-loop\n"
		"    start ack-kmsg-dump\n";
	char buf[8192], message[160];
	int in = open(target, O_RDONLY | O_CLOEXEC);
	int fd = memfd_create("ack-logcat-rc", MFD_CLOEXEC);
	int error = 0;
	ssize_t n;

	if (in < 0 || fd < 0)
		goto failed;
	for (;;) {
		n = read(in, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto failed;
		if (!n)
			break;
		if (write_all(fd, buf, (size_t)n))
			goto failed;
	}
	if (write_all(fd, action, sizeof(action) - 1) ||
	    guard_bind_config(proc, fd, "/dev/block/ack-early-logcat.rc", target))
		goto failed;
	goto done;
failed:
	error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: early logcat rc appended errno=%d\n", error);
	say(log, message);
}
#endif

/*
 * Remove the ROM's `on property:sys.init.updatable_crashing=1` action from an
 * rc buffer. Returns 0 and the filtered length, -ENODATA when the action is
 * absent, or -E2BIG when the output does not fit.
 */
static int filter_flags_health_check_rc(const char *text, size_t used,
					char *out, size_t out_size,
					size_t *out_len)
{
	static const char trigger[] =
		"on property:sys.init.updatable_crashing=1";
	size_t offset = 0, written = 0, tlen = sizeof(trigger) - 1;
	int skipping = 0, removed = 0;

	while (offset < used) {
		const char *line = text + offset;
		const char *end = memchr(line, '\n', used - offset);
		size_t len = end ? (size_t)(end - line) + 1 : used - offset;
		int top = 0;

		offset += len;
		if (len && line[0] != ' ' && line[0] != '\t') {
			top = (len >= 3 && !memcmp(line, "on ", 3)) ||
			      (len >= 8 && !memcmp(line, "service ", 8)) ||
			      (len >= 7 && !memcmp(line, "import ", 7));
		}
		if (!skipping && len >= tlen &&
		    !memcmp(line, trigger, tlen) &&
		    (len == tlen || line[tlen] == '\n' ||
		     line[tlen] == '\r' || line[tlen] == ' ' ||
		     line[tlen] == '\t')) {
			skipping = 1;
			removed++;
			continue;
		}
		if (skipping) {
			if (top)
				skipping = 0;
			else
				continue;
		}
		if (written + len > out_size)
			return -E2BIG;
		memcpy(out + written, line, len);
		written += len;
	}
	if (!removed)
		return -ENODATA;
	*out_len = written;
	return 0;
}

/*
 * Vendor services that crash-loop on this port.  dataqti waits for the
 * rmnet/MHI node that only exists once the modem data path is up, so
 * keep it stopped instead of retrying it every second.  The camera
 * services used to be stopped here as well, because the kernel had no
 * camera at all; the CAF camera stack is in the tree now, so they run.
 *
 * Service definitions for a vendor service cannot be overridden from
 * /system: init rejects an override that crosses the treble boundary
 * (service_parser.cpp compares the file's subcontext).  Only actions are
 * installed from this system-side file; the qrtr-ns service override is
 * appended to a vendor rc file instead (see guard_vendor_qrtr).
 */
static const char crashloop_rc[] =
	"\n"
	"# ack: data DPM waits on the missing rmnet/MHI node.\n"
	"on property:init.svc.vendor.dataqti=running\n"
	"    stop vendor.dataqti\n"
	"on property:init.svc.vendor.dataqti=restarting\n"
	"    stop vendor.dataqti\n"
	"\n"
	"# ack: the camera services used to crash-loop here because the\n"
	"# kernel had no camera at all; the CAF camera stack is in the tree\n"
	"# now, so they run.\n";

/*
 * The ROM's flags_health_check.rc runs flags_health_check
 * UPDATABLE_CRASHING every time sys.init.updatable_crashing flips to 1.
 * Several vendor services crash-loop on this port for bring-up reasons
 * unrelated to the boot flag, so that action becomes a continuous exec loop.
 * Keep the BOOT_FAILURE check and the boot-count reset; drop only the
 * crashing action, and append the vendor overrides above, in a read-only
 * bind overlay installed before policy load.
 */
static void guard_flags_health_check(int proc, int log)
{
	static const char target[] = "/system/etc/init/flags_health_check.rc";
	static char text[32 * 1024];
	static char filtered[32 * 1024];
	char message[160];
	size_t used = 0, filtered_len = 0;
	int in = -1, fd = -1, error = 0, ret;

	in = open(target, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		goto failed;
	for (;;) {
		ssize_t n;

		if (used == sizeof(text)) {
			error = E2BIG;
			goto done;
		}
		n = read(in, text + used, sizeof(text) - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto failed;
		if (!n)
			break;
		used += (size_t)n;
	}
	ret = filter_flags_health_check_rc(text, used, filtered,
					  sizeof(filtered), &filtered_len);
	if (ret) {
		error = -ret;
		goto done;
	}
	if (filtered_len + sizeof(crashloop_rc) - 1 > sizeof(filtered)) {
		error = E2BIG;
		goto done;
	}
	memcpy(filtered + filtered_len, crashloop_rc, sizeof(crashloop_rc) - 1);
	filtered_len += sizeof(crashloop_rc) - 1;
	fd = memfd_create("ack-flags-health-check-rc", MFD_CLOEXEC);
	if (fd < 0)
		goto failed;
	if (write_all(fd, filtered, filtered_len)) {
		error = errno;
		goto done;
	}
	if (guard_bind_config(proc, fd,
			      "/dev/block/ack-flags-health-check.rc", target)) {
		error = errno;
		goto done;
	}
	goto done;
failed:
	error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: flags_health_check rc override errno=%d\n", error);
	say(log, message);
}

/*
 * The qrtr-ns override must be parsed from a vendor file: init rejects an
 * override of a vendor service from /system as a treble-boundary crossing.
 * qcrild.rc is in /vendor/etc/init, so it is parsed after the hardware file
 * that defines the service and in the vendor subcontext.  Append the
 * override there without touching the rest of the file.
 */
static const char qrtr_vendor_rc[] =
	"\n"
	"# ack: 6.12 QRTR needs CAP_NET_ADMIN for the userspace name service.\n"
	"service vendor.qrtr-ns /vendor/bin/qrtr-ns -f\n"
	"    class core\n"
	"    user vendor_qrtr\n"
	"    group vendor_qrtr\n"
	"    capabilities NET_BIND_SERVICE NET_ADMIN\n"
	"    override\n";

static void guard_vendor_qrtr(int proc, int log)
{
	static const char target[] = "/vendor/etc/init/qcrild.rc";
	static char text[32 * 1024];
	char message[160];
	size_t used = 0;
	int in = -1, fd = -1, error = 0;

	in = open(target, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		goto failed;
	for (;;) {
		ssize_t n;

		if (used == sizeof(text)) {
			error = E2BIG;
			goto done;
		}
		n = read(in, text + used, sizeof(text) - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			goto failed;
		if (!n)
			break;
		used += (size_t)n;
	}
	if (used + sizeof(qrtr_vendor_rc) - 1 > sizeof(text)) {
		error = E2BIG;
		goto done;
	}
	memcpy(text + used, qrtr_vendor_rc, sizeof(qrtr_vendor_rc) - 1);
	used += sizeof(qrtr_vendor_rc) - 1;
	fd = memfd_create("ack-qrtr-vendor-rc", MFD_CLOEXEC);
	if (fd < 0)
		goto failed;
	if (write_all(fd, text, used)) {
		error = errno;
		goto done;
	}
	if (guard_bind_config(proc, fd, "/dev/block/ack-qrtr-vendor.rc", target))
		error = errno;
	goto done;
failed:
	error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: vendor qrtr override errno=%d\n", error);
	say(log, message);
}
#ifdef ANDROID_DISPLAY_GATE
/*
 * Display-gate probe (run110, reinstated for run125): stub the display stack
 * only. Run124 ran it and userspace daemons crashed in a SIGSEGV cascade
 * after the first GPU opens and MDSS SMMU faults; run123 gated it and had
 * none. hwcomposer and display-color-hal drive MDSS sysfs/framebuffer/PP
 * ioctls; surfaceflinger opens the GPU. Everything else is kept.
 */
static const char *const displaygate_rc[][2] = {
	{ "/dev/block/ack-displaygate-composer.rc",
	  "/vendor/etc/init/android.hardware.graphics.composer@2.1-service.rc" },
	{ "/dev/block/ack-displaygate-color.rc",
	  "/vendor/etc/init/vendor.display.color@1.0-service.rc" },
	{ "/dev/block/ack-displaygate-surfaceflinger.rc",
	  "/system/etc/init/surfaceflinger.rc" },
};

static void guard_displaygate(int proc, int log)
{
	static const char stub[] = "# disabled by ACK diag display gate probe\n";
	char message[256];
	size_t i;

	for (i = 0; i < sizeof(displaygate_rc) / sizeof(displaygate_rc[0]); i++) {
		int fd = memfd_create("ack-displaygate-rc", MFD_CLOEXEC);
		int error = 0;

		if (fd < 0 || write_all(fd, stub, sizeof(stub) - 1) ||
		    guard_bind_config(proc, fd, displaygate_rc[i][0],
				      displaygate_rc[i][1]))
			error = errno;
		if (fd >= 0)
			close(fd);
		snprintf(message, sizeof(message),
			 "DIAG: display gate rc override %s errno=%d\n",
			 displaygate_rc[i][1], error);
		say(log, message);
	}
}
#endif

/*
 * GPU firmware staging. Upstream adreno requests qcom/a530_pm4.fw,
 * filesystem firmware loader, which this early only searches the ramdisk
 * (/lib/firmware); the blobs live on /vendor/firmware, unreadable to
 * shell/adb. This process is root in PID 1's mount namespace after vendor
 * is up, so copy them into the ramdisk before the deferred GPU probe runs.
 * Missing files are logged, never fatal.
 */
static int stage_firmware_file(const char *src, const char *dest, long *bytes)
{
	char buf[8192];
	int in, out, error = 0;
	ssize_t n;

	*bytes = 0;
	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return errno;
	out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0444);
	if (out < 0) {
		error = errno;
		close(in);
		return error;
	}
	for (;;) {
		n = read(in, buf, sizeof(buf));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			error = errno;
			break;
		}
		if (!n)
			break;
		if (write_all(out, buf, (size_t)n)) {
			error = errno;
			break;
		}
		*bytes += n;
	}
	close(in);
	if (close(out) && !error)
		error = errno;
	return error;
}

static const char b64digits[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void exfil_blob(const char *src, const char *base, int log)
{
	static char data[128 * 1024];
	char message[256], chunk[200];
	struct stat st;
	int in = open(src, O_RDONLY | O_CLOEXEC);
	off_t offset = 0;
	ssize_t n;
	size_t total, seq = 0, at = 0;

	if (in < 0) {
		snprintf(message, sizeof(message), "DIAG: fwblob %s open errno=%d\n",
			 base, errno);
		say(log, message);
		return;
	}
	if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > (off_t)sizeof(data)) {
		snprintf(message, sizeof(message), "DIAG: fwblob %s stat errno=%d\n",
			 base, errno);
		say(log, message);
		close(in);
		return;
	}
	while (offset < st.st_size) {
		n = pread(in, data + offset,
			  (size_t)(st.st_size - offset), offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		offset += n;
	}
	close(in);
	total = (size_t)offset;
	while (at < total) {
		size_t step = total - at > 144 ? 144 : total - at;
		size_t groups = step / 3, rem = step % 3, i, o = 0;

		for (i = 0; i < groups * 3; i += 3) {
			unsigned triple = ((unsigned char)data[at + i] << 16) |
				((unsigned char)data[at + i + 1] << 8) |
				(unsigned char)data[at + i + 2];

			chunk[o++] = b64digits[(triple >> 18) & 63];
			chunk[o++] = b64digits[(triple >> 12) & 63];
			chunk[o++] = b64digits[(triple >> 6) & 63];
			chunk[o++] = b64digits[triple & 63];
		}
		if (rem) {
			unsigned triple = (unsigned char)data[at + groups * 3] << 16;

			if (rem == 2)
				triple |= (unsigned char)data[at + groups * 3 + 1] << 8;
			chunk[o++] = b64digits[(triple >> 18) & 63];
			chunk[o++] = b64digits[(triple >> 12) & 63];
			chunk[o++] = rem == 2 ? b64digits[(triple >> 6) & 63] : '=';
			chunk[o++] = '=';
		}
		chunk[o] = '\0';
		snprintf(message, sizeof(message), "DIAG: fwblob %s %zu %s\n",
			 base, seq++, chunk);
		say(log, message);
		at += step;
	}
}

static void guard_firmware(int log)
{
	static const char *const dests[] = {
		"/lib/firmware/qcom/a530_pm4.fw",
		"/lib/firmware/qcom/a530_pfp.fw",
		"/lib/firmware/a506_zap.mdt",
		"/lib/firmware/a506_zap.b00",
		"/lib/firmware/a506_zap.b01",
		"/lib/firmware/a506_zap.b02",
	};
	static const char *const bases[] = {
		"a530_pm4.fw",
		"a530_pfp.fw",
		"a506_zap.mdt",
		"a506_zap.b00",
		"a506_zap.b01",
		"a506_zap.b02",
	};
	char message[256], src[128], exfil[128];
	size_t i;

	make_dir("/lib");
	make_dir("/lib/firmware");
	make_dir("/lib/firmware/qcom");
	make_dir("/dev/fw-exfil");
	for (i = 0; i < sizeof(dests) / sizeof(dests[0]); i++) {
		int error, xerror;
		long bytes = 0, xbytes = 0;

		snprintf(src, sizeof(src), "/vendor/firmware/%s", bases[i]);
		if (access(src, R_OK) &&
		    snprintf(src, sizeof(src), "/vendor/firmware/qcom/%s",
			     bases[i]) < 0)
			src[0] = '\0';
		error = *src ? stage_firmware_file(src, dests[i], &bytes) :
			ENOENT;
		snprintf(exfil, sizeof(exfil), "/dev/fw-exfil/%s", bases[i]);
		xerror = *src ? stage_firmware_file(src, exfil, &xbytes) :
			ENOENT;
		snprintf(message, sizeof(message),
			 "DIAG: firmware %s bytes=%ld errno=%d\n",
			 dests[i], bytes, error);
		say(log, message);
		snprintf(message, sizeof(message), "DIAG: exfil %s bytes=%ld errno=%d\n",
			 exfil, xbytes, xerror);
		say(log, message);
		if (*src)
			exfil_blob(src, bases[i], log);
	}
}
/*
 * Wait for the modem partition, retain the working GPU firmware search
 * directories, and start the autoboot-disabled MSS. Leave the already
 * running ADSP and WCNSS alone. Detached fork, never blocks the guard or boot.
 */
static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static int mounts_contain(const char *word)
{
	static char buf[8192];
	int fd = open("/proc/mounts", O_RDONLY | O_CLOEXEC);
	ssize_t n, total = 0;

	if (fd < 0)
		return 0;
	for (;;) {
		n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		total += n;
		if (total >= (ssize_t)sizeof(buf) - 1)
			break;
	}
	close(fd);
	buf[total] = '\0';
	return strstr(buf, word) != NULL;
}

/*
 * The MSS (qcom_q6v5_mss) sets rproc->auto_boot = false in this port, so
 * nothing starts it.  With the firmware search path already pointed at the
 * modem image, writing "start" to the matching remoteproc's state is the
 * documented interface (Documentation/ABI/testing/sysfs-class-remoteproc).
 * Match on "mba", the MBA image name the MSS driver is registered with;
 * adsp.mdt and wcnss.mdt do not contain it.
 */
static void start_remoteproc(const char *match, int log)
{
	char message[256], dirpath[128], name[64], state[32];
	DIR *rp;
	struct dirent *entry;
	int ret;

	rp = opendir("/sys/class/remoteproc");
	if (!rp) {
		snprintf(message, sizeof(message),
			 "DIAG: fwlate %s: remoteproc class errno=%d\n",
			 match, errno);
		say(log, message);
		return;
	}
	while ((entry = readdir(rp))) {
		if (strncmp(entry->d_name, "remoteproc", 10))
			continue;
		snprintf(dirpath, sizeof(dirpath),
			 "/sys/class/remoteproc/%s/firmware", entry->d_name);
		if (read_file(dirpath, name, sizeof(name)))
			continue;
		if (!strstr(name, match))
			continue;
		snprintf(dirpath, sizeof(dirpath),
			 "/sys/class/remoteproc/%s/state", entry->d_name);
		if (read_file(dirpath, state, sizeof(state)))
			state[0] = '\0';
		snprintf(message, sizeof(message),
			 "DIAG: fwlate %s %s before state=%s\n",
			 match, entry->d_name, state[0] ? state : "?");
		say(log, message);
		if (strstr(state, "running"))
			continue;
		ret = attribute(dirpath, "start");
		snprintf(message, sizeof(message),
			 "DIAG: fwlate %s %s start ret=%d errno=%d\n",
			 match, entry->d_name, ret, ret ? errno : 0);
		say(log, message);
		if (read_file(dirpath, state, sizeof(state)))
			state[0] = '\0';
		snprintf(message, sizeof(message),
			 "DIAG: fwlate %s %s after state=%s\n",
			 match, entry->d_name, state[0] ? state : "?");
		say(log, message);
	}
	closedir(rp);
}

static void harvest_modem_child(int log)
{
	time_t deadline = monotonic_seconds() + 240;
	char message[256];
	int pathset = 0;
	int patherr = 0;
	/*
	 * Whole-partition harvest first: needs no mounts (devtmpfs block
	 * nodes exist from boot) and sidesteps all file-level DAC issues.
	 * Shell cannot read mmcblk0p1 (brw------- root); this root child
	 * can. /sdlog is vfat and host-readable; adb pull is a fallback.
	 */
	{
		long bytes = 0;
		int error = 0;

		/* /sdlog must be the card, not ramdisk: refuse unless the
		 * mirror log the SD setup creates is already there.
		 */
		if (!mounts_contain("/sdlog")) {
			snprintf(message, sizeof(message),
				 "DIAG: fwlate partition skipped: no sdlog mount\n");
			say(log, message);
		} else {
			make_dir("/sdlog/fwpart");
			error = stage_firmware_file("/dev/block/mmcblk0p1",
						    "/sdlog/fwpart/modem_a.img",
						    &bytes);
		}
		snprintf(message, sizeof(message),
			 "DIAG: fwlate partition mmcblk0p1 bytes=%ld errno=%d\n",
			 bytes, error);
		say(log, message);
	}

	say(log, "DIAG: fwlate: waiting for modem mount\n");
	for (;;) {
		if (mounts_contain("firmware_mnt"))
			break;
		if (monotonic_seconds() >= deadline) {
			say(log, "DIAG: fwlate: modem never mounted\n");
			_exit(0);
		}
		sleep(2);
	}
	/*
	 * The diag init still runs in the kernel domain here: the Android
	 * init exec (and the transition to init) happens after this child is
	 * forked.  The kernel domain may be denied the generic sysfs write,
	 * and the boot command line already carries the image path, so treat
	 * an already-correct path as success.  Log uid/euid/context and the
	 * exact errno: run336 showed EACCES even though a root su shell can
	 * rewrite the same file.
	 */
	{
		char current[256] = "";
		char context[64] = "";

		if (read_file("/proc/self/attr/current", context, sizeof(context)))
			context[0] = '\0';
		context[strcspn(context, "\r\n")] = '\0';
		if (attribute("/sys/module/firmware_class/parameters/path",
			       "/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image"))
			patherr = errno;
		if (read_file("/sys/module/firmware_class/parameters/path",
			      current, sizeof(current)))
			current[0] = '\0';
		current[strcspn(current, "\r\n")] = '\0';
		pathset = !patherr;
		if (!pathset && !strcmp(current,
			"/lib/firmware,/vendor/firmware,/vendor/firmware_mnt/image"))
			pathset = 2;
		snprintf(message, sizeof(message),
			 "DIAG: fwlate: custom path set=%d errno=%d uid=%d euid=%d ctx=%s path=%s\n",
			 pathset, patherr, (int)getuid(), (int)geteuid(),
			 context[0] ? context : "?", current[0] ? current : "?");
		say(log, message);
	}
	if (!pathset)
		_exit(0);
	sleep(2);
	/*
	 * adsp and wcnss are already up: the boot loaded adsp.mdt and
	 * wcnss.mdt.  Rebinding them here unbound them and then failed to
	 * bind again because the device/driver symlink disappears with the
	 * unbind.  Only the
	 * MSS is autoboot-disabled in this port.
	 */
	start_remoteproc("mba", log);
	say(log, "DIAG: fwlate done\n");
	_exit(0);
}

static void harvest_modem_firmware(int log)
{
	pid_t pid = fork();

	if (pid < 0) {
		say(log, "DIAG: modem harvest: fork failed\n");
		return;
	}
	if (!pid) {
		setsid();
		harvest_modem_child(log);
	}
}

/*
 * run281: relabel the sysfs node families that the 6.12 kernel adds where
 * the ROM's policy expects the 4.19 layout.
 *
 * The ROM is Android 17 EvoX and its policy was written for the 4.19 CAF
 * kernel. Two families do not line up:
 *
 *  - The per-device wakeup statistics,
 *    /sys/devices/platform/soc@0/<device>/wakeup/wakeupN/, are new in
 *    6.12. The policy has a type for them - sysfs_wakeup - and it lets
 *    system_suspend read that type, but the rule was written for the 4.19
 *    path /sys/devices/virtual/wakeup, which this kernel does not have.
 *    The new nodes therefore land on the generic sysfs type, the suspend
 *    service is denied reading them, its checkpoint commit fails,
 *    system_server asks for a reboot with the reason "Checkpoint commit
 *    failed", and the reboot does not finish: the phone is left with a lit
 *    screen, no adb and no way back. run278's logdump ends with the
 *    kernel's "sysrq: Terminate All Tasks" and nothing after it.
 *    These stay a runtime walk: the devices sit at different depths (the
 *    PMIC's at seven levels), there is no single path to name, and the
 *    policy's genfscon wildcard capability is off.
 *
 *  - The kgsl's power attributes (max_pwrlevel, min_pwrlevel, idle_timer,
 *    force_clk_on, force_rail_on) are on the generic sysfs type too. The
 *    vendor policy grants hal_power_default access to sysfs_kgsl, but its
 *    file-context rule matches the old soc/<addr>.qcom,kgsl-3d0 path, not this
 *    kernel's soc@0/1c00000.gpu path. sysfs_gpu is a different type and
 *    does not grant the power HAL the writes it needs. The service runs
 *    as root, so the existing root-owned 0644 attributes need no chmod.
 *
 *    This one is a fixed path, so it is labelled by the genfscon in
 *    porting/policy/port-genfs-kgsl.cil, which build-kernel.sh appends to
 *    the ramdisk CIL and the ROM's policy compiler applies at policy load.
 *    The worker below still checks the directory's label - it never
 *    changes a directory, so that label tells which mechanism ran - and
 *    walks the files only as a fallback for an image packaged without the
 *    fragment.
 *
 * Relabelling is a file attribute, not a policy change: the ROM's policy
 * is untouched and its existing rules for sysfs_wakeup and sysfs_kgsl
 * apply from then on. That matters, because the alternative - patching
 * the policy at runtime - deadlocks this kernel: the kernel-side live
 * policy rewrite that KernelSU-Next uses for it (the SET_SEPOLICY
 * supercall) froze the phone on the first attempt.
 */
#define DIAG_WAKEUP_LABEL	"u:object_r:sysfs_wakeup:s0"
#define DIAG_GPU_LABEL		"u:object_r:sysfs_kgsl:s0"
#define DIAG_KGSL_SYSFS \
	"/sys/devices/platform/soc@0/1c00000.gpu/kgsl/kgsl-3d0"

/*
 * Relabel every regular file below one directory.
 *
 * The number of labels set is returned. A directory that cannot be opened
 * is normal in a live sysfs - devices come and go - and is counted as
 * skipped; a file whose label could not be set is a failure, and the
 * first errno is kept.
 */
static int relabel_failures;
static int relabel_skipped;
static int relabel_first_error;

static int relabel_dir(const char *path, const char *label, unsigned int depth)
{
	DIR *dir;
	struct dirent *entry;
	int set = 0;

	if (depth > 4)
		return 0;
	dir = opendir(path);
	if (!dir) {
		relabel_skipped++;
		return 0;
	}
	while ((entry = readdir(dir))) {
		char child[1024];
		struct stat st;

		if (!strcmp(entry->d_name, ".") ||
		    !strcmp(entry->d_name, ".."))
			continue;
		if (snprintf(child, sizeof child, "%s/%s", path,
			     entry->d_name) >= (int)sizeof child)
			continue;
		if (lstat(child, &st) || !S_ISREG(st.st_mode))
			continue;
		if (setxattr(child, "security.selinux", label, strlen(label),
			     0)) {
			relabel_failures++;
			if (!relabel_first_error)
				relabel_first_error = errno;
		} else {
			set++;
		}
	}
	closedir(dir);
	return set;
}

/* A wakeupN directory: the 6.12 per-device wakeup statistics. */
static int is_wakeup_dir(const char *name)
{
	size_t i;

	if (strncmp(name, "wakeup", 6))
		return 0;
	for (i = 6; name[i]; i++) {
		if (!isdigit((unsigned char)name[i]))
			return 0;
	}
	return i > 6;
}

/*
 * Read a node's current SELinux label.
 *
 * The policy fragment names the kgsl directory itself, and this worker
 * only ever sets labels on regular files, never on a directory. A
 * matching label on the directory is therefore proof that the genfscon
 * landed, and the file walk is unnecessary.
 */
static int read_selinux_label(const char *path, char *buf, size_t size);

/*
 * Walk the platform devices for the per-device wakeup directories. The
 * devices are at different depths (the remoteproc is directly under
 * soc@0, the PMIC is under spmi-0/0-02/...), so this is a bounded walk
 * rather than a fixed path.
 */
static int relabel_wakeups(const char *root, unsigned int depth)
{
	DIR *dir;
	struct dirent *entry;
	int set = 0;

	/*
	 * The depth matters: the remoteproc's statistics are two levels
	 * down, but the PMIC's are seven - soc@0/200f000.spmi/spmi-0/0-02/
	 * 200f000.spmi:pmic@2:qcom,fg/wakeup/wakeupNN - and the suspend
	 * service reads those too. A walk that stops at four leaves the
	 * PMIC's on the generic sysfs type, the checkpoint commit fails,
	 * and the phone reboots itself.
	 */
	if (depth > 9)
		return 0;
	dir = opendir(root);
	if (!dir) {
		relabel_skipped++;
		return 0;
	}
	while ((entry = readdir(dir))) {
		char child[1024];
		struct stat st;

		if (!strcmp(entry->d_name, ".") ||
		    !strcmp(entry->d_name, ".."))
			continue;
		if (snprintf(child, sizeof child, "%s/%s", root,
			     entry->d_name) >= (int)sizeof child)
			continue;
		if (lstat(child, &st) || !S_ISDIR(st.st_mode))
			continue;
		if (is_wakeup_dir(entry->d_name)) {
			char label[64];

			/*
			 * A directory that already carries the label is
			 * proof that the genfscon of the policy landed,
			 * and then the files under it carry it as well:
			 * the label of an object is the longest matching
			 * genfscon of its path, and the genfscon of the
			 * directory covers its subtree. Walking the files
			 * then buys nothing and costs an audit record for
			 * every file, because the policy does not let this
			 * domain set a label.
			 */
			if (!read_selinux_label(child, label, sizeof label) &&
			    !strcmp(label, DIAG_WAKEUP_LABEL))
				continue;

			set += relabel_dir(child, DIAG_WAKEUP_LABEL, 0);
		} else {
			set += relabel_wakeups(child, depth + 1);
		}
	}
	closedir(dir);
	return set;
}

/*
 * Read a node's current SELinux label.
 *
 * The policy fragment names the kgsl directory itself, and this worker
 * only ever sets labels on regular files, never on a directory. A
 * matching label on the directory is therefore proof that the genfscon
 * landed, and the file walk is unnecessary.
 */
static int read_selinux_label(const char *path, char *buf, size_t size)
{
	ssize_t n;

	if (size < 2)
		return -1;
	n = getxattr(path, "security.selinux", buf, size - 1);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static void relabel_sysfs_for_rom_policy(int log)
{
	char message[192], label[64];
	int n;

	relabel_failures = 0;
	relabel_skipped = 0;
	relabel_first_error = 0;

	n = relabel_wakeups("/sys/devices/platform", 0);
	snprintf(message, sizeof message,
		 "DIAG: wakeup stats relabelled labels=%d failures=%d skipped=%d%s\n",
		 n, relabel_failures, relabel_skipped,
		 relabel_failures ? " (see the errno below)" : "");
	say(log, message);
	if (relabel_failures)
		snprintf(message, sizeof message,
			 "DIAG: relabel errno=%d\n", relabel_first_error),
		say(log, message);

	relabel_failures = 0;
	relabel_skipped = 0;
	relabel_first_error = 0;

	if (!read_selinux_label(DIAG_KGSL_SYSFS, label, sizeof label) &&
	    !strcmp(label, DIAG_GPU_LABEL)) {
		say(log, "DIAG: kgsl sysfs labelled by policy\n");
	} else {
		n = relabel_dir(DIAG_KGSL_SYSFS, DIAG_GPU_LABEL, 0);
		snprintf(message, sizeof message,
			 "DIAG: kgsl sysfs relabelled labels=%d failures=%d skipped=%d\n",
			 n, relabel_failures, relabel_skipped);
		say(log, message);
		if (relabel_failures)
			snprintf(message, sizeof message,
				 "DIAG: kgsl relabel errno=%d\n",
				 relabel_first_error),
			say(log, message);
	}
}

/*
 * Downstream adsprpcd opens /dev/adsprpc-smd, but upstream FastRPC
 * registers /dev/fastrpc-adsp (misc). Same device, different name:
 * wait for the upstream node, then mknod the downstream alias with
 * identical numbers (mode 0666 for the daemon). Detached fork.
 */
static void adsprpc_compat_child(int log)
{
	time_t deadline = monotonic_seconds() + 240;
	char message[256], dev[32];
	unsigned int maj, min;
	ssize_t n;
	say(log, "DIAG: adsprpc compat: waiting for fastrpc-adsp\n");
	for (;;) {
		if (!read_file("/sys/class/misc/fastrpc-adsp/dev", dev,
			       sizeof(dev)) &&
		    sscanf(dev, "%u:%u", &maj, &min) == 2)
			break;
		if (monotonic_seconds() >= deadline) {
			say(log, "DIAG: adsprpc compat: fastrpc-adsp never appeared\n");
			_exit(0);
		}
		sleep(2);
	}
	if (mknod("/dev/adsprpc-smd", 0600 | S_IFCHR, makedev(maj, min)) &&
	    errno != EEXIST) {
		snprintf(message, sizeof(message),
			 "DIAG: adsprpc compat: mknod errno=%d\n", errno);
		say(log, message);
		_exit(0);
	}
	/* Stock identity (read off live stock): 0664 system:system
	 * u:object_r:qdsp_device:s0. Without the label the daemon gets
	 * avc denied on the tmpfs default context.
	 */
	chmod("/dev/adsprpc-smd", 0664);
	chown("/dev/adsprpc-smd", 1000, 1000);
	n = setxattr("/dev/adsprpc-smd", "security.selinux",
		     "u:object_r:qdsp_device:s0",
		     sizeof("u:object_r:qdsp_device:s0") - 1, 0);
	snprintf(message, sizeof(message),
		 "DIAG: adsprpc compat: alias %u:%u label ret=%d errno=%d\n",
		 maj, min, (int)n, n ? errno : 0);
	say(log, message);
	_exit(0);
}

static void adsprpc_compat_fork(int log)
{
	pid_t pid = fork();

	if (pid < 0) {
		say(log, "DIAG: adsprpc compat: fork failed\n");
		return;
	}
	if (!pid) {
		setsid();
		adsprpc_compat_child(log);
	}
}
/*
 * Debuggable props. Our /init replaced first-stage init, so the ramdisk
 * prop.default is never loaded; second-stage init takes ro.secure=1 /
 * ro.debuggable=0 from the system/vendor partitions and adbd stays shell.
 * Patch those keys in every on-device prop file that carries them and bind
 * the result over the original before second-stage init parses it, the
 * same way the USB handoff rc is installed. Missing files are skipped.
 */
static const char *const debuggable_props[] = {
	"/system/build.prop",
	"/vendor/build.prop",
	"/odm/etc/build.prop",
	"/product/etc/build.prop",
	"/system_ext/etc/build.prop",
	"/system/etc/prop.default",
	"/vendor/etc/prop.default",
	"/vendor/default.prop",
	"/odm/etc/prop.default",
	"/product/etc/prop.default",
	"/system_ext/etc/prop.default",
};

static int patch_prop_line(const char *line, size_t len, int fd)
{
	static const char secure[] = "ro.secure=";
	static const char debuggable[] = "ro.debuggable=";
	const char *value = NULL;

	if (len > sizeof(secure) - 1 &&
	    !memcmp(line, secure, sizeof(secure) - 1))
		value = "ro.secure=0\n";
	else if (len > sizeof(debuggable) - 1 &&
		 !memcmp(line, debuggable, sizeof(debuggable) - 1))
		value = "ro.debuggable=1\n";
	if (value)
		return write_all(fd, value, strlen(value));
	if (len && write_all(fd, line, len))
		return -1;
	return write_all(fd, "\n", 1);
}

static void guard_debuggable(int proc, int log)
{
	char message[256];
	size_t i;

	for (i = 0; i < sizeof(debuggable_props) / sizeof(debuggable_props[0]); i++) {
		const char *target = debuggable_props[i];
		int in = open(target, O_RDONLY | O_CLOEXEC);
		int fd = memfd_create("ack-debuggable-prop", MFD_CLOEXEC);
		int error = 0, patched = 0;
		struct stat st;
		static char text[128 * 1024 + 1];
		char *line;
		size_t left;
		ssize_t n;
		off_t offset = 0;

		if (in < 0 || fd < 0) {
			error = errno;
			goto done;
		}
		if (fstat(in, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
		    st.st_size > 128 * 1024) {
			error = errno ? errno : EINVAL;
			goto done;
		}
		while (offset < st.st_size) {
			n = pread(in, text + offset,
				  (size_t)(st.st_size - offset), offset);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				error = errno ? errno : EIO;
				goto done;
			}
			offset += n;
		}
		line = text;
		left = (size_t)st.st_size;
		while (left) {
			char *end = memchr(line, '\n', left);
			size_t len = end ? (size_t)(end - line) : left;

			if ((len >= sizeof("ro.secure=") - 1 &&
			     !memcmp(line, "ro.secure=",
				     sizeof("ro.secure=") - 1)) ||
			    (len >= sizeof("ro.debuggable=") - 1 &&
			     !memcmp(line, "ro.debuggable=",
				     sizeof("ro.debuggable=") - 1)))
				patched = 1;
			if (patch_prop_line(line, len, fd)) {
				error = errno;
				goto done;
			}
			if (!end)
				break;
			line = end + 1;
			left -= len + 1;
		}
		if (!patched) {
			error = ENODATA;
			goto done;
		}
		snprintf(message, sizeof(message),
			 "/dev/block/ack-debuggable-%zu.prop", i);
		if (guard_bind_config(proc, fd, message, target))
			error = errno;
done:
		if (in >= 0)
			close(in);
		if (fd >= 0)
			close(fd);
		snprintf(message, sizeof(message), "DIAG: debuggable %s errno=%d\n",
			 target, error);
		say(log, message);
	}
}

/* Validate the complete property before changing any bytes. */
static int patch_usb_controller(char *text, size_t *size)
{
	static const char key[] = "vendor.usb.controller";
	static const char old[] = "vendor.usb.controller=7000000.dwc3";
	static const char new[] = "vendor.usb.controller=7000000.usb";
	size_t offset = 0, match = 0;
	int found = 0, changed = 0;

	if (memchr(text, '\0', *size))
		goto invalid;
	while (offset < *size) {
		char *line = text + offset;
		char *end = memchr(line, '\n', *size - offset);
		size_t len = end ? (size_t)(end - line) : *size - offset;
		size_t value_len = len;
		char *start = line;
		char *equals = memchr(line, '=', len);
		char *key_end = equals;

		if (value_len && line[value_len - 1] == '\r')
			value_len--;
		if (equals) {
			while (start < equals && isspace((unsigned char)*start))
				start++;
			while (key_end > start &&
			       isspace((unsigned char)key_end[-1]))
				key_end--;
		}
		if (equals && (size_t)(key_end - start) == sizeof(key) - 1 &&
		    !memcmp(start, key, sizeof(key) - 1)) {
			if (found++)
				goto invalid;
			if (value_len == sizeof(old) - 1 &&
			    !memcmp(line, old, sizeof(old) - 1)) {
				match = offset;
				changed = 1;
			} else if (value_len != sizeof(new) - 1 ||
				   memcmp(line, new, sizeof(new) - 1)) {
				goto invalid;
			}
		}
		offset += len + !!end;
	}
	if (!found) {
		errno = ENODATA;
		return -1;
	}
	if (changed) {
		memmove(text + match + sizeof(new) - 1,
			text + match + sizeof(old) - 1,
			*size - match - (sizeof(old) - 1));
		memcpy(text + match, new, sizeof(new) - 1);
		*size -= sizeof(old) - sizeof(new);
	}
	return changed;
invalid:
	errno = EINVAL;
	return -1;
}

/* This ROM derives the vendor controller from sys.usb.controller at boot. */
static int patch_usb_controller_rc(char *text, size_t *size, size_t capacity)
{
	static const char vendor[] =
		"    setprop vendor.usb.controller ${sys.usb.controller}";
	static const char system[] = "    setprop sys.usb.controller 7000000.usb\n";
	size_t offset = 0, match = 0, sys_match = 0;
	int found = 0, sys_found = 0;

	if (memchr(text, '\0', *size))
		goto invalid;
	while (offset < *size) {
		char *line = text + offset;
		char *end = memchr(line, '\n', *size - offset);
		size_t len = end ? (size_t)(end - line) : *size - offset;
		char *start = line, *key, *key_end;

		while (start < line + len && isspace((unsigned char)*start))
			start++;
		if (line + len - start < 8 || memcmp(start, "setprop", 7) ||
		    !isspace((unsigned char)start[7]))
			goto next;
		key = start + 7;
		while (key < line + len && isspace((unsigned char)*key))
			key++;
		key_end = key;
		while (key_end < line + len && !isspace((unsigned char)*key_end))
			key_end++;
		if ((size_t)(key_end - key) == strlen("vendor.usb.controller") &&
		    !memcmp(key, "vendor.usb.controller", key_end - key)) {
			if (found++ || len != sizeof(vendor) - 1 ||
			    memcmp(line, vendor, len))
				goto invalid;
			match = offset;
		} else if ((size_t)(key_end - key) == strlen("sys.usb.controller") &&
			   !memcmp(key, "sys.usb.controller", key_end - key)) {
			if (sys_found++ || !end || len != sizeof(system) - 2 ||
			    memcmp(line, system, len))
				goto invalid;
			sys_match = offset;
		}
next:
		offset += len + !!end;
	}
	if (!found || (sys_found && sys_match + sizeof(system) - 1 != match))
		goto invalid;
	if (sys_found)
		return 0;
	if (*size > capacity || capacity - *size < sizeof(system) - 1) {
		errno = ENOSPC;
		return -1;
	}
	memmove(text + match + sizeof(system) - 1, text + match, *size - match);
	memcpy(text + match, system, sizeof(system) - 1);
	*size += sizeof(system) - 1;
	return 1;
invalid:
	errno = EINVAL;
	return -1;
}

static int guard_usb_controller(int proc, int log)
{
	static char text[128 * 1024];
	const char *target = "/vendor/build.prop";
	struct stat st;
	char message[128];
	off_t offset = 0;
	size_t size;
	int in = -1, fd = -1, error = 0, changed;

read_target:
	in = open(target, O_RDONLY | O_CLOEXEC);
	if (in < 0 || fstat(in, &st)) {
		error = errno;
		goto done;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > sizeof(text)) {
		error = EINVAL;
		goto done;
	}
	while (offset < st.st_size) {
		ssize_t n = pread(in, text + offset,
				  (size_t)(st.st_size - offset), offset);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			error = n < 0 ? errno : EIO;
			goto done;
		}
		offset += n;
	}
	size = (size_t)st.st_size;
	if (!strcmp(target, "/vendor/build.prop")) {
		changed = patch_usb_controller(text, &size);
		if (changed < 0 && errno == ENODATA) {
			close(in);
			in = -1;
			offset = 0;
			target = USB_RC;
			goto read_target;
		}
	} else {
		changed = patch_usb_controller_rc(text, &size, sizeof(text));
	}
	if (changed < 0) {
		error = errno;
		goto done;
	}
	if (!changed)
		goto done;
	fd = memfd_create("ack-usb-controller-prop", MFD_CLOEXEC);
	if (fd < 0 || write_all(fd, text, size) ||
	    guard_bind_config(proc, fd, "/dev/block/ack-usb-controller.prop",
			      target))
		error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: USB controller %s -> 7000000.usb errno=%d\n",
		 target, error);
	say(log, message);
	errno = error;
	return error ? -1 : 0;
}

/* Keep the ROM's FRP ownership rule attached to the ACK controller path. */
static int patch_config_ueventd(char *text, size_t size)
{
	static const char old[] =
		"/dev/block/platform/soc/7824900.sdhci/by-name/config";
	static const char new[] =
		"/dev/block/platform/soc@0/7824900.sdhci/by-name/config";
	size_t offset = 0, match = 0;
	int found = 0, changed = 0;

	if (memchr(text, '\0', size))
		goto invalid;
	while (offset < size) {
		char *line = text + offset;
		char *end = memchr(line, '\n', size - offset);
		size_t len = end ? (size_t)(end - line) : size - offset;
		char *start = line, *key_end;

		while (start < line + len && isspace((unsigned char)*start))
			start++;
		key_end = start;
		while (key_end < line + len && !isspace((unsigned char)*key_end))
			key_end++;
		if ((size_t)(key_end - start) == sizeof(old) - 1 &&
		    (!memcmp(start, old, sizeof(old) - 1) ||
		     !memcmp(start, new, sizeof(new) - 1))) {
			char rule[128], mode[16], uid[16], gid[16], extra;
			size_t tail = line + len - key_end;

			if (found++ || tail >= sizeof(rule))
				goto invalid;
			memcpy(rule, key_end, tail);
			rule[tail] = '\0';
			if (sscanf(rule, "%15s %15s %15s %c", mode, uid, gid, &extra) != 3 ||
			    strcmp(mode, "0600") || strcmp(uid, "system") ||
			    strcmp(gid, "system"))
				goto invalid;
			match = start - text;
			changed = !memcmp(start, old, sizeof(old) - 1);
		}
		offset += len + !!end;
	}
	if (!found)
		goto invalid;
	if (changed)
		memcpy(text + match, new, sizeof(new) - 1);
	return changed;
invalid:
	errno = EINVAL;
	return -1;
}

static int guard_config_ueventd(int proc, int log)
{
	static const char target[] = "/vendor/etc/ueventd.rc";
	static char text[128 * 1024];
	struct stat st;
	char message[128];
	size_t used = 0;
	int in = -1, fd = -1, error = 0, changed;

	in = open(target, O_RDONLY | O_CLOEXEC);
	if (in < 0 || fstat(in, &st))
		goto failed;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > (off_t)sizeof(text)) {
		errno = EINVAL;
		goto failed;
	}
	while (used < (size_t)st.st_size) {
		ssize_t n = pread(in, text + used, st.st_size - used, used);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (!n)
				errno = EIO;
			goto failed;
		}
		used += (size_t)n;
	}
	changed = patch_config_ueventd(text, used);
	if (changed < 0)
		goto failed;
	if (!changed)
		goto done;
	fd = memfd_create("ack-config-ueventd", MFD_CLOEXEC);
	if (fd < 0 || write_all(fd, text, used) ||
	    guard_bind_config(proc, fd, "/dev/block/ack-config-ueventd.rc", target))
		goto failed;
	goto done;
failed:
	error = errno;
done:
	if (in >= 0)
		close(in);
	if (fd >= 0)
		close(fd);
	snprintf(message, sizeof(message),
		 "DIAG: config ueventd ACK path 0600 system system errno=%d\n", error);
	say(log, message);
	errno = error;
	return error ? -1 : 0;
}

static int guard_bootdevice(int proc)
{
	struct stat st;
	char temporary[128];
	int error;

	if (guard_pre_policy(proc))
		return -1;
	if (!lstat("/dev/block/bootdevice", &st)) {
		if (!S_ISLNK(st.st_mode)) {
			errno = EEXIST;
			return -1;
		}
	} else if (errno != ENOENT) {
		return -1;
	}
	/* With bootdevice=../.. the stock vendor wait resolves through soc.
	 * Do not release that wait until both read-only overlays are verified. */
	if (make_dir("/dev/block/platform/soc") ||
	    stat("/dev/block/platform/soc", &st))
		return -1;
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	snprintf(temporary, sizeof temporary, "/dev/block/.bootdevice-ack-%ld",
		 (long)getpid());
	if (symlink(BOOT_CONTROLLER, temporary))
		return -1;
	if (guard_pre_policy(proc) ||
	    rename(temporary, "/dev/block/bootdevice")) {
		error = errno;
		unlink(temporary);
		errno = error;
		return -1;
	}
	return 0;
}

static void install_android_guard(int proc, int fstab, int contexts, int log)
{
	time_t deadline = monotonic_seconds() + 60;
	int ns = -1, root = -1;
	int n;
	int ready[2] = { -1, -1 };
	struct stat st;
	char message[192];
	const char *step = "validate protected fstab";

	/*
	 * Relabel the nodes the ROM's policy expects to be labelled, in a
	 * process of their own, in the initramdisk's own mount namespace.
	 *
	 * The namespace is the reason it is a separate process. After the
	 * setns below, /sys is the ROM's, and the ROM has not mounted it
	 * yet, so the walk finds nothing - that is why an earlier version
	 * of this code, which ran at the end of the guard install,
	 * reported that it could not open its directory and changed no
	 * labels at all.
	 *
	 * The timing is the reason it waits. The guard has to install the
	 * read-only fstab and the vendor contexts before the ROM's init
	 * opens its SELinux policy, and that window is about 1.35 seconds
	 * wide. The relabel walks the whole device tree, which is enough
	 * CPU to push the guard's own work past the end of that window,
	 * and a guard that misses it fails, and the ROM's init then waits
	 * for a boot device that nobody releases: the phone comes up on a
	 * dim lit screen with no adb and no USB.
	 *
	 * So the child waits for this process to say the guard is done,
	 * and only then walks. It has all the time it needs: the suspend
	 * service reads the statistics at around two minutes.
	 */
	if (pipe(ready) < 0)
		goto failed;
	if (fork() == 0) {
		char c;

		close(ready[1]);
		/* Wait for the guard to be finished with its window. */
		if (read(ready[0], &c, 1) <= 0) {
			/* The guard failed; there is nothing to relabel. */
			_exit(0);
		}
		close(ready[0]);
		/*
		 * The initramdisk's own mount namespace has no /sys: the
		 * kernel mounts nothing there, and the ROM mounts its own
		 * in the ROM's own namespace, which this process is not
		 * in. So the walk has to mount the kernel's sysfs for
		 * itself. Without this the walk found nothing and changed
		 * no labels at all, and the only sign of it was a "ret=-1"
		 * in the log.
		 */
		mkdir("/sys", 0755);
		if (mount("sysfs", "/sys", "sysfs", 0, NULL))
			say(log, "DIAG: relabel could not mount sysfs\n");
		/*
		 * The label of a node can only be set once the policy is
		 * loaded, and the ROM's init loads it a few seconds after
		 * it starts. So the walk is repeated: the early attempts
		 * do nothing, and a later one lands.
		 */
		for (n = 0; n < 30; n++) {
			relabel_sysfs_for_rom_policy(log);
			sleep(2);
		}
		_exit(0);
	}
	close(ready[0]);
	ready[0] = -1;

	if (validate_guard_fstab(fstab))
		goto failed;
	for (;;) {
		if (guard_pre_policy(proc))
			goto failed;
		step = "enter PID 1 mount namespace/root";
		ns = openat(proc, "1/ns/mnt", O_RDONLY | O_CLOEXEC);
		root = openat(proc, "1/root", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (ns < 0 || root < 0 || setns(ns, CLONE_NEWNS) ||
		    fchdir(root) || chroot(".") || chdir("/"))
			goto failed;
		close(ns);
		close(root);
		ns = root = -1;
		if (!stat("/vendor/etc/fstab.qcom", &st) &&
		    S_ISREG(st.st_mode) &&
		    !stat("/vendor/etc/selinux/vendor_file_contexts", &st) &&
		    S_ISREG(st.st_mode))
			break;
		if (monotonic_seconds() >= deadline) {
			errno = ETIMEDOUT;
			goto failed;
		}
		usleep(10000);
	}
	step = "install read-only protected fstab";
	if (guard_bind_config(proc, fstab, GUARDED_FSTAB,
			      "/vendor/etc/fstab.qcom"))
		goto failed;
	step = "install read-only vendor contexts";
	if (guard_bind_config(proc, contexts, GUARDED_CONTEXTS,
			      "/vendor/etc/selinux/vendor_file_contexts"))
		goto failed;
	step = "install read-only USB controller property";
	if (guard_usb_controller(proc, log))
		goto failed;
	step = "install read-only config partition ownership rule";
	if (guard_config_ueventd(proc, log))
		goto failed;
	/*
	 * flags_health_check.rc lives in /system/etc/init and is parsed at
	 * the start of second-stage init, before logd.rc and long before the
	 * vendor files.  Install this overlay in the pre-policy window, in
	 * the same pass as the early-logcat overlay.  At the end of the
	 * sequence below the ROM has already read the file, so the bind
	 * cannot take effect even if it succeeds.
	 */
	guard_flags_health_check(proc, log);
	guard_vendor_qrtr(proc, log);
#ifdef ANDROID_EARLY_LOGCAT
	guard_early_logcat(proc, log);
#endif
	step = "release canonical bootdevice and legacy wait";
	if (guard_bootdevice(proc))
		goto failed;
	say(log, "DIAG: bootdevice guard installed read-only f2fs-only fstab"
		 " and vendor contexts before policy load\n");
	say(log, "DIAG: bootdevice canonical alias and legacy wait path ready\n");
	/* After the alias release: a late override must never block boot. */
	/*
	 * Create g1 here, before Android's init runs, and leave it unbound so
	 * adbd can take the UDC. This is the same point the USB handoff used
	 * to fire from, and it must stay on the ANDROID_CAPTURE path: the
	 * console-capture block in main() is after an early `return
	 * launch_android(argv)`, so a call there is dead code in this build.
	 */
	if (setup_gadget(0))
		say(log, "DIAG: gadget g1 create failed; Android USB may stall\n");
	else
		say(log, "DIAG: gadget g1 created, UDC left for adbd\n");
#ifdef ANDROID_USB_CONSOLE
	guard_usb_handoff(proc, log);
#endif
#ifdef ANDROID_DISPLAY_GATE
	guard_displaygate(proc, log);
#endif
	guard_firmware(log);
	harvest_modem_firmware(log);
	adsprpc_compat_fork(log);
	guard_debuggable(proc, log);
	/*
	 * The guard's window is closed: the fstab and the vendor
	 * contexts are in place, so the relabel can walk the device tree
	 * now without taking CPU from the work that had to happen
	 * before the ROM's init opened its policy.
	 */
	if (ready[1] >= 0) {
		char c = 0;

		if (write(ready[1], &c, 1) != 1)
			say(log, "DIAG: relabel child was not woken\n");
		close(ready[1]);
		ready[1] = -1;
	}
	_exit(0);
failed:
	snprintf(message, sizeof message,
		 "DIAG: bootdevice guard step=%s failed errno=%d; alias not released\n",
		 step, errno);
	say(log, message);
	_exit(1);
}

#ifdef ANDROID_GUARD_TEST
static void config_ueventd_selftest(void)
{
	static const char old[] =
		"# preserved\n/dev/block/platform/soc/7824900.sdhci/by-name/config"
		"    0600   system     system\r\nother 0600 root root\n";
	static const char new[] =
		"# preserved\n/dev/block/platform/soc@0/7824900.sdhci/by-name/config"
		"    0600   system     system\r\nother 0600 root root\n";
	static const char *const invalid[] = {
		"", "# /dev/block/platform/soc/7824900.sdhci/by-name/config\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0666 system system\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0600 root system\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0600 system root\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0600 system\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0600 system system extra\n",
		"/dev/block/platform/soc/7824900.sdhci/by-name/config 0600 system system\n"
		"/dev/block/platform/soc@0/7824900.sdhci/by-name/config 0600 system system\n",
	};
	char text[512];
	size_t i, size;

	memcpy(text, old, sizeof(old));
	assert(patch_config_ueventd(text, sizeof(old) - 1) == 1);
	assert(sizeof(old) == sizeof(new) && !memcmp(text, new, sizeof(new)));
	assert(!patch_config_ueventd(text, sizeof(new) - 1));
	assert(!memcmp(text, new, sizeof(new)));
	assert(patch_config_ueventd(text, sizeof(new)) == -1);
	assert(errno == EINVAL);
	for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		size = strlen(invalid[i]);
		memcpy(text, invalid[i], size);
		assert(patch_config_ueventd(text, size) == -1);
		assert(errno == EINVAL && !memcmp(text, invalid[i], size));
	}
	puts("Guard config ueventd ownership and rejection checks PASS");
}

static void usb_controller_selftest(void)
{
	static const struct {
		const char *input;
		const char *output;
		int result;
	} properties[] = {
		{ "other=1\n", "other=1\n", -1 },
		{ "vendor.usb.controller=7000000.dwc3\r\nother=1\n",
		  "vendor.usb.controller=7000000.usb\r\nother=1\n", 1 },
		{ "vendor.usb.controller=7000000.usb",
		  "vendor.usb.controller=7000000.usb", 0 },
		{ "vendor.usb.controller=wrong\n", NULL, -1 },
		{ " vendor.usb.controller=7000000.dwc3\n", NULL, -1 },
		{ "vendor.usb.controller=7000000.dwc3\n"
		  "vendor.usb.controller=7000000.usb\n", NULL, -1 },
	};
	static const char rc[] = "on boot\n"
		"    setprop vendor.usb.controller ${sys.usb.controller}\n"
		"    setprop other 1\n";
	static const char expected[] = "on boot\n"
		"    setprop sys.usb.controller 7000000.usb\n"
		"    setprop vendor.usb.controller ${sys.usb.controller}\n"
		"    setprop other 1\n";
	static const char *const invalid_rc[] = {
		"on boot\n",
		"    setprop vendor.usb.controller wrong\n",
		"\tsetprop vendor.usb.controller ${sys.usb.controller}\n",
		"    setprop vendor.usb.controller ${sys.usb.controller}\n"
		"    setprop vendor.usb.controller ${sys.usb.controller}\n",
		"    setprop sys.usb.controller wrong\n"
		"    setprop vendor.usb.controller ${sys.usb.controller}\n",
		"    setprop sys.usb.controller 7000000.usb\n"
		"    setprop other 1\n"
		"    setprop vendor.usb.controller ${sys.usb.controller}\n",
	};
	char text[512], saved[512];
	size_t size, i;

	for (i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
		size = strlen(properties[i].input);
		memcpy(text, properties[i].input, size);
		assert(patch_usb_controller(text, &size) == properties[i].result);
		if (properties[i].output) {
			assert(size == strlen(properties[i].output));
			assert(!memcmp(text, properties[i].output, size));
		}
		if (!i)
			assert(errno == ENODATA);
	}
	size = sizeof(rc) - 1;
	memcpy(text, rc, size);
	assert(patch_usb_controller_rc(text, &size, size) == -1);
	assert(errno == ENOSPC && size == sizeof(rc) - 1);
	assert(!memcmp(text, rc, size));
	assert(patch_usb_controller_rc(text, &size, sizeof(text)) == 1);
	assert(size == sizeof(expected) - 1 && !memcmp(text, expected, size));
	assert(!patch_usb_controller_rc(text, &size, sizeof(text)));
	assert(size == sizeof(expected) - 1 && !memcmp(text, expected, size));
	for (i = 0; i < sizeof(invalid_rc) / sizeof(invalid_rc[0]); i++) {
		size = strlen(invalid_rc[i]);
		memcpy(text, invalid_rc[i], size);
		memcpy(saved, text, size);
		assert(patch_usb_controller_rc(text, &size, sizeof(text)) == -1);
		assert(errno == EINVAL && size == strlen(invalid_rc[i]));
		assert(!memcmp(text, saved, size));
	}
	size = sizeof(rc);
	memcpy(text, rc, size);
	assert(patch_usb_controller_rc(text, &size, sizeof(text)) == -1);
	assert(errno == EINVAL);
	assert(patch_usb_controller(text, &size) == -1);
	assert(errno == EINVAL);
	puts("Guard USB controller property and runtime-rule checks PASS");
}

static int guard_selftest(void)
{
	static const char data[] =
		"/dev/block/bootdevice/by-name/userdata /data f2fs defaults wait\n";
	static const char *const invalid[] = {
		"", "/dev/block/bootdevice/by-name/userdata /data ext4 defaults wait\n",
		"/dev/block/bootdevice/by-name/userdata /data f2fs defaults wait,formattable\n",
		"/dev/block/bootdevice/by-name/userdata /data f2fs defaults formattable=false\n",
		"/dev/mmcblk1p2 /data f2fs defaults wait\n",
		"/dev/block/bootdevice/by-name/userdata /data f2fs defaults\n",
		"/dev/block/bootdevice/by-name/userdata /data f2fs defaults wait extra\n",
	};
	static const char health_rc[] =
		"on load_persist_props_action\n"
		"    exec - system system -- /system/bin/flags_health_check BOOT_FAILURE\n"
		"\n"
		"on property:sys.boot_completed=1\n"
		"    setprop persist.device_config.attempted_boot_count 0\n"
		"\n"
		"on property:sys.init.updatable_crashing=1\n"
		"    exec - system system -- /system/bin/flags_health_check UPDATABLE_CRASHING\n"
		"\n"
		"on property:sys.boot_completed=1\n"
		"    setprop after.trigger 1\n";
	char temporary[] = "/tmp/ack-fstab-check-XXXXXX";
	char filtered[sizeof(health_rc)];
	size_t filtered_len = 0;
	int fd = mkstemp(temporary);
	size_t i;

	assert(fd >= 0);
	assert(!unlink(temporary));
	assert(!write_all(fd, data, sizeof data - 1));
	assert(!validate_guard_fstab(fd));
	assert(!write_all(fd, data, sizeof data - 1));
	assert(validate_guard_fstab(fd) < 0);
	for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
		assert(!ftruncate(fd, 0));
		assert(lseek(fd, 0, SEEK_SET) == 0);
		assert(!write_all(fd, invalid[i], strlen(invalid[i])));
		assert(validate_guard_fstab(fd) < 0);
	}
	close(fd);
	assert(!filter_flags_health_check_rc(health_rc, sizeof(health_rc) - 1,
					     filtered, sizeof(filtered),
					     &filtered_len));
	assert(!memmem(filtered, filtered_len, "UPDATABLE_CRASHING",
		       sizeof("UPDATABLE_CRASHING") - 1));
	assert(memmem(filtered, filtered_len, "BOOT_FAILURE",
		      sizeof("BOOT_FAILURE") - 1));
	assert(memmem(filtered, filtered_len, "attempted_boot_count 0",
		      sizeof("attempted_boot_count 0") - 1));
	assert(memmem(filtered, filtered_len, "after.trigger",
		      sizeof("after.trigger") - 1));
	assert(filter_flags_health_check_rc("on x\n", 5, filtered,
					    sizeof(filtered), &filtered_len) ==
	       -ENODATA);
	puts("Guard flags_health_check filter checks PASS");
	puts("Guard fstab safety checks PASS");
	usb_controller_selftest();
	config_ueventd_selftest();
	return 0;
}
#endif

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
/* Proc-relative component probe for the PID 1 namespace/root failure.
 * openat(O_PATH) performs no data access, only parent search, so it names
 * the exact failing depth where a plain openat cannot. Read-only. */
static void report_proc_component(int log, int proc, const char *rel)
{
	int fd = openat(proc, rel, O_PATH | O_CLOEXEC);
	struct stat st;
	char target[256] = "-", label[256], message[640];
	ssize_t n;
	int label_error = 0;

	if (fd < 0) {
		snprintf(message, sizeof message,
			 "DIAG: proc component=%s open_errno=%d\n", rel, errno);
		say(log, message);
		return;
	}
	if (fstat(fd, &st)) {
		snprintf(message, sizeof message,
			 "DIAG: proc component=%s fstat_errno=%d\n", rel, errno);
		say(log, message);
		close(fd);
		return;
	}
	if (S_ISLNK(st.st_mode)) {
		n = readlinkat(proc, rel, target, sizeof target - 1);
		if (n < 0)
			snprintf(target, sizeof target, "readlink_errno=%d", errno);
		else
			target[n] = '\0';
	}
	n = fgetxattr(fd, "security.selinux", label, sizeof label - 1);
	if (n < 0) {
		label_error = errno;
		label[0] = '\0';
	} else {
		label[n] = '\0';
	}
	snprintf(message, sizeof message,
		 "DIAG: proc component=%s mode=%o uid=%u dev=%u:%u ino=%lu"
		 " target=%s label=%s label_errno=%d\n",
		 rel, (unsigned)st.st_mode, (unsigned)st.st_uid,
		 major(st.st_dev), minor(st.st_dev), (unsigned long)st.st_ino,
		 target, label, label_error);
	say(log, message);
	close(fd);
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
	const int first_snapshot = 120;
	size_t i;
	char message[160];
	const char *step = "open procfs";

	/* Keep procfs reachable even if init removes our old root's /proc path. */
	proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (proc < 0)
		goto failed;
	while (monotonic_seconds() < first_snapshot)
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
		while (monotonic_seconds() < first_snapshot + sample * 30)
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
	if (ns < 0 && proc >= 0 && !strcmp(step, "open mount namespace")) {
		static const char *const components[] = {
			"1", "1/ns", "1/ns/mnt", "1/root",
		};
		size_t k;

		for (k = 0; k < sizeof components / sizeof components[0]; k++)
			report_proc_component(log, proc, components[k]);
	}
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
	} else if (dump_log(kin, out, 900, 0, NULL, NULL, NULL)) {
		snprintf(msg, sizeof msg,
			 "DIAG: USB dump failed errno=%d\n", errno);
		say(out, msg);
	} else {
		say(out, "\nDIAG: EOF Android USB kmsg dump complete\n");
	}
	_exit(0);
}

/*
 * init logs every triggered action to kmsg (printk.devkmsg=on), so the
 * on property:sys.boot_completed=1 action marks a completed boot.
 */
static void boot_deadline(int kin, int klog)
{
	static const char done[] = "processing action (sys.boot_completed=1";
	char rec[1024], msg[128];
	struct pollfd p = { .fd = kin, .events = POLLIN };
	ssize_t got;

	while (monotonic_seconds() < BOOT_DEADLINE_SECONDS) {
		got = read(kin, rec, sizeof(rec) - 1);
		if (got > 0) {
			if (memmem(rec, (size_t)got, done, sizeof(done) - 1)) {
				snprintf(msg, sizeof(msg),
					 "DIAG: boot completed t=%ld; deadline reboot disarmed\n",
					 (long)monotonic_seconds());
				say(klog, msg);
				_exit(0);
			}
		} else if (got < 0 && errno == EAGAIN) {
			poll(&p, 1, 1000);
		} else if (got < 0 && (errno == EACCES || errno == EPERM)) {
			/*
			 * Enforcing SELinux denies this pre-policy (u:r:kernel)
			 * child its inherited kmsg fd; every retry is another
			 * avc line on the console (262 in snapshot60 run25).
			 * Completion is unobservable from here on, so keep the
			 * deadline reboot but stop reading.
			 */
			snprintf(msg, sizeof(msg),
				 "DIAG: deadline kmsg read denied errno=%d t=%ld; sleeping to deadline\n",
				 errno, (long)monotonic_seconds());
			say(klog, msg);
			while (monotonic_seconds() < BOOT_DEADLINE_SECONDS)
				sleep(5);
			break;
		} else if (got == 0 || (errno != EINTR && errno != EPIPE)) {
			sleep(1);
		}
	}
	snprintf(msg, sizeof(msg),
		 "DIAG: boot not completed within %d s; rebooting\n",
		 BOOT_DEADLINE_SECONDS);
	say(klog, msg);
	/* Let the recorders copy the marker, then flush every sink. */
	sleep(2);
	sync();
	reboot(RB_AUTOBOOT);
	_exit(1);
}

/*
 * Load the port's kernel modules from the ramdisk, in the order the build
 * wrote to /ack-modules/load-order. Built in, the CAF audio stack takes the
 * kernel's image_size past the DTB the bootloader places at 0x13400000 and
 * the phone does not boot, so it is modules
 * (porting/findings/2026-10-21-kernel-overruns-the-dtb.md).
 *
 * They are not in /lib/modules on purpose: Android's first-stage init loads
 * that directory itself and aborts the boot if any module fails. Here a
 * module that does not load costs its own function, not the boot. Each
 * result goes to the kernel log. Drivers that load before their devices
 * exist (the APR channel appears only once the ADSP is up) probe later, as
 * they would built in.
 */
static int load_ack_module(int log, const char *name, int fd)
{
	char message[384];
	int error = 0;

	if (fd < 0 || syscall(SYS_finit_module, fd, "", 0))
		error = fd < 0 ? ENOENT : errno;
	if (fd >= 0)
		close(fd);
	/* EEXIST: already in the kernel, which is what was wanted. */
	if (error == EEXIST)
		error = 0;
	snprintf(message, sizeof message, "DIAG: module %s %s errno=%d\n",
		 name, error ? "failed" : "loaded", error);
	say(log, message);
	return error ? 1 : 0;
}

static int open_ack_module(const char *name)
{
	char path[320];

	snprintf(path, sizeof path, "/ack-modules/%s", name);
	return open(path, O_RDONLY | O_CLOEXEC);
}

static void load_ack_modules(int log)
{
	/*
	 * The list is read whole first: a FILE shared across the fork below
	 * shares its file offset, and the parent closing it moved the child's
	 * (run333d loaded the late module twice).
	 */
	static char text[4096];
	char *names[64], message[384], *line, *save = NULL;
	int fds[64], count = 0, late_at = -1, i, fd;
	unsigned int delay = 0, loaded = 0, failed = 0;
	ssize_t len;

	fd = open("/ack-modules/load-order", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		say(log, "DIAG: no /ack-modules/load-order; no modules loaded\n");
		return;
	}
	len = read(fd, text, sizeof text - 1);
	close(fd);
	if (len <= 0)
		return;
	text[len] = '\0';
	for (line = strtok_r(text, "\r\n", &save); line && count < 64;
	     line = strtok_r(NULL, "\r\n", &save)) {
		if (!line[0] || line[0] == '#')
			continue;
		/*
		 * "@late <seconds>": load the rest from a detached child after
		 * that long, once the recorder is writing the kernel log to
		 * logdump, so a module that resets the phone leaves its log.
		 */
		if (!strncmp(line, "@late", 5)) {
			if (late_at < 0) {
				late_at = count;
				delay = (unsigned int)strtoul(line + 5, NULL, 10);
			}
			continue;
		}
		names[count++] = line;
	}
	for (i = 0; i < (late_at < 0 ? count : late_at); i++) {
		failed += load_ack_module(log, names[i], open_ack_module(names[i]));
		loaded++;
	}
	if (late_at >= 0 && late_at < count) {
		pid_t late;

		/*
		 * Open the late modules now: by the time they load, Android's
		 * init has switched root and deleted the ramdisk's files
		 * (run333d: ENOENT). An open file keeps its contents.
		 */
		for (i = late_at; i < count; i++)
			fds[i] = open_ack_module(names[i]);
		late = fork();
		if (late == 0) {
			snprintf(message, sizeof message,
				 "DIAG: loading %d module(s) in %u s\n",
				 count - late_at, delay);
			say(log, message);
			sleep(delay);
			for (i = late_at; i < count; i++)
				failed += load_ack_module(log, names[i], fds[i]);
			snprintf(message, sizeof message,
				 "DIAG: late modules loaded=%d failed=%u\n",
				 count - late_at - (int)failed, failed);
			say(log, message);
			/* The late child must never return to the caller. */
			_exit(0);
		}
		for (i = late_at; i < count; i++)
			if (fds[i] >= 0)
				close(fds[i]);
		if (late < 0)
			say(log, "DIAG: @late fork failed; late modules not loaded\n");
	}
	snprintf(message, sizeof message, "DIAG: modules loaded=%u failed=%u\n",
		 loaded - failed, failed);
	say(log, message);
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
		int in = -1, out = -1, kin = -1, klog = -1, kerr = 0, n, kdl;
		int proc = -1, fstab = -1, contexts = -1, sd = -1;
		size_t remaining = LOGDUMP_TEXT_LIMIT;
		size_t *primary_remaining = NULL;
		struct ramoops_sampler sampler = {
			.input = -1, .output = { -1, -1 }, .log = -1,
		};
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
		kdl = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
		klog = open("/dev/kmsg", O_WRONLY);
		if (klog < 0)
			kerr = errno;
		/*
		 * Before the handshake: Android's init starts with them in.
		 * In a child of its own: a module whose init oopses kills the
		 * process loading it, and if that were this one, the bootdevice
		 * guard below would never be forked and Android's init would
		 * wait forever for the boot device (run332, run332c). Wait for
		 * it, but not forever: a load stuck in the kernel must not hold
		 * the boot either.
		 */
		{
			pid_t loader = fork();

			if (loader == 0) {
				close(ready[1]);
				close(in);
				load_ack_modules(klog);
				_exit(0);
			}
			if (loader > 0) {
				int status, waited;

				for (waited = 0; waited < 300; waited++) {
					if (waitpid(loader, &status, WNOHANG) == loader)
						break;
					usleep(100000);
				}
				if (waited == 300)
					say(klog, "DIAG: module loader still running after 30 s; boot continues\n");
				else if (!WIFEXITED(status) || WEXITSTATUS(status))
					say(klog, "DIAG: module loader died (a module's init crashed?); boot continues\n");
			} else {
				say(klog, "DIAG: module loader fork failed; loading inline\n");
				load_ack_modules(klog);
			}
		}
		for (n = 0; n < POLL_TRIES; n++) {
			out = open_logdump();
			if (out >= 0)
				break;
			usleep(500000);
		}
		if (out >= 0)
			primary_remaining = &remaining;
		/* Retain inputs and procfs before Android removes the old ramdisk.
		 * The guard, unlike the recorder, joins PID 1's namespace. */
		proc = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		fstab = open("/ack-protected-fstab.qcom", O_RDONLY | O_CLOEXEC);
		contexts = open("/ack-vendor-file-contexts", O_RDONLY | O_CLOEXEC);
		if (proc >= 0 && fstab >= 0 && contexts >= 0) {
			pid_t guard = fork();
			if (guard == 0) {
				close(ready[1]);
				close(in);
				close(out);
				if (kin >= 0)
					close(kin);
				install_android_guard(proc, fstab, contexts, klog);
			}
			if (guard < 0)
				say(klog, "DIAG: bootdevice guard fork failed; alias not released\n");
		} else {
			say(klog, "DIAG: bootdevice guard inputs unavailable; alias not released\n");
		}
		if (proc >= 0)
			close(proc);
		if (fstab >= 0)
			close(fstab);
		if (contexts >= 0)
			close(contexts);
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
		if (BOOT_DEADLINE_SECONDS > 0 && kdl >= 0 && klog >= 0 &&
		    fork() == 0) {
			close(ready[1]);
			close(in);
			close(out);
			if (kin >= 0)
				close(kin);
			boot_deadline(kdl, klog);
		}
		if (kdl >= 0)
			close(kdl);
		/* Open the card before Android loads SELinux policy. Forked USB
		 * and guard children never inherit the SD fd: one writer only. */
		sd = open_sdlog();
		if (sd < 0)
			say(klog, "DIAG: SD capture unavailable; other sinks continue\n");
		else {
			say(klog, "DIAG: SD capture ready at /sdlog/ack-kmsg.log\n");
			sampler.log = klog;
			ramoops_sampler_open(&sampler);
		}
		if (out < 0) {
			out = sd;
			sd = -1;
		}
		if (out < 0 ||
		    !(write_capture(out, &sd, marker, sizeof(marker) - 1, 1,
				     primary_remaining) & 3))
			goto child_failed;
		ramoops_sampler_tick(&sampler);
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
				write_capture(out, &sd, "DIAG: Android init exec succeeded\n",
					      sizeof("DIAG: Android init exec succeeded\n") - 1,
					      1, primary_remaining);
			else {
				snprintf(message, sizeof message,
					 "DIAG: Android init exec failed or handshake lost: errno=%d\n",
					 error);
				write_capture(out, &sd, message, strlen(message), 1,
					      primary_remaining);
			}
			fsync(out);
		}
		close(ready[1]);
		/*
		 * Three minutes covers boot to the lock screen. Every record is
		 * fsync()ed, so a longer capture costs the eMMC megabytes a
		 * second for the whole window.
		 */
		if (dump_log(in, out, 180, 1, &sd, &sampler, primary_remaining)) {
			static const char failed[] = "\nDIAG: Android capture failed\n";
			write_capture(out, &sd, failed, sizeof(failed) - 1, 1,
				      primary_remaining);
		} else {
			static const char eof[] = "\nDIAG: EOF Android kmsg dump complete\n";
			write_capture(out, &sd, eof, sizeof(eof) - 1, 1,
				      primary_remaining);
		}
		ramoops_sampler_close(&sampler);
		if (sd >= 0)
			close(sd);
		close(out);
		close(in);
		_exit(0);
child_failed:
		ramoops_sampler_close(&sampler);
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
	size_t remaining = LOGDUMP_TEXT_LIMIT;
	size_t *primary_remaining = NULL;

	(void)argc;
	(void)argv;
#ifdef ANDROID_CAPTURE
#ifdef ANDROID_GUARD_TEST
	return guard_selftest();
#endif
#ifdef ANDROID_RAMOOPS_TEST
	if (argc == 2 && !strcmp(argv[1], "--prefix-only"))
		return prefix_budget_selftest();
	{
		int rc = ramoops_selftest();

		if (rc)
			return rc;
		rc = sink_retirement_selftest();
		if (rc)
			return rc;
		return prefix_budget_selftest();
	}
#endif
	return launch_android(argv);
#endif
#ifdef HEARTBEAT_RECORDER
	if (argc == 2 && !strcmp(argv[1], "--usb-setup")) {
		/* PID 1 owns /dev, /proc and /sys; do not hide its later mounts. */
		mkdir("/sys/kernel/config", 0755);
		mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
		for (n = 0; n < POLL_TRIES; n++) {
			if (!setup_gadget(1)) {
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
#ifdef RESET_PATH_DIAG
	/* Recovery stays out of Android and streams independently of disk I/O. */
	out = open_usb_sink();
	usb = 1;
#else
	out = open_sink(console, &usb);
#endif
	if (out < 0) {
		say(console, "DIAG: no storage or USB sink\n");
		goto spin;
	}
	if (!usb)
		primary_remaining = &remaining;
	/* Allow time for the host to configure USB before consuming kmsg. */
	for (n = 0; n < (usb ? POLL_TRIES : 1); n++) {
		static const char marker[] = "DIAG: init running; kernel log follows\n";
		if (!write_all_limit(out, marker, sizeof(marker) - 1, primary_remaining))
			break;
		usleep(500000);
	}
	if (n == (usb ? POLL_TRIES : 1)) {
		say(console, "DIAG: sink write failed\n");
		goto spin;
	}
	if (dump_log(in, out, DUMP_SECONDS, DURABLE_CAPTURE && !usb, NULL, NULL,
		     primary_remaining)) {
		say(console, "DIAG: kernel log capture failed\n");
		goto spin;
	}
#ifndef RESET_PANIC_TEST
	{
#ifdef RESET_PATH_DIAG
		static const char eof[] = "\nDIAG: EOF Android USB kmsg dump complete\n";
#else
		static const char eof[] = "\nDIAG: EOF kmsg dump complete\n";
#endif

		write_all_limit(out, eof, sizeof(eof) - 1, primary_remaining);
	}
#endif
	if (!usb && fsync(out))
		say(console, "DIAG: logdump fsync failed\n");
	say(console, "DIAG: dump finished\n");
#ifdef RESET_PANIC_TEST
	/* Deliberate retention control, never present in the experiment init. */
	{
		int trigger = open("/proc/sysrq-trigger", O_WRONLY | O_CLOEXEC);

		say(console, "DIAG: controlled reset-marker panic test\n");
		say(out, "DIAG: controlled reset-marker panic test\n");
		sync();
		if (trigger >= 0) {
			if (write(trigger, "c", 1) != 1)
				say(console, "DIAG: panic test trigger failed\n");
			close(trigger);
		} else {
			say(console, "DIAG: panic test trigger unavailable\n");
		}
	}
#endif

spin:
	/* Stay alive even if no sink works; absence of a reset alone is not proof. */
	say(console, "DIAG: looping forever\n");
	for (;;)
		pause();
	return 0;
}
