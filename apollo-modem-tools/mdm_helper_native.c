// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal native replacement for the vendor mdm_helper on ESOC-based modems.
 *
 * This tool intentionally targets the current kernel behavior on Apollo:
 * the in-kernel debug request engine owns the ESOC request-engine slot and
 * exposes requests through debugfs. We respond via debugfs and use the ESOC
 * char device only for status polling.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <dirent.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * Keep the userspace copy small and self-contained instead of depending on
 * exported kernel UAPI headers during host builds.
 */
#define ESOC_CODE               0xCC
#define ESOC_GET_STATUS         _IOR(ESOC_CODE, 4, unsigned int)
#define ESOC_GET_ERR_FATAL      _IOR(ESOC_CODE, 5, unsigned int)

enum esoc_notify {
	ESOC_IMG_XFER_DONE = 1,
	ESOC_BOOT_DONE,
	ESOC_BOOT_FAIL,
	ESOC_IMG_XFER_RETRY,
	ESOC_IMG_XFER_FAIL,
	ESOC_UPGRADE_AVAILABLE,
	ESOC_DEBUG_DONE,
	ESOC_DEBUG_FAIL,
	ESOC_PRIMARY_CRASH,
	ESOC_PRIMARY_REBOOT,
	ESOC_PON_RETRY,
};

struct config {
	const char *debugfs_dir;
	const char *device_path;
	unsigned int poll_ms;
	unsigned int boot_done_timeout_ms;
	unsigned int startup_timeout_ms;
	bool status_polling;
	bool send_boot_done;
	bool send_debug_done;
	bool verbose;
};

struct state {
	bool img_xfer_done_sent;
	bool boot_done_sent;
	bool shutdown_requested;
	bool status_valid;
	bool errfatal_valid;
	int esoc_fd;
	unsigned int last_status;
	unsigned int last_errfatal;
	char last_req[64];
};

static bool file_exists(const char *path);

static int discover_esoc_device(char *path, size_t path_size)
{
	DIR *dir;
	struct dirent *entry;
	int matches = 0;
	char candidate[PATH_MAX];

	dir = opendir("/dev");
	if (!dir)
		return -errno;

	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "subsys_", 7) != 0)
			continue;

		snprintf(candidate, sizeof(candidate), "/dev/%s", entry->d_name);
		if (!file_exists(candidate))
			continue;

		matches++;
		snprintf(path, path_size, "%s", candidate);
	}

	closedir(dir);

	if (matches == 1)
		return 0;
	if (matches == 0)
		return -ENOENT;
	return -E2BIG;
}

static const struct option long_options[] = {
	{ "debugfs-dir", required_argument, NULL, 'd' },
	{ "device", required_argument, NULL, 'D' },
	{ "poll-ms", required_argument, NULL, 'p' },
	{ "status-polling", no_argument, NULL, 'P' },
	{ "boot-done", no_argument, NULL, 'b' },
	{ "debug-done", no_argument, NULL, 'g' },
	{ "boot-done-timeout-ms", required_argument, NULL, 't' },
	{ "startup-timeout-ms", required_argument, NULL, 's' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "help", no_argument, NULL, 'h' },
	{ NULL, 0, NULL, 0 },
};

static void usage(FILE *stream, const char *progname)
{
	fprintf(stream,
		"Usage: %s [options]\n"
		"\n"
		"Watch ESOC debugfs requests and answer them in native Linux userspace.\n"
		"\n"
		"Options:\n"
		"  -d, --debugfs-dir PATH           ESOC debugfs directory\n"
		"                                   default: /sys/kernel/debug/esoc_mdm_dbg_eng/esoc0\n"
		"  -D, --device PATH                ESOC device for status polling\n"
		"                                   default: /dev/subsys_mdm\n"
		"  -p, --poll-ms N                  Poll interval in milliseconds\n"
		"                                   default: 200\n"
		"  -P, --status-polling             Poll ESOC status and errfatal state\n"
		"  -b, --boot-done                  Send BOOT_DONE once modem status goes high\n"
		"  -g, --debug-done                 Send DEBUG_DONE instead of DEBUG_FAIL\n"
		"  -t, --boot-done-timeout-ms N     Timeout waiting for modem status high\n"
		"                                   default: 30000\n"
		"  -s, --startup-timeout-ms N       Timeout waiting for ESOC debugfs files\n"
		"                                   default: 30000, 0 means wait forever\n"
		"  -v, --verbose                    Log idle state changes too\n"
		"  -h, --help                       Show this help\n",
		progname);
}

