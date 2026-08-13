// SPDX-License-Identifier: GPL-2.0
/*
 * Read the ADSP's F3 debug log.
 *
 * The modem's F3 arrives over its MHI DIAG port and diag_reader decodes it. The
 * ADSP's does not: it speaks the same DIAG protocol, but the transport is IPC
 * router (QRTR), not a character device and not raw glink. Its firmware carries
 * diagcomm_io_socket.c and logs "diagcomm_io_socket_open: Opening Socket -
 * channel_type=%d, io_type=%d, port_num=%d", which is what gave it away; an
 * earlier attempt to open glink channels named DIAG_DATA/DIAG_CTRL/DIAG_CMD had
 * every open time out after glink's 5 s, because nothing on the ADSP serves
 * those names.
 *
 * The addressing comes from the vendor kernel, drivers/char/diag/
 * diagfwd_socket.c:
 *
 *	service        0x1001
 *	LPASS base     64          (MODEM 0, WCNSS 128, SENSORS 192, CDSP 256)
 *	offsets        CNTL 0, CMD 1, DATA 2, DCI_CMD 3, DCI 4
 *
 * so the ADSP's three channels are instances 64, 65 and 66. The direction is
 * the part worth knowing: **the AP is the server** for CNTL and DATA -- it
 * publishes them and the ADSP sends into them -- while the ADSP is the server
 * for CMD, which the AP looks up and sends commands to. Publishing nothing is
 * why the ADSP has never said a word to us; it has had nowhere to say it.
 *
 * Why bother: the ADSP is the only agent in the call-audio path whose reasoning
 * has never been observed. The AP's conversation is fully traced and every
 * command succeeds through START_VOICE; the modem's own state machine is
 * visible over DIAG and stalls asking the ADSP to map memory. The ADSP's
 * decision not to answer that is logged in its F3, and this is how to read it.
 *
 * Needs no kernel change -- QRTR is already up (the ADSP is node 2).
 *
 *	sudo adsp_diag --raw
 */
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/qrtr.h>

#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL		0xfffffffeu
#endif
#ifndef QRTR_TYPE_NEW_SERVER
#define QRTR_TYPE_NEW_SERVER	4
#endif
#ifndef QRTR_TYPE_DEL_SERVER
#define QRTR_TYPE_DEL_SERVER	5
#endif
#ifndef QRTR_TYPE_NEW_LOOKUP
#define QRTR_TYPE_NEW_LOOKUP	10
#endif

#define DIAG_SVC_ID		0x1001
#define LPASS_INST_BASE		64
#define INST_ID_CNTL		0
#define INST_ID_CMD		1
#define INST_ID_DATA		2

/* DIAG command codes, the same protocol the modem speaks. */
#define DIAG_EXT_MSG_CONFIG_F		0x7D
#define DIAG_EXT_MSG_F			0x79
#define DIAG_LOG_F			0x10
#define DIAG_QSR4_EXT_MSG_F		0x61
#define DIAG_BAD_CMD_F			0x13
#define DIAG_BAD_PARM_F			0x14

#define MSG_EXT_CFG_GET_SSID_RANGES	1
#define MSG_EXT_CFG_SET_ALL_RT_MASKS	5

/*
 * The control channel carries its own little protocol, {u32 id, u32 len, body}.
 * The ADSP opens with FEATURE, announcing what it supports, and waits to be
 * answered in kind -- a peripheral that never hears the host's feature mask
 * does not consider diag up and streams nothing.
 */
#define DIAG_CTRL_MSG_FEATURE		8
#define DIAG_CTRL_MSG_F3_MASK_V2	11
#define DIAG_CTRL_MSG_DIAGMODE		3
#define DIAG_CTRL_MSG_EQUIP_LOG_MASK	9
#define DIAG_CTRL_MSG_EVENT_MASK_V2	10
#define DIAG_CTRL_MSG_TX_MODE		17
#define DIAG_CTRL_MSG_DIAGID		33
#define DIAG_CTRL_MSG_SSID_RANGE_REPORT	24
#define DIAG_CTRL_MASK_ALL_ENABLED	2
#define MSG_MASK_CTRL_HEADER_LEN	11
#define DIAG_BUFFERING_MODE_STREAMING	0

