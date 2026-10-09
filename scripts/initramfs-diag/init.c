/*
 * Diagnostic initramfs /init for the tissot ACK 6.12 port.
 *
 * Capture /dev/kmsg to the partition identified by PARTNAME=logdump, or
 * stream it through a CDC-ACM gadget if storage is unavailable. devtmpfs
 * provides /dev/<device>, not Android's /dev/block hierarchy.
 * Default mode never returns after setup; recovery requires a reboot.
 * Build with -DANDROID_CAPTURE to exec the original /system/bin/init as
 * PID 1 while a child records for 900 seconds in a private mount namespace.
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
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(ANDROID_CAPTURE) && \
	(defined(ANDROID_GUARD_TEST) || defined(ANDROID_RAMOOPS_TEST) || \
	 defined(ANDROID_DAEMON_TEST))
#include <assert.h>
#include <stdlib.h>
#endif

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
struct ramoops_sampler;

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

/* A failed backup must never take down the primary recorder. */
static int write_capture(int out, int *mirror, const char *buf, size_t len,
			 int durable)
{
	if (write_all(out, buf, len) || (durable && fsync(out)))
		return -1;
	if (mirror && *mirror >= 0 &&
	    (write_all(*mirror, buf, len) || fsync(*mirror))) {
		close(*mirror);
		*mirror = -1;
		say(out, "\nDIAG: SD mirror failed; primary capture continues\n");
	}
	return 0;
}

#ifdef ANDROID_CAPTURE
/* comm is capped at TASK_COMM_LEN-1 (15 visible bytes), so watch names match
 * by prefix only; full-string equality would never fire. */
static const char *const daemon_watch[] = {
	"keymaster@3.0", "keystore2", "vold", "vdc",
	"servicemanager", "logd",
};
#define DAEMON_WATCH_COUNT (sizeof daemon_watch / sizeof daemon_watch[0])

struct daemon_probe {
	char last[DAEMON_WATCH_COUNT][48];
	time_t emitted[DAEMON_WATCH_COUNT];
	/* -1 before the first scan so the census line always emits once. */
	int comm_errno;
	unsigned long entries;
	unsigned long highest;
};

/* Owned by the durable recorder child; the USB child never writes it. */
static struct daemon_probe daemon_probe_state;

struct daemon_status {
	int pid;
	char state;
	int ppid;
	int threads;
	unsigned long utime;
	unsigned long stime;
	char wchan[16];
	char comm[16];
};

static void daemon_probe_init(struct daemon_probe *p)
{
	unsigned i;

	memset(p, 0, sizeof *p);
	/* -1 is what marks "no scan yet"; 0 would look like a successful scan. */
	p->comm_errno = -1;
	for (i = 0; i < DAEMON_WATCH_COUNT; i++)
		strcpy(p->last[i], "-");
}

/* No strtoul: stdlib.h is only pulled in for the host self-checks. */
static unsigned long parse_ul(const char *s)
{
	unsigned long value = 0;

	while (*s >= '0' && *s <= '9')
		value = value * 10 + (unsigned long)(*s++ - '0');
	return value;
}