static long monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return -1;

	return (ts.tv_sec * 1000L) + (ts.tv_nsec / 1000000L);
}

static bool file_exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static int read_trimmed_file(const char *path, char *buf, size_t buf_size)
{
	FILE *fp;
	size_t len;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;

	if (!fgets(buf, buf_size, fp)) {
		int io_error = ferror(fp);

		fclose(fp);
		return io_error ? -errno : -EIO;
	}
	fclose(fp);

	len = strcspn(buf, "\r\n");
	buf[len] = '\0';
	return 0;
}

static int write_string_file(const char *path, const char *value)
{
	FILE *fp;

	fp = fopen(path, "w");
	if (!fp)
		return -errno;

	if (fputs(value, fp) == EOF) {
		int saved_errno = errno;

		fclose(fp);
		return -saved_errno;
	}

	if (fclose(fp) != 0)
		return -errno;

	return 0;
}

static int open_esoc_device(const char *device_path)
{
	int fd;

	fd = open(device_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	return fd;
}

static int read_esoc_status_fd(int fd, unsigned int ioctl_cmd, unsigned int *value)
{
	if (ioctl(fd, ioctl_cmd, value) < 0)
		return -errno;
	return 0;
}

static void log_notify(const char *name)
{
	fprintf(stderr, "notify %s\n", name);
}

static void log_status_value(const char *name, unsigned int value)
{
	fprintf(stderr, "%s=%u\n", name, value);
}

static int send_notify(const char *resp_path, enum esoc_notify notify)
{
	const char *value;

	switch (notify) {
	case ESOC_IMG_XFER_DONE:
		value = "XFER_DONE\n";
		break;
	case ESOC_BOOT_DONE:
		value = "BOOT_DONE\n";
		break;
	case ESOC_BOOT_FAIL:
		value = "BOOT_FAIL\n";
		break;
	case ESOC_IMG_XFER_RETRY:
		value = "XFER_RETRY\n";
		break;
	case ESOC_IMG_XFER_FAIL:
		value = "XFER_FAIL\n";
		break;
	case ESOC_DEBUG_DONE:
		value = "DEBUG_DONE\n";
		break;
	case ESOC_DEBUG_FAIL:
		value = "DEBUG_FAIL\n";
		break;
	default:
		return -EINVAL;
	}

	return write_string_file(resp_path, value);
}

/*
 * Start of a new boot cycle: forget what was sent for the previous one.
 *
 * last_esoc_req is a level, not a queue -- it keeps returning the last
 * request on every poll -- so the one-shot latches are what stop XFER_DONE
 * from being resent once a second. That makes clearing them at the end of a
 * cycle mandatory: otherwise the next REQ_IMG is silently ignored, BOOT_DONE
 * is never sent, and mdm_rproc_powerup() waits on pon_done forever holding
 * the remoteproc lock, which no signal can break.
 */
static void rearm_boot_cycle(struct state *st)
{
	st->img_xfer_done_sent = false;
	st->boot_done_sent = false;
}

static int poll_status(const struct config *cfg, struct state *st)
{
	unsigned int status = 0;
	unsigned int errfatal = 0;
	int rc;

	if (!cfg->status_polling || st->esoc_fd < 0)
		return 0;

	rc = read_esoc_status_fd(st->esoc_fd, ESOC_GET_STATUS, &status);
	if (rc != 0)
		return rc;

	rc = read_esoc_status_fd(st->esoc_fd, ESOC_GET_ERR_FATAL, &errfatal);
	if (rc != 0)
		return rc;

	if (st->status_valid && st->last_status != 0 && status == 0) {
		fprintf(stderr, "status dropped to 0; re-arming for next boot\n");
		rearm_boot_cycle(st);
	}

	if (!st->status_valid || st->last_status != status || cfg->verbose) {
		log_status_value("status", status);
		st->last_status = status;
		st->status_valid = true;
	}

	if (!st->errfatal_valid || st->last_errfatal != errfatal || cfg->verbose) {
		log_status_value("errfatal", errfatal);
		st->last_errfatal = errfatal;
		st->errfatal_valid = true;
	}

	return 0;
}

static int maybe_send_boot_done(const struct config *cfg, const char *resp_path, struct state *st)
{
	unsigned int status = 0;
	int rc;

	if (!cfg->send_boot_done || st->boot_done_sent || !st->img_xfer_done_sent || st->esoc_fd < 0)
		return 0;

	rc = read_esoc_status_fd(st->esoc_fd, ESOC_GET_STATUS, &status);
	if (rc != 0)
		return rc;

	if (status != 1)
		return 0;

	rc = send_notify(resp_path, ESOC_BOOT_DONE);
	if (rc != 0)
		return rc;

	log_notify("BOOT_DONE");
	st->boot_done_sent = true;
	return 0;
}

static int handle_request(const struct config *cfg, const char *resp_path, const char *request,
			  struct state *st, long *img_xfer_ms)
{
	int rc;

	if (strcmp(request, "REQ_IMG") == 0) {
		if (!st->img_xfer_done_sent) {
			rc = send_notify(resp_path, ESOC_IMG_XFER_DONE);
			if (rc != 0)
				return rc;

			log_notify("XFER_DONE");
			st->img_xfer_done_sent = true;
			st->boot_done_sent = false;
			*img_xfer_ms = monotonic_ms();
		}
		return 0;
	}

	if (strcmp(request, "REQ_DEBUG") == 0) {
		rc = send_notify(resp_path,
				 cfg->send_debug_done ? ESOC_DEBUG_DONE : ESOC_DEBUG_FAIL);
		if (rc != 0)
			return rc;

		log_notify(cfg->send_debug_done ? "DEBUG_DONE" : "DEBUG_FAIL");
		return 0;
	}

	if (strcmp(request, "REQ_SHUTDOWN") == 0 ||
	    strcmp(request, "REQ_SEND_SHUTDOWN") == 0 ||
	    strcmp(request, "REQ_CRASH_SHUTDOWN") == 0) {
		fprintf(stderr, "request %s seen; marking shutdown requested\n", request);
		st->shutdown_requested = true;
		rearm_boot_cycle(st);
		return 0;
	}

	if (cfg->verbose)
		fprintf(stderr, "request %s seen; no automatic response\n", request);

	return 0;
}

static int wait_for_startup_files(const struct config *cfg, const char *req_path, const char *resp_path)
{
	long start_ms;
	long now_ms;

	start_ms = monotonic_ms();
	for (;;) {
		if (file_exists(req_path) && file_exists(resp_path))
			return 0;

		now_ms = monotonic_ms();
		if (cfg->startup_timeout_ms != 0 &&
		    start_ms >= 0 &&
		    now_ms >= 0 &&
		    (unsigned long)(now_ms - start_ms) > cfg->startup_timeout_ms)
			return -ETIMEDOUT;

		poll(NULL, 0, (int)cfg->poll_ms);
	}
}

int main(int argc, char **argv)
{
	struct config cfg = {
		.debugfs_dir = "/sys/kernel/debug/esoc_mdm_dbg_eng/esoc0",
		.device_path = "/dev/subsys_mdm",
		.poll_ms = 200,
		.boot_done_timeout_ms = 30000,
		.startup_timeout_ms = 30000,
		.status_polling = false,
		.send_boot_done = false,
		.send_debug_done = false,
		.verbose = false,
	};
	struct state st = {
		.esoc_fd = -1,
	};
	char req_path[PATH_MAX];
	char resp_path[PATH_MAX];
	char request[64];
	long img_xfer_ms = -1;
	int opt;

	while ((opt = getopt_long(argc, argv, "d:D:p:t:s:Pbgvh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			cfg.debugfs_dir = optarg;
			break;
		case 'D':
			cfg.device_path = optarg;
			break;
		case 'p':
			cfg.poll_ms = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'P':
			cfg.status_polling = true;
			break;
		case 't':
			cfg.boot_done_timeout_ms = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 's':
			cfg.startup_timeout_ms = (unsigned int)strtoul(optarg, NULL, 10);
			break;
		case 'b':
			cfg.send_boot_done = true;
			cfg.status_polling = true;
			break;
		case 'g':
			cfg.send_debug_done = true;
			break;
		case 'v':
			cfg.verbose = true;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}

	snprintf(req_path, sizeof(req_path), "%s/last_esoc_req", cfg.debugfs_dir);
	snprintf(resp_path, sizeof(resp_path), "%s/req_eng_resp", cfg.debugfs_dir);

	if (wait_for_startup_files(&cfg, req_path, resp_path) != 0) {
		fprintf(stderr, "timed out waiting for ESOC debugfs files under %s\n", cfg.debugfs_dir);
		return 1;
	}

	if (!file_exists(cfg.device_path)) {
		static char discovered_path[PATH_MAX];
		int rc;

		rc = discover_esoc_device(discovered_path, sizeof(discovered_path));
		if (rc == 0) {
			cfg.device_path = discovered_path;
		} else if (cfg.status_polling) {
			fprintf(stderr, "device %s not found for status polling\n", cfg.device_path);
			return 1;
		}
	}

	fprintf(stderr, "watching %s\n", cfg.debugfs_dir);
	if (cfg.status_polling) {
		st.esoc_fd = open_esoc_device(cfg.device_path);
		if (st.esoc_fd < 0) {
			fprintf(stderr, "failed to open %s: %s\n",
				cfg.device_path, strerror(-st.esoc_fd));
			return 1;
		}
		fprintf(stderr, "status polling enabled on %s\n", cfg.device_path);
	}

	for (;;) {
		int rc;
		long now_ms;

		memset(request, 0, sizeof(request));
		rc = read_trimmed_file(req_path, request, sizeof(request));
		if (rc != 0) {
			fprintf(stderr, "read %s failed: %s\n", req_path, strerror(-rc));
			return 1;
		}

		if (request[0] != '\0' && strcmp(request, st.last_req) != 0) {
			fprintf(stderr, "request %s\n", request);
			snprintf(st.last_req, sizeof(st.last_req), "%s", request);
			rc = handle_request(&cfg, resp_path, request, &st, &img_xfer_ms);
			if (rc != 0) {
				fprintf(stderr, "handling %s failed: %s\n",
					request, strerror(-rc));
				return 1;
			}
		}

		rc = poll_status(&cfg, &st);
		if (rc != 0) {
			fprintf(stderr, "status polling failed: %s\n", strerror(-rc));
			return 1;
		}

		rc = maybe_send_boot_done(&cfg, resp_path, &st);
		if (rc != 0) {
			fprintf(stderr, "status polling failed: %s\n", strerror(-rc));
			return 1;
		}

		if (cfg.send_boot_done && st.img_xfer_done_sent && !st.boot_done_sent) {
			now_ms = monotonic_ms();
			if (img_xfer_ms >= 0 &&
			    now_ms >= 0 &&
			    (unsigned long)(now_ms - img_xfer_ms) > cfg.boot_done_timeout_ms) {
				fprintf(stderr, "timeout waiting for modem status high after XFER_DONE\n");
				return 1;
			}
		}

		poll(NULL, 0, (int)cfg.poll_ms);
	}
}