/*
 * Bit 11 of the peripheral's feature mask. When it is set -- and this ADSP
 * announces 73 ae 0b, so it is -- the peripheral ignores the classic 0x7D
 * SET_ALL_RT_MASKS command and takes its masks only from the control channel.
 * That is why the command that works on the modem does nothing here.
 */
#define F_DIAG_MASK_CENTRALIZATION	11

#define MAX_PKT			0x4400

static volatile sig_atomic_t stop;
static int raw;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double mono_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void dlog(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "[%9.3f] ", mono_sec());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

/* Bind an auto-assigned port and announce it as @service/@instance. */
static int serve(uint32_t service, uint32_t instance, const char *what)
{
	struct sockaddr_qrtr sq;
	struct qrtr_ctrl_pkt pkt;
	socklen_t sl = sizeof(sq);
	int fd;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		dlog("%s: socket: %s\n", what, strerror(errno));
		return -1;
	}

	memset(&sq, 0, sizeof(sq));
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = 1;
	sq.sq_port = 0;
	if (bind(fd, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
		dlog("%s: bind: %s\n", what, strerror(errno));
		close(fd);
		return -1;
	}
	if (getsockname(fd, (struct sockaddr *)&sq, &sl) < 0) {
		dlog("%s: getsockname: %s\n", what, strerror(errno));
		close(fd);
		return -1;
	}

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_SERVER;
	pkt.server.service = service;
	pkt.server.instance = instance;
	pkt.server.node = sq.sq_node;
	pkt.server.port = sq.sq_port;

	sq.sq_port = QRTR_PORT_CTRL;
	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq,
		   sizeof(sq)) < 0) {
		dlog("%s: NEW_SERVER(%u/%u): %s\n", what, service, instance,
		     strerror(errno));
		close(fd);
		return -1;
	}

	dlog("%s: serving %#x/%u\n", what, service, instance);
	return fd;
}

static void unpublish(int fd, uint32_t service, uint32_t instance)
{
	struct sockaddr_qrtr sq = {0};
	struct qrtr_ctrl_pkt pkt;
	socklen_t sl = sizeof(sq);

	if (fd < 0 || getsockname(fd, (struct sockaddr *)&sq, &sl) < 0)
		return;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_DEL_SERVER;
	pkt.server.service = service;
	pkt.server.instance = instance;
	pkt.server.node = sq.sq_node;
	pkt.server.port = sq.sq_port;

	sq.sq_port = QRTR_PORT_CTRL;
	sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq, sizeof(sq));
}

/*
 * Ask the router to tell us about every server of @service. The ADSP's CMD
 * instance is one of them, and its address is the only way to send it anything.
 */