static int read_proc_file(int root, const char *pid_name, const char *leaf,
			  char *buf, size_t size)
{
	char path[96];
	ssize_t got;
	int fd;

	snprintf(path, sizeof path, "%s/%s", pid_name, leaf);
	fd = openat(root, path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	do {
		got = read(fd, buf, size - 1);
	} while (got < 0 && errno == EINTR);
	close(fd);
	if (got <= 0)
		return -1;
	buf[got] = '\0';
	return (int)got;
}

/* Field 2 (comm) may itself contain spaces and parentheses: split after the
 * LAST ')' so token 0 is field 3. A short or truncated stat leaves state '-'. */
static void daemon_parse_stat(const char *stat, struct daemon_status *out)
{
	unsigned long field[19];
	char state;
	const char *cursor = strrchr(stat, ')');
	size_t n;

	if (!cursor)
		return;
	for (cursor++; *cursor == ' '; cursor++)
		;
	if (!*cursor)
		return;
	state = *cursor;
	for (n = 0; n < sizeof field / sizeof field[0]; n++) {
		while (*cursor && *cursor != ' ')
			cursor++;
		while (*cursor == ' ')
			cursor++;
		if (!*cursor)
			break;
		field[n] = parse_ul(cursor);
	}
	if (n < sizeof field / sizeof field[0])
		return;
	out->state = state;
	out->ppid = (int)field[0];
	out->utime = field[10];
	out->stime = field[11];
	out->threads = (int)field[16];
}

static void daemon_probe_collect(const char *proc_root, struct daemon_status *status,
				 unsigned long *entries, int *comm_errno,
				 unsigned long *highest,
				 char *dump, size_t dump_size)
{
	DIR *dir = opendir(proc_root);
	struct dirent *entry;
	unsigned i;
	char found[DAEMON_WATCH_COUNT] = { 0 };

	*entries = 0;
	*comm_errno = 0;
	*highest = 0;
	if (dump && dump_size)
		dump[0] = '\0';

	memset(status, 0, DAEMON_WATCH_COUNT * sizeof *status);
	for (i = 0; i < DAEMON_WATCH_COUNT; i++) {
		status[i].state = '-';
		strcpy(status[i].wchan, "-");
		strcpy(status[i].comm, daemon_watch[i]);
	}
	if (!dir) {
		*comm_errno = errno;
		return;
	}
	while ((entry = readdir(dir))) {
		unsigned matched = DAEMON_WATCH_COUNT;
		size_t digits = strspn(entry->d_name, "0123456789");
		char comm[32], stat[1024], wchan[32];
		size_t n;

		if (!digits || entry->d_name[digits])
			continue;
		(*entries)++;
		{
			unsigned long pid = parse_ul(entry->d_name);

			if (pid > *highest)
				*highest = pid;
		}
		if (read_proc_file(dirfd(dir), entry->d_name, "comm", comm,
				   sizeof comm) <= 0) {
			/* errno is only meaningful here: on success it is stale. */
			if (!*comm_errno)
				*comm_errno = errno;
			continue;
		}
		/* comm carries a trailing newline and stops at 15 bytes. */
		comm[15] = '\0';
		for (n = strlen(comm); n && comm[n - 1] == '\n'; n--)
			comm[n - 1] = '\0';
		if (dump && dump_size) {
			char line[320];
			size_t at = strlen(dump);

			snprintf(line, sizeof line, " pid=%s comm=%s", entry->d_name, comm);
			if (at + strlen(line) + 1 < dump_size)
				strcpy(dump + at, line);
		}
		for (i = 0; i < DAEMON_WATCH_COUNT; i++) {
			if (found[i] ||
			    strncmp(daemon_watch[i], comm, strlen(daemon_watch[i])))
				continue;
			found[i] = 1;
			matched = i;
			break;
		}
		if (matched == DAEMON_WATCH_COUNT)
			continue;
		status[matched].pid = (int)parse_ul(entry->d_name);
		memcpy(status[matched].comm, comm, sizeof status[matched].comm);
		if (read_proc_file(dirfd(dir), entry->d_name, "stat", stat,
				   sizeof stat) > 0)
			daemon_parse_stat(stat, &status[matched]);
		/* wchan needs ptrace-mode access this domain may lack: diagnostic
		 * only, never fatal. */
		if (read_proc_file(dirfd(dir), entry->d_name, "wchan", wchan,
				   sizeof wchan) > 0) {
			wchan[15] = '\0';
			memcpy(status[matched].wchan, wchan,
			       sizeof status[matched].wchan);
		}
	}
	closedir(dir);
}

static void daemon_pidns_report(const struct daemon_probe *p, int out, int *mirror);

/* Written straight to the capture fd, never through /dev/kmsg: if printk is
 * what is wedged, the probe must survive it. */
static void daemon_probe_tick(struct daemon_probe *p, const char *proc_root,
			      int out, int *mirror)
{
	struct daemon_status status[DAEMON_WATCH_COUNT];
	char dump[8192];
	unsigned long entries, highest;
	unsigned i;
	int comm_errno, first = p && p->comm_errno == -1;

	if (!p)
		return;
	daemon_probe_collect(proc_root, status, &entries, &comm_errno,
			     &highest, first ? dump : NULL, sizeof dump);
	p->highest = highest;
	/* One census of every comm seen, so a watch name that can never match on
	 * this kernel is visible instead of silently reporting pid=0. */
	if (first && dump[0]) {
		char line[sizeof dump];
		size_t len;

		len = (size_t)snprintf(line, sizeof line, "DIAG: proccomm%s\n", dump);
		if (len < sizeof line)
			write_capture(out, mirror, line, len, 1);
	}
	/* An unreadable /proc must say so: silence here is indistinguishable
	 * from "no daemon yet" without it. */
	if (comm_errno != p->comm_errno || entries != p->entries) {
		char census[128];
		size_t len;

		len = (size_t)snprintf(census, sizeof census,
			"DIAG: procdir path=%s self=%d entries=%lu comm_errno=%d\n",
			proc_root, (int)getpid(), entries, comm_errno);
		p->comm_errno = comm_errno;
		p->entries = entries;
		if (len < sizeof census)
			write_capture(out, mirror, census, len, 1);
	}
	if (first)
		daemon_pidns_report(p, out, mirror);
	for (i = 0; i < DAEMON_WATCH_COUNT; i++) {
		char signature[48], line[256];
		time_t now = monotonic_seconds();
		size_t len;

		snprintf(signature, sizeof signature, "%d:%c:%d:%lu:%lu",
			 status[i].pid, status[i].state, status[i].threads,
			 status[i].utime, status[i].stime);
		/* ponytail: measured 20s heartbeat missed the whole 65.2-67.0s
		 * stall window; 1s costs ~600KB of the 64MiB logdump. */
		if (!strcmp(p->last[i], signature) && now - p->emitted[i] < 1)
			continue;
		len = (size_t)snprintf(line, sizeof line,
			"DIAG: daemon name=%s pid=%d state=%c ppid=%d threads=%d utime=%lu stime=%lu wchan=%s\n",
			status[i].comm, status[i].pid, status[i].state, status[i].ppid,
			status[i].threads, status[i].utime, status[i].stime, status[i].wchan);
		memcpy(p->last[i], signature, sizeof signature);
		p->emitted[i] = now;
		if (len < sizeof line)
			write_capture(out, mirror, line, len, 1);
	}
}

/* The recorder's /proc ceiling sat at ~168 while Android allocated pids past
 * 350. ns_last_pid separates "this view is restricted or frozen" from
 * "Android's pids live in another namespace"; the readlinks show whether PID 1
 * exposes one at all. Emitted once per run and again when the run ends. */
static void daemon_pidns_report(const struct daemon_probe *p, int out, int *mirror)
{
	char one[64], me[64], line[320];
	unsigned long next;
	ssize_t n;
	int ns_errno, fd;
	size_t len;

	n = readlink("/proc/1/ns/pid", one, sizeof one - 1);
	if (n < 0)
		snprintf(one, sizeof one, "errno=%d", errno);
	else
		one[n] = '\0';
	n = readlink("/proc/self/ns/pid", me, sizeof me - 1);
	if (n < 0)
		snprintf(me, sizeof me, "errno=%d", errno);
	else
		me[n] = '\0';
	fd = open("/proc/1/ns/pid", O_RDONLY | O_CLOEXEC);
	ns_errno = fd < 0 ? errno : 0;
	if (fd >= 0)
		close(fd);
	next = 0;
	{
		char buf[32];
		ssize_t got;
		int fd = open("/proc/sys/kernel/ns_last_pid",
			      O_RDONLY | O_CLOEXEC);

		if (fd >= 0) {
			got = read(fd, buf, sizeof buf - 1);
			close(fd);
			if (got > 0) {
				buf[got] = '\0';
				next = parse_ul(buf);
			}
		}
	}
	len = (size_t)snprintf(line, sizeof line,
		"DIAG: pidns entries=%lu highest=%lu ns_last_pid=%lu ns_pid_1=%s ns_self=%s ns_open_errno=%d\n",
		p->entries, p->highest, next, one, me, ns_errno);
	if (len < sizeof line)
		write_capture(out, mirror, line, len, 1);
}
#endif

static int dump_log(int in, int out, unsigned seconds, int durable, int *mirror,
		    struct ramoops_sampler *sampler)
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
		/* Only the durable writer probes: the USB child is a separate
		 * process with its own copy of the state. */
		if (sampler)
			daemon_probe_tick(&daemon_probe_state, "/proc", out, mirror);
#endif
		got = read(in, buf, sizeof buf);
		if (got > 0) {
			if (write_capture(out, mirror, buf, (size_t)got, durable))
				return -1;
		} else if (got < 0 && errno == EPIPE) {
			static const char lost[] =
				"\nDIAG: kmsg overrun; some records lost\n";
			if (write_capture(out, mirror, lost, sizeof lost - 1, durable))
				return -1;
		} else if (got < 0 && errno != EAGAIN && errno != EINTR) {
			return -1;
		} else {
			usleep(200000);
		}
	} while (monotonic_seconds() <= deadline);
#ifdef ANDROID_CAPTURE
	/* Last words: if Android's pids were invisible for the whole run, the
	 * final ns_last_pid says whether the view ever moved. */
	if (sampler)
		daemon_pidns_report(&daemon_probe_state, out, mirror);
#endif
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

	for (n = 0; n < POLL_TRIES; n++) {
		if (find_sd_partition(path, sizeof path) &&
		    !stat(path, &st) && S_ISBLK(st.st_mode))
			break;
		usleep(500000);
	}
	if (n == POLL_TRIES || make_dir("/sdlog") ||
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
	assert(!dump_log(kmsg, primary, 0, 1, NULL, &s));
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
#endif

#ifdef ANDROID_DAEMON_TEST
/* Catch prefix misses, the last-')' stat split, and a heartbeat that would
 * flood the capture with an unchanged tree. */
static void daemon_test_write(int dir, const char *path, const char *data)
{
	int fd = openat(dir, path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	assert(fd >= 0);
	assert(!write_all(fd, data, strlen(data)));
	close(fd);
}

static int count_lines(const char *text, const char *needle)
{
	int seen = 0;
	const char *at = text;

	while ((at = strstr(at, needle))) {
		seen++;
		at += strlen(needle);
	}
	return seen;
}

static int daemon_probe_selftest(void)
{
	char directory[] = "/tmp/ack-daemon-check-XXXXXX";
	char root[256];
	char captured[8192];
	/* comm carries a space and the field count is exactly 19 after ')'. */
	static const char keystore_stat[] =
		"100 (key store 2) S 1 100 100 0 -1 4194560 1234 5 0 0 20 3 0 0 20 0 4 0 987654 1234567 42\n";
	static const char keymaster_stat[] =
		"101 (keymaster@3.0-s) R 1 100 100 0 -1 4194560 1 0 0 0 7 1 0 0 20 0 2 0 111 222 7\n";
	static const char unrelated_stat[] =
		"102 (unrelated) D 1 100 100 0 -1 4194560 9 0 0 0 1 2 0 0 20 0 1 0 5 6 1\n";
	static const char truncated_stat[] = "103 (vdc) S 1 100\n";
	struct daemon_probe p;
	struct stat before, after;
	off_t size;
	int dir, out;
	ssize_t got;

	assert(mkdtemp(directory));
	dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dir >= 0);
	assert((size_t)snprintf(root, sizeof root, "%s/root", directory) < sizeof root);
	assert(!mkdirat(dir, "root", 0700));
	{
		int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		assert(root_fd >= 0);
		assert(!mkdirat(root_fd, "100", 0700));
		assert(!mkdirat(root_fd, "101", 0700));
		assert(!mkdirat(root_fd, "102", 0700));
		assert(!mkdirat(root_fd, "103", 0700));
		daemon_test_write(root_fd, "100/comm", "keystore2\n");
		daemon_test_write(root_fd, "100/stat", keystore_stat);
		daemon_test_write(root_fd, "100/wchan", "binder_ioctl_write_read\n");
		daemon_test_write(root_fd, "101/comm", "keymaster@3.0-s\n");
		daemon_test_write(root_fd, "101/stat", keymaster_stat);
		daemon_test_write(root_fd, "102/comm", "unrelated\n");
		daemon_test_write(root_fd, "102/stat", unrelated_stat);
		daemon_test_write(root_fd, "103/comm", "vdc\n");
		daemon_test_write(root_fd, "103/stat", truncated_stat);
		close(root_fd);
	}
	out = openat(dir, "capture", O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	assert(out >= 0);
	daemon_probe_init(&p);
	daemon_probe_tick(&p, root, out, NULL);
	assert(!fsync(out));
	assert(!fstat(out, &before));
	size = before.st_size;
	assert(size > 0);
	assert(pread(out, captured, (size_t)size, 0) == size);
	captured[size] = '\0';

	assert(strstr(captured, "DIAG: procdir path=") &&
	       strstr(captured, " entries=4 comm_errno=0\n"));
	/* The /proc ceiling question: highest pid in the fixture must reach the
	 * report, and every namespace field must be present even when the
	 * kernel refuses the read. */
	assert(strstr(captured, "DIAG: pidns entries=4 highest=103 ns_last_pid="));
	assert(strstr(captured, " ns_pid_1=") && strstr(captured, " ns_self=") &&
	       strstr(captured, " ns_open_errno="));
	/* The one-shot comm census must land, or a watch name that can never match
	 * on this kernel stays invisible. readdir order is not stable. */
	assert(strstr(captured, "DIAG: proccomm "));
	assert(strstr(captured, " pid=100 comm=keystore2") &&
	       strstr(captured, " pid=101 comm=keymaster@3.0-s") &&
	       strstr(captured, " pid=102 comm=unrelated") &&
	       strstr(captured, " pid=103 comm=vdc"));

	assert(count_lines(captured, "name=keystore2 ") == 1);
	assert(count_lines(captured, "name=keymaster@3.0-s ") == 1);
	/* The census legitimately names every process; only watch lines must skip it. */
	assert(!strstr(captured, "name=unrelated"));
	assert(strstr(captured,
		      "DIAG: daemon name=keystore2 pid=100 state=S ppid=1 threads=4 utime=20 stime=3 wchan=binder_ioctl_wr\n"));
	assert(strstr(captured,
		      "DIAG: daemon name=keymaster@3.0-s pid=101 state=R ppid=1 threads=2 utime=7 stime=1 wchan=-\n"));
	assert(strstr(captured, "DIAG: daemon name=vdc pid=103 state=- ppid=0 threads=0"));
	assert(strstr(captured, "DIAG: daemon name=logd pid=0 state=-"));

	/* Unchanged tree, inside the heartbeat: the probe must stay silent. */
	daemon_probe_tick(&p, root, out, NULL);
	assert(!fstat(out, &after));
	assert(after.st_size == size);

	/* Past the heartbeat the same tree must still emit: a cadence that never
	 * fires leaves a short stall window unobserved. */
	{
		unsigned slot;

		for (slot = 0; slot < DAEMON_WATCH_COUNT; slot++)
			p.emitted[slot] -= 2;
	}
	daemon_probe_tick(&p, root, out, NULL);
	assert(!fstat(out, &after));
	assert(after.st_size > size);

	/* An unreadable proc root must report the errno instead of going quiet. */
	{
		struct daemon_probe missing;
		char missing_root[sizeof root + 8];

		snprintf(missing_root, sizeof missing_root, "%s/absent", root);
		daemon_probe_init(&missing);
		daemon_probe_tick(&missing, missing_root, out, NULL);
		assert(!fsync(out));
		got = pread(out, captured, sizeof captured - 1, 0);
		assert(got > (ssize_t)size);
		captured[got] = '\0';
		assert(strstr(captured, " entries=0 comm_errno=2\n"));
	}

	close(out);
	assert(!unlinkat(dir, "capture", 0));
	{
		int root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		unsigned pid;

		assert(root_fd >= 0);
		for (pid = 100; pid < 104; pid++) {
			char leaf[16];

			snprintf(leaf, sizeof leaf, "%u/comm", pid);
			assert(!unlinkat(root_fd, leaf, 0));
			snprintf(leaf, sizeof leaf, "%u/stat", pid);
			assert(!unlinkat(root_fd, leaf, 0));
			if (pid == 100)
				assert(!unlinkat(root_fd, "100/wchan", 0));
			snprintf(leaf, sizeof leaf, "%u", pid);
			assert(!unlinkat(root_fd, leaf, AT_REMOVEDIR));
		}
		close(root_fd);
	}
	assert(!rmdir(root));
	close(dir);
	assert(!rmdir(directory));
	puts("Daemon probe prefix, stat split, dedup and heartbeat checks PASS");
	return 0;
}
#endif

#define GUARDED_FSTAB "/dev/block/ack-protected-fstab.qcom"
#define GUARDED_CONTEXTS "/dev/block/ack-vendor-file-contexts"
#define BOOT_CONTROLLER "/dev/block/platform/soc@0/7824900.mmc"

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

/* Report what /proc looks like from the namespace this process is in. Runs in
 * a short-lived child so the recorder keeps its single-writer invariant. */
static void guardns_census(int log, const char *vantage)
{
	struct daemon_status seen[DAEMON_WATCH_COUNT];
	char done[80];
	time_t stop = monotonic_seconds() + 150;

	while (monotonic_seconds() < stop) {
		unsigned long entries, highest;
		char line[320];
		size_t len;
		int err;
		unsigned i;

		daemon_probe_collect("/proc", seen, &entries, &err, &highest,
				     NULL, 0);
		len = (size_t)snprintf(line, sizeof line,
			"DIAG: guardns vantage=%s entries=%lu comm_errno=%d",
			vantage, entries, err);
		for (i = 0; i < DAEMON_WATCH_COUNT && len < sizeof line; i++)
			len += (size_t)snprintf(line + len, sizeof line - len,
				" %s=%d/%c", seen[i].comm, seen[i].pid, seen[i].state);
		if (len < sizeof line)
			snprintf(line + len, sizeof line - len, "\n");
		say(log, line);
		usleep(1000000);
	}
	snprintf(done, sizeof done,
		 "DIAG: guardns vantage=%s census complete\n", vantage);
	say(log, done);
}

static void install_android_guard(int proc, int fstab, int contexts, int log)
{
	time_t deadline = monotonic_seconds() + 60;
	int ns = -1, root = -1;
	struct stat st;
	char message[192];
	const char *step = "validate protected fstab";

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
	step = "release canonical bootdevice and legacy wait";
	if (guard_bootdevice(proc))
		goto failed;
	say(log, "DIAG: bootdevice guard installed read-only f2fs-only fstab"
		 " and vendor contexts before policy load\n");
	say(log, "DIAG: bootdevice canonical alias and legacy wait path ready\n");
	/* The mount namespace does not cross the boundary: Android's services are
	 * in a different PID namespace, visible only after setns(CLONE_NEWPID)
	 * followed by a fork. Reap the child and keep the record either way. */
	{
		int pidns = openat(proc, "1/ns/pid", O_RDONLY | O_CLOEXEC);

		if (pidns >= 0 && !setns(pidns, CLONE_NEWPID) && fork() == 0) {
			close(pidns);
			close(proc);
			guardns_census(log, "pidns");
			_exit(0);
		}
		snprintf(message, sizeof message,
			 "DIAG: guardns pid namespace join failed errno=%d; "
			 "mount namespace census follows\n", errno);
		say(log, message);
		if (pidns >= 0)
			close(pidns);
	}
	/* Fallback vantage point: PID 1's mount namespace. */
	guardns_census(log, "mntns");
	_exit(0);
failed:
	snprintf(message, sizeof message,
		 "DIAG: bootdevice guard step=%s failed errno=%d; alias not released\n",
		 step, errno);
	say(log, message);
	_exit(1);
}

#ifdef ANDROID_GUARD_TEST
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
	char temporary[] = "/tmp/ack-fstab-check-XXXXXX";
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
	puts("Guard fstab safety checks PASS");
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
	} else if (dump_log(kin, out, 900, 0, NULL, NULL)) {
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
		int proc = -1, fstab = -1, contexts = -1, sd = -1;
		struct ramoops_sampler sampler = {
			.input = -1, .output = { -1, -1 }, .log = -1,
		};
		static const char marker[] =
			"DIAG: Android capture ready; handing PID 1 to Android init\n";

		/* The probe owns nothing: never closed, never freed. */
		daemon_probe_init(&daemon_probe_state);
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
		    write_capture(out, &sd, marker, sizeof marker - 1, 1))
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
					      sizeof("DIAG: Android init exec succeeded\n") - 1, 1);
			else {
				snprintf(message, sizeof message,
					 "DIAG: Android init exec failed or handshake lost: errno=%d\n",
					 error);
				write_capture(out, &sd, message, strlen(message), 1);
			}
			fsync(out);
		}
		close(ready[1]);
		if (dump_log(in, out, 900, 1, &sd, &sampler)) {
			static const char failed[] = "\nDIAG: Android capture failed\n";
			write_capture(out, &sd, failed, sizeof failed - 1, 1);
		} else {
			static const char eof[] = "\nDIAG: EOF Android kmsg dump complete\n";
			write_capture(out, &sd, eof, sizeof eof - 1, 1);
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

	(void)argc;
	(void)argv;
#ifdef ANDROID_CAPTURE
#ifdef ANDROID_GUARD_TEST
	return guard_selftest();
#endif
#ifdef ANDROID_RAMOOPS_TEST
	return ramoops_selftest();
#endif
#ifdef ANDROID_DAEMON_TEST
	return daemon_probe_selftest();
#endif
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
	if (dump_log(in, out, DUMP_SECONDS, DURABLE_CAPTURE && !usb, NULL, NULL)) {
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