static int start_lookup(uint32_t service)
{
	struct sockaddr_qrtr sq;
	struct qrtr_ctrl_pkt pkt;
	int fd;

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	memset(&sq, 0, sizeof(sq));
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = 1;
	sq.sq_port = 0;
	if (bind(fd, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
		close(fd);
		return -1;
	}

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = service;
	pkt.server.instance = 0;

	sq.sq_port = QRTR_PORT_CTRL;
	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq,
		   sizeof(sq)) < 0) {
		dlog("NEW_LOOKUP(%#x): %s\n", service, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

static void hexdump(const uint8_t *p, size_t n)
{
	size_t i;

	for (i = 0; i < n && i < 96; i++)
		fprintf(stderr, "%02x ", p[i]);
	if (n > 96)
		fprintf(stderr, "... (%zu bytes)", n);
	fprintf(stderr, "\n");
}

/*
 * Print the readable part of a packet.
 *
 * F3 messages carry their format string and source filename as plain text, so
 * the content is recoverable without the QSR database just by printing the
 * printable runs -- the same reason the modem's log turned out to be readable.
 */
static void show_strings(const uint8_t *p, size_t n)
{
	size_t i, run = 0;

	for (i = 0; i < n; i++) {
		if (isprint(p[i]) || p[i] == '\t') {
			run++;
			continue;
		}
		if (run >= 5)
			fprintf(stderr, "  str: \"%.*s\"\n", (int)run,
				(const char *)p + i - run);
		run = 0;
	}
	if (run >= 5)
		fprintf(stderr, "  str: \"%.*s\"\n", (int)run,
			(const char *)p + n - run);
	fflush(stderr);
}

/*
 * Answer the ADSP's FEATURE announcement with our own, echoing the mask length
 * it used. Mirroring rather than inventing a mask keeps us from claiming a
 * capability we do not implement, which would change how it frames what it
 * sends us.
 */
static void reply_feature(int fd, const struct sockaddr_qrtr *to,
			  const uint8_t *mask, uint32_t masklen)
{
	uint8_t pkt[64];
	uint32_t v;
	size_t off = 0;

	if (masklen > sizeof(pkt) - 12)
		return;

	v = DIAG_CTRL_MSG_FEATURE;	memcpy(pkt + off, &v, 4); off += 4;
	v = 4 + masklen;		memcpy(pkt + off, &v, 4); off += 4;
	v = masklen;			memcpy(pkt + off, &v, 4); off += 4;
	memcpy(pkt + off, mask, masklen); off += masklen;

	if (sendto(fd, pkt, off, 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("feature reply: %s\n", strerror(errno));
	else
		dlog("answered FEATURE (%u mask bytes)\n", masklen);
}

/*
 * Turn on every F3 message in one SSID range.
 *
 * ALL_ENABLED still carries a mask word -- the vendor sets msg_mask_size to 1
 * and appends four bytes -- and the range has to be a real one the peripheral
 * knows about, so this is sent once per range out of its own SSID_RANGE_REPORT
 * rather than once with a zero range. A zero-length, zero-range version is
 * accepted silently and enables nothing, which is what made the first few
 * attempts look like the mask was being ignored outright.
 */
static void send_f3_mask_range(int fd, const struct sockaddr_qrtr *to,
			       uint16_t first, uint16_t last)
{
	struct {
		uint32_t cmd_type;
		uint32_t data_len;
		uint8_t stream_id;
		uint8_t status;
		uint8_t msg_mode;
		uint16_t ssid_first;
		uint16_t ssid_last;
		uint32_t msg_mask_size;
		uint32_t mask;
	} __attribute__((packed)) m = {
		.cmd_type = DIAG_CTRL_MSG_F3_MASK_V2,
		.data_len = MSG_MASK_CTRL_HEADER_LEN + sizeof(uint32_t),
		.stream_id = 1,
		.status = DIAG_CTRL_MASK_ALL_ENABLED,
		.msg_mode = 0,
		.ssid_first = first,
		.ssid_last = last,
		.msg_mask_size = 1,
		.mask = 0xffffffff,
	};

	if (sendto(fd, &m, sizeof(m), 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("F3 mask %u-%u: %s\n", first, last, strerror(errno));
}

/*
 * The peripheral lists the SSID ranges it actually implements; enable each.
 */
static void send_f3_masks(int fd, const struct sockaddr_qrtr *to,
			  const uint8_t *buf, size_t n)
{
	uint32_t count, i;
	size_t off = 16;

	if (n < 16)
		return;
	memcpy(&count, buf + 12, 4);
	if (count > 256 || off + count * 4 > n)
		return;

	for (i = 0; i < count; i++) {
		uint16_t first, last;

		memcpy(&first, buf + off, 2);
		memcpy(&last, buf + off + 2, 2);
		off += 4;
		send_f3_mask_range(fd, to, first, last);
	}

	dlog("F3 masks enabled for %u SSID range(s)\n", count);
}

/*
 * Ask for messages as they happen rather than in batches.
 *
 * The ADSP advertises F_DIAG_PERIPHERAL_BUFFERING, and a peripheral left in a
 * buffering mode holds its log until something drains it -- which looks exactly
 * like a peripheral that emits nothing.
 */
static void send_tx_streaming(int fd, const struct sockaddr_qrtr *to)
{
	struct {
		uint32_t pkt_id;
		uint32_t len;
		uint32_t version;
		uint8_t stream_id;
		uint8_t tx_mode;
	} __attribute__((packed)) m = {
		.pkt_id = DIAG_CTRL_MSG_TX_MODE,
		.len = sizeof(m) - 8,
		.version = 1,
		.stream_id = 1,
		.tx_mode = DIAG_BUFFERING_MODE_STREAMING,
	};

	if (sendto(fd, &m, sizeof(m), 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("tx mode: %s\n", strerror(errno));
	else
		dlog("TX_MODE(STREAMING) sent to %u:%u\n", to->sq_node,
		     to->sq_port);
}

static const char *ctrl_name(uint32_t id)
{
	switch (id) {
	case 3:  return "DIAGMODE";
	case 4:  return "DIAGDATA";
	case DIAG_CTRL_MSG_FEATURE: return "FEATURE";
	case 9:  return "EQUIP_LOG_MASK";
	case 10: return "EVENT_MASK_V2";
	case DIAG_CTRL_MSG_F3_MASK_V2: return "F3_MASK_V2";
	case 12: return "NUM_PRESETS";
	case DIAG_CTRL_MSG_TX_MODE: return "TX_MODE";
	case 22: return "LAST_EVENT_REPORT";
	case 23: return "LOG_RANGE_REPORT";
	case 24: return "SSID_RANGE_REPORT";
	case 25: return "BUILD_MASK_REPORT";
	case 30: return "PD_STATUS";
	case DIAG_CTRL_MSG_DIAGID: return "DIAGID";
	default: return NULL;
	}
}

/*
 * Assign the ADSP a diag_id for one of its processes and tell it.
 *
 * The peripheral sends a DIAGID record per PD -- here msm/adsp/root_pd and
 * msm/adsp/audio_pd, the second being the one that runs the voice code -- with
 * the id field left as a placeholder for the host to fill in. Until that reply
 * arrives the peripheral drops every mask it is given, which is why enabling
 * all F3 messages had no effect: the vendor driver spells this out, sending
 * masks "only if ... diag_id has been sent to peripheral".
 *
 * The reply is always version 1 regardless of the version it announced with.
 */
static void reply_diagid(int fd, const struct sockaddr_qrtr *to,
			 uint32_t diag_id, const char *name)
{
	uint8_t pkt[64];
	size_t nlen = strlen(name) + 1;
	uint32_t v;
	size_t off = 0;

	if (nlen > sizeof(pkt) - 16)
		return;

	v = DIAG_CTRL_MSG_DIAGID;	memcpy(pkt + off, &v, 4); off += 4;
	v = 4 + 4 + nlen;		memcpy(pkt + off, &v, 4); off += 4;
	v = 1;				memcpy(pkt + off, &v, 4); off += 4;
	v = diag_id;			memcpy(pkt + off, &v, 4); off += 4;
	memcpy(pkt + off, name, nlen); off += nlen;

	if (sendto(fd, pkt, off, 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("diagid reply: %s\n", strerror(errno));
	else
		dlog("assigned diag_id %u to \"%s\"\n", diag_id, name);
}

/*
 * Pull the process name out of a DIAGID record. Version 2 carries a
 * variable-length feature mask between the id and the name, so the name is not
 * at a fixed offset.
 */
static const char *diagid_name(const uint8_t *buf, size_t n)
{
	uint32_t version, feature_len;
	size_t off;

	if (n < 16)
		return NULL;
	memcpy(&version, buf + 8, 4);

	if (version == 2) {
		if (n < 20)
			return NULL;
		memcpy(&feature_len, buf + 16, 4);
		off = 20 + feature_len;
	} else {
		off = 16;
	}
	if (off >= n)
		return NULL;

	/* Must be NUL-terminated inside the packet to be safe to print. */
	if (!memchr(buf + off, 0, n - off))
		return NULL;

	return (const char *)buf + off;
}

/*
 * Put a process into real-time logging.
 *
 * A peripheral defaults to non-real-time, where it accumulates its log and
 * hands it over only when something asks -- indistinguishable from a peripheral
 * that has nothing to say. The v2 form carries the diag_id, which is the one we
 * just assigned, so each PD is switched over separately.
 */
static void send_realtime(int fd, const struct sockaddr_qrtr *to,
			  uint8_t diag_id)
{
	struct {
		uint32_t ctrl_pkt_id;
		uint32_t ctrl_pkt_data_len;
		uint32_t version;
		uint32_t sleep_vote;
		uint32_t real_time;
		uint32_t use_nrt_values;
		uint32_t commit_threshold;
		uint32_t sleep_threshold;
		uint32_t sleep_time;
		uint32_t drain_timer_val;
		uint32_t event_stale_timer_val;
		uint8_t diag_id;
	} __attribute__((packed)) m = {
		.ctrl_pkt_id = DIAG_CTRL_MSG_DIAGMODE,
		.ctrl_pkt_data_len = sizeof(m) - 8,
		.version = 2,
		.sleep_vote = 1,
		.real_time = 1,
		.diag_id = diag_id,
	};

	if (sendto(fd, &m, sizeof(m), 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("diagmode: %s\n", strerror(errno));
	else
		dlog("REALTIME on for diag_id %u\n", diag_id);
}


/*
 * Render one F3 message.
 *
 * Layout (DIAG_EXT_MSG_F): cmd, ts_type, num_args, drop_cnt, u64 ts, u16 line,
 * u16 ssid, u32 mask, then num_args 32-bit arguments, then the format string
 * and the source file name, both NUL-terminated.
 *
 * The arguments are the whole point -- "Buf not available from client 0x%lX"
 * says nothing without them -- so the format is walked and the numeric
 * conversions filled in. Arguments are always 32-bit here, whatever length
 * modifier the format claims, and %s cannot be resolved because the pointer
 * belongs to the DSP.
 */
static void print_ext_msg(const uint8_t *p, size_t n)
{
	uint32_t args[16], mask;
	uint16_t line, ssid;
	const char *fmt, *file;
	size_t off, i, nargs;
	uint64_t ts;
	int ai = 0;

	if (n < 20)
		return;
	nargs = p[2];
	if (nargs > 16 || 20 + nargs * 4 >= n)
		return;

	memcpy(&ts, p + 4, 8);
	memcpy(&line, p + 12, 2);
	memcpy(&ssid, p + 14, 2);
	memcpy(&mask, p + 16, 4);
	for (i = 0; i < nargs; i++)
		memcpy(&args[i], p + 20 + i * 4, 4);

	off = 20 + nargs * 4;
	fmt = (const char *)p + off;
	if (!memchr(p + off, 0, n - off))
		return;
	off += strlen(fmt) + 1;
	file = (off < n && memchr(p + off, 0, n - off)) ? (const char *)p + off
						       : "?";

	printf("%-28s:%-5u ssid=%-5u ", file, line, ssid);

	for (i = 0; fmt[i]; i++) {
		char spec[24];
		size_t j = 0;

		if (fmt[i] != '%') {
			putchar(fmt[i]);
			continue;
		}
		if (fmt[i + 1] == '%') {
			putchar('%');
			i++;
			continue;
		}

		spec[j++] = '%';
		i++;
		/* flags, width, precision */
		while (fmt[i] && (strchr("-+ #0", fmt[i]) || isdigit(fmt[i]) ||
				  fmt[i] == '.') && j < sizeof(spec) - 4)
			spec[j++] = fmt[i++];
		/* length modifiers are dropped: every argument is 32-bit */
		while (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'z')
			i++;

		if (!fmt[i])
			break;

		switch (fmt[i]) {
		case 'd': case 'i':
			spec[j++] = 'd'; spec[j] = 0;
			printf(spec, ai < (int)nargs ? (int)args[ai++] : 0);
			break;
		case 'u': case 'x': case 'X': case 'o': case 'p':
			spec[j++] = (fmt[i] == 'p') ? 'x' : fmt[i];
			spec[j] = 0;
			printf(spec, ai < (int)nargs ? args[ai++] : 0);
			break;
		case 'c':
			spec[j++] = 'c'; spec[j] = 0;
			printf(spec, ai < (int)nargs ? (int)args[ai++] : '?');
			break;
		case 's':
			fputs("<str>", stdout);
			break;
		default:
			putchar(fmt[i]);
			break;
		}
	}
	putchar('\n');
}

/*
 * A DATA packet is a diag_id-tagged container of one or more non-HDLC frames:
 * a four byte header, then repeating {0x7e, version, u16 len} payload {0x7e}.
 */
static void decode_data(const uint8_t *buf, size_t n)
{
	size_t off = 4;

	if (n < 8)
		return;

	while (off + 4 <= n) {
		uint16_t len;

		if (buf[off] != 0x7e) {
			off++;	/* resynchronise rather than give up */
			continue;
		}
		memcpy(&len, buf + off + 2, 2);
		if (!len || off + 4 + len > n)
			break;

		if (buf[off + 4] == DIAG_EXT_MSG_F) {
			print_ext_msg(buf + off + 4, len);
		} else if (buf[off + 4] == DIAG_LOG_F && len >= 36) {
			/*
			 * CVD records every APR packet it exchanges as a log
			 * entry rather than an F3 message, so this is where the
			 * modem<->ADSP conversation is visible. After the log
			 * header {u16 len, u16 code, u64 ts} and a four byte
			 * marker comes a verbatim 20-byte APR header, then the
			 * payload.
			 *
			 * Print it decoded: the addresses say who is talking,
			 * and the payload is the part that matters -- the
			 * mailbox config handed to the modem lives here.
			 */
			static const char *dom[] = { "?", "SIM", "PC", "MODEM",
						     "ADSP", "APPS" };
			const uint8_t *lp = buf + off + 4;
			const uint8_t *a = lp + 16;	/* APR header */
			uint16_t code, sp, dp;
			uint32_t tok, op;
			size_t i, end;

			memcpy(&code, lp + 6, 2);
			memcpy(&sp, a + 6, 2);
			memcpy(&dp, a + 10, 2);
			memcpy(&tok, a + 12, 4);
			memcpy(&op, a + 16, 4);

			printf("APR %-5s:%04x svc%02x -> %-5s:%04x svc%02x "
			       "tok=%08x op=%08x |",
			       a[5] < 6 ? dom[a[5]] : "?", sp, a[4],
			       a[9] < 6 ? dom[a[9]] : "?", dp, a[8], tok, op);

			end = len < 100 ? len : 100;
			for (i = 36; i < end; i++)
				printf(" %02x", lp[i]);
			putchar('\n');
		}

		off += 4 + len;
		if (off < n && buf[off] == 0x7e)
			off++;	/* frame trailer */
	}
	fflush(stdout);
}

/*
 * Enable the log and event streams as well as F3.
 *
 * CVD records the APR packets it exchanges as *log packets*, not as F3
 * messages, so with only the message mask set the entire modem<->ADSP
 * conversation is invisible even though its debug prints are not. Turning these
 * on is what makes the ADSP's replies observable.
 *
 * ALL_ENABLED means no payload for either, and for logs a zero equip_id covers
 * every equipment class.
 */
static void send_log_event_masks(int fd, const struct sockaddr_qrtr *to)
{
	struct {
		uint32_t cmd_type;
		uint32_t data_len;
		uint8_t stream_id;
		uint8_t status;
		uint8_t equip_id;
		uint32_t num_items;
		uint32_t log_mask_size;
	} __attribute__((packed)) lg = {
		.cmd_type = DIAG_CTRL_MSG_EQUIP_LOG_MASK,
		.data_len = sizeof(lg) - 8,
		.stream_id = 1,
		.status = DIAG_CTRL_MASK_ALL_ENABLED,
		.equip_id = 0,
		.num_items = 0,
		.log_mask_size = 0,
	};
	struct {
		uint32_t cmd_type;
		uint32_t data_len;
		uint8_t stream_id;
		uint8_t status;
		uint8_t event_config;
		uint32_t event_mask_size;
	} __attribute__((packed)) ev = {
		.cmd_type = DIAG_CTRL_MSG_EVENT_MASK_V2,
		.data_len = sizeof(ev) - 8,
		.stream_id = 1,
		.status = DIAG_CTRL_MASK_ALL_ENABLED,
		.event_config = 1,
		.event_mask_size = 0,
	};

	if (sendto(fd, &lg, sizeof(lg), 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("log mask: %s\n", strerror(errno));
	if (sendto(fd, &ev, sizeof(ev), 0, (const struct sockaddr *)to,
		   sizeof(*to)) < 0)
		dlog("event mask: %s\n", strerror(errno));
	dlog("log + event masks enabled\n");
}

static void handle_packet(const char *tag, const uint8_t *buf, size_t n)
{
	if (!n)
		return;

	if (raw) {
		const char *cn = NULL;
		uint32_t id = 0;

		if (n >= 8 && !strcmp(tag, "CNTL")) {
			memcpy(&id, buf, 4);
			cn = ctrl_name(id);
		}
		if (cn)
			fprintf(stderr, "[%9.3f] %s %s (%zu bytes): ",
				mono_sec(), tag, cn, n);
		else if (n >= 8 && !strcmp(tag, "CNTL"))
			fprintf(stderr, "[%9.3f] %s ctrl-%u (%zu bytes): ",
				mono_sec(), tag, id, n);
		else
			fprintf(stderr, "[%9.3f] %s %zu bytes: ", mono_sec(),
				tag, n);
		hexdump(buf, n);
	}

	if (buf[0] == DIAG_EXT_MSG_CONFIG_F && n > 1) {
		if (buf[1] == MSG_EXT_CFG_SET_ALL_RT_MASKS)
			dlog("%s: SET_ALL_RT_MASKS accepted\n", tag);
		else if (buf[1] == MSG_EXT_CFG_GET_SSID_RANGES)
			dlog("%s: SSID ranges reply (%zu bytes)\n", tag, n);
		return;
	}
	if (buf[0] == DIAG_BAD_CMD_F) {
		dlog("%s: BAD_CMD\n", tag);
		return;
	}
	if (buf[0] == DIAG_BAD_PARM_F) {
		dlog("%s: BAD_PARM\n", tag);
		return;
	}

	show_strings(buf, n);
}

int main(int argc, char **argv)
{
	struct sockaddr_qrtr cmd_addr = {0};
	int cntl_fd, data_fd, lookup_fd;
	uint32_t level = 0xffffffff;
	int have_cmd = 0, sent = 0, masked = 0;
	uint32_t diag_id = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw"))
			raw = 1;
		else if (!strcmp(argv[i], "--level") && i + 1 < argc)
			level = strtoul(argv[++i], NULL, 0);
		else {
			fprintf(stderr, "usage: %s [--level <mask>] [--raw]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	cntl_fd = serve(DIAG_SVC_ID, LPASS_INST_BASE + INST_ID_CNTL, "CNTL");
	data_fd = serve(DIAG_SVC_ID, LPASS_INST_BASE + INST_ID_DATA, "DATA");
	lookup_fd = start_lookup(DIAG_SVC_ID);

	if (cntl_fd < 0 && data_fd < 0) {
		dlog("could not publish any diag service\n");
		return 1;
	}

	dlog("waiting for the ADSP; ctrl-c to stop\n");

	while (!stop) {
		struct pollfd pfd[3];
		uint8_t buf[MAX_PKT];
		int nfds = 0;
		int i_cntl = -1, i_data = -1, i_look = -1;
		ssize_t n;

		if (cntl_fd >= 0) {
			pfd[nfds].fd = cntl_fd;
			pfd[nfds].events = POLLIN;
			i_cntl = nfds++;
		}
		if (data_fd >= 0) {
			pfd[nfds].fd = data_fd;
			pfd[nfds].events = POLLIN;
			i_data = nfds++;
		}
		if (lookup_fd >= 0) {
			pfd[nfds].fd = lookup_fd;
			pfd[nfds].events = POLLIN;
			i_look = nfds++;
		}
		if (!nfds)
			break;

		if (poll(pfd, nfds, 1000) < 0)
			break;

		if (i_look >= 0 && (pfd[i_look].revents & POLLIN)) {
			struct sockaddr_qrtr from;
			socklen_t fl = sizeof(from);

			n = recvfrom(lookup_fd, buf, sizeof(buf), 0,
				     (struct sockaddr *)&from, &fl);

			/*
			 * The same socket carries router notifications and the
			 * ADSP's replies to our commands; only the control port
			 * speaks the router's own packet format.
			 */
			if (n > 0 && from.sq_port != QRTR_PORT_CTRL) {
				handle_packet("CMDRSP", buf, n);
			} else if (n >= (ssize_t)sizeof(struct qrtr_ctrl_pkt)) {
				struct qrtr_ctrl_pkt pkt;

				memcpy(&pkt, buf, sizeof(pkt));
				if (pkt.cmd == QRTR_TYPE_NEW_SERVER &&
				    pkt.server.service == DIAG_SVC_ID) {
					dlog("server %#x/%u at %u:%u\n",
					     pkt.server.service,
					     pkt.server.instance,
					     pkt.server.node, pkt.server.port);

					if (pkt.server.instance ==
					    LPASS_INST_BASE + INST_ID_CMD) {
						cmd_addr.sq_family = AF_QIPCRTR;
						cmd_addr.sq_node =
							pkt.server.node;
						cmd_addr.sq_port =
							pkt.server.port;
						have_cmd = 1;
					}
				}
			}
		}

		/*
		 * Only ask for messages once the ADSP has told us where its
		 * command port is; before that there is nowhere to send.
		 */
		/*
		 * Deliberately not sending 0x7D SET_ALL_RT_MASKS: this ADSP
		 * answers it with BAD_CMD, because a peripheral with mask
		 * centralization takes masks only from the control channel.
		 * The command port is still worth knowing about for one-off
		 * queries, so the lookup stays.
		 */
		if (have_cmd && !sent) {
			dlog("ADSP command port is %u:%u\n", cmd_addr.sq_node,
			     cmd_addr.sq_port);
			sent = 1;
		}

		if (i_cntl >= 0 && (pfd[i_cntl].revents & POLLIN)) {
			struct sockaddr_qrtr from;
			socklen_t fl = sizeof(from);

			n = recvfrom(cntl_fd, buf, sizeof(buf), 0,
				     (struct sockaddr *)&from, &fl);
			/*
			 * The router delivers its own client-death notices to
			 * every bound socket. They are not diag traffic and
			 * there are hundreds of them; ignoring the control port
			 * is the difference between a readable log and noise.
			 */
			if (n > 0 && from.sq_port == QRTR_PORT_CTRL)
				n = 0;
			if (n > 0) {
				handle_packet("CNTL", buf, n);

				if (n >= 12) {
					uint32_t id, len, mlen;

					memcpy(&id, buf, 4);
					memcpy(&len, buf + 4, 4);
					memcpy(&mlen, buf + 8, 4);
					if (id == DIAG_CTRL_MSG_FEATURE &&
					    mlen <= (uint32_t)n - 12)
						reply_feature(cntl_fd, &from,
							      buf + 12, mlen);
				}

				/*
				 * Answer each DIAGID, then (re)push the mask.
				 * The masks only stick once the peripheral has
				 * its ids, so they are sent after every reply
				 * rather than once up front.
				 */
				if (n >= 12) {
					uint32_t id;

					memcpy(&id, buf, 4);
					if (id == DIAG_CTRL_MSG_DIAGID) {
						const char *nm =
							diagid_name(buf, n);

						if (nm) {
							reply_diagid(cntl_fd,
								     &from,
								     ++diag_id,
								     nm);
							send_realtime(cntl_fd,
								      &from,
								      diag_id);
						}
						send_tx_streaming(cntl_fd,
								  &from);
						masked = 1;
					}
				}

				/*
				 * The range report is the trigger for the
				 * masks: it is the first point at which we know
				 * which SSIDs the ADSP actually has.
				 */
				if (n >= 16) {
					uint32_t id;

					memcpy(&id, buf, 4);
					if (id == DIAG_CTRL_MSG_SSID_RANGE_REPORT) {
						send_f3_masks(cntl_fd, &from,
							      buf, n);
						send_log_event_masks(cntl_fd,
								     &from);
					}
				}

				if (!masked) {
					send_tx_streaming(cntl_fd, &from);
					masked = 1;
				}
			}
		}
		if (i_data >= 0 && (pfd[i_data].revents & POLLIN)) {
			struct sockaddr_qrtr from;
			socklen_t fl = sizeof(from);

			n = recvfrom(data_fd, buf, sizeof(buf), 0,
				     (struct sockaddr *)&from, &fl);
			if (n > 0 && from.sq_port != QRTR_PORT_CTRL)
				decode_data(buf, n);
		}
	}

	/*
	 * Withdraw the servers. Without this the ADSP still believes diag is
	 * connected, and the next run gets no FEATURE announcement -- and
	 * therefore never learns where to push a mask.
	 */
	unpublish(cntl_fd, DIAG_SVC_ID, LPASS_INST_BASE + INST_ID_CNTL);
	unpublish(data_fd, DIAG_SVC_ID, LPASS_INST_BASE + INST_ID_DATA);

	dlog("stopped\n");
	return 0;
}
