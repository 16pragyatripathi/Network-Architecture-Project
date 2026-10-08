/*
 * bcurl - fetch files from a BHTP/1 server (see SPEC.md).
 *
 *     ./bcurl [-v] [-I] [-X method] [-H 'name: value']... host[:port]/path [more]...
 *
 * Bodies go to stdout. Every URL after the first is fetched over the same TCP
 * connection, and bcurl never opens a second one: not for extra URLs, not to
 * retry, not after the server hangs up.
 *
 * Exit status: 0 ok, 1 usage, 2 network, 3 protocol error,
 *              4 / 5 if the worst response was a 4xx / 5xx.
 */
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PREFACE         "BHTP/1\r\n"
#define FRAME_HDR       8
#define MAX_PAYLOAD     65535
#define DEFAULT_PORT    "9000"
#define USER_AGENT      "bcurl/1.0"
#define READ_TIMEOUT    30          /* seconds to wait on a silent server */
#define MAX_EXTRA       32

#define T_REQUEST       0x01
#define T_RESPONSE      0x02
#define T_DATA          0x03
#define T_GOAWAY        0x04
#define END_STREAM      0x01

enum { M_GET = 1, M_HEAD, M_POST, M_PUT, M_DELETE };

enum { EXIT_USAGE = 1, EXIT_NET = 2, EXIT_PROTO = 3 };

static const char *methods[] = { NULL, "GET", "HEAD", "POST", "PUT", "DELETE" };

static const char *static_table[] = {
	NULL, "host", "user-agent", "accept", "if-none-match", "server",
	"date", "content-type", "content-length", "last-modified", "etag",
};
#define STATIC_COUNT 10

static const char *goaway_codes[] = {
	"NO_ERROR", "PROTOCOL_ERROR", "BAD_PREFACE", "INTERNAL_ERROR",
};

struct url {
	char host[256];
	char port[8];
	const char *path;
};

/* what we keep from a RESPONSE while reading its body */
struct reply {
	int status;
	long long length;       /* content-length, -1 if absent */
};

static int verbose;
static int show_headers;    /* -I: print status and fields to stdout */

static struct {
	char *name, *value;
} extra[MAX_EXTRA];
static int n_extra;

static uint8_t in[FRAME_HDR + MAX_PAYLOAD];
static uint8_t out[FRAME_HDR + MAX_PAYLOAD];
static size_t out_len;      /* payload bytes written to out so far */
static int out_overflow;

static unsigned get16(const uint8_t *p)
{
	return (unsigned)p[0] << 8 | p[1];
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void usage(void)
{
	fprintf(stderr,
		"usage: bcurl [-v] [-I] [-X method] [-H 'name: value']... host[:port]/path [/path | url]...\n"
		"  -v  hexdump every frame sent and received (to stderr)\n"
		"  -I  send HEAD and print the response fields instead of a body\n"
		"  -X  method: GET HEAD POST PUT DELETE, or a raw method code\n"
		"  -H  add a header field; repeat for more\n"
		"Extra paths or URLs on the same host:port reuse the one connection.\n");
}

static const char *reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 204: return "No Content";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 503: return "Service Unavailable";
	}
	return "";
}

static int is_name_char(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int static_index(const char *name, size_t len)
{
	int i;

	for (i = 1; i <= STATIC_COUNT; i++)
		if (strlen(static_table[i]) == len && memcmp(static_table[i], name, len) == 0)
			return i;
	return 0;
}

/* ---- tracing (-v) ---- */

static const char *type_name(int type)
{
	switch (type) {
	case T_REQUEST:  return "REQUEST";
	case T_RESPONSE: return "RESPONSE";
	case T_DATA:     return "DATA";
	case T_GOAWAY:   return "GOAWAY";
	}
	return NULL;
}

static void hexdump(char dir, const uint8_t *p, size_t n)
{
	size_t off, i;

	for (off = 0; off < n; off += 16) {
		fprintf(stderr, "%c %04zx  ", dir, off);
		for (i = 0; i < 16; i++) {
			if (off + i < n)
				fprintf(stderr, "%02x ", p[off + i]);
			else
				fputs("   ", stderr);
			if (i == 7)
				fputc(' ', stderr);
		}
		fputs(" |", stderr);
		for (i = 0; i < 16 && off + i < n; i++)
			fputc(isprint(p[off + i]) ? p[off + i] : '.', stderr);
		fputs("|\n", stderr);
	}
}

/* f points at a whole frame: header then payload */
static void trace_frame(char dir, const uint8_t *f)
{
	const char *name = type_name(f[2]);
	size_t len = get16(f);

	if (name)
		fprintf(stderr, "%c %s", dir, name);
	else
		fprintf(stderr, "%c unknown type 0x%02x (skipped)", dir, f[2]);
	fprintf(stderr, "  stream=%u flags=0x%02x%s length=%zu\n", (unsigned)get32(f + 4),
		f[3], f[3] & END_STREAM ? " END_STREAM" : "", len);
	hexdump(dir, f, FRAME_HDR + len);
}

/* ---- I/O ---- */

static int read_all(int fd, void *buf, size_t n)
{
	size_t got = 0;
	ssize_t r;

	while (got < n) {
		r = read(fd, (char *)buf + got, n - got);
		if (r == 0)
			return got == 0 ? 0 : -1;
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		got += (size_t)r;
	}
	return 1;
}

static int write_all(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	ssize_t w;

	while (n > 0) {
		w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static void out_bytes(const void *src, size_t n)
{
	if (out_len + n > MAX_PAYLOAD) {
		out_overflow = 1;
		return;
	}
	memcpy(out + FRAME_HDR + out_len, src, n);
	out_len += n;
}

static void out_u8(unsigned v)
{
	uint8_t b = (uint8_t)v;

	out_bytes(&b, 1);
}

static void out_u16(size_t v)
{
	uint8_t b[2];

	if (v > 0xffff) {
		out_overflow = 1;
		return;
	}
	b[0] = (uint8_t)(v >> 8);
	b[1] = (uint8_t)(v & 0xff);
	out_bytes(b, 2);
}

static void out_field(const char *name, const char *value)
{
	size_t nlen = strlen(name);
	int idx = static_index(name, nlen);

	if (idx) {
		out_u8(0x80 | (unsigned)idx);
	} else {
		out_u8((unsigned)nlen);         /* add_extra() keeps names under 128 */
		out_bytes(name, nlen);
	}
	out_u16(strlen(value));
	out_bytes(value, strlen(value));
}

static int send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream)
{
	out[0] = (uint8_t)(out_len >> 8);
	out[1] = (uint8_t)(out_len & 0xff);
	out[2] = type;
	out[3] = flags;
	out[4] = (uint8_t)(stream >> 24);
	out[5] = (uint8_t)(stream >> 16);
	out[6] = (uint8_t)(stream >> 8);
	out[7] = (uint8_t)stream;
	if (verbose)
		trace_frame('>', out);
	return write_all(fd, out, FRAME_HDR + out_len);
}

static void send_goaway(int fd, unsigned code, const char *why)
{
	out_len = 0;
	out_overflow = 0;
	out_u16(code);
	out_bytes(why, strlen(why));
	send_frame(fd, T_GOAWAY, 0, 0);
}

/* Reads the next frame into in[]. Returns 0, or minus an exit code. */
static int read_frame(int fd)
{
	int r;

	errno = 0;
	r = read_all(fd, in, FRAME_HDR);
	if (r == 0) {
		fprintf(stderr, "bcurl: server closed the connection\n");
		return -EXIT_NET;
	}
	if (r > 0)
		r = read_all(fd, in + FRAME_HDR, get16(in));
	if (r <= 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			fprintf(stderr, "bcurl: timed out waiting for the server\n");
			send_goaway(fd, 0, "timed out");
		} else {
			fprintf(stderr, "bcurl: connection lost in the middle of a frame\n");
		}
		return -EXIT_NET;
	}
	if (verbose)
		trace_frame('<', in);
	return 0;
}

/* ---- header fields ---- */

static int has_extra(const char *name)
{
	int i;

	for (i = 0; i < n_extra; i++)
		if (strcmp(extra[i].name, name) == 0)
			return 1;
	return 0;
}

/* -H 'Name: value'. Names are lowercased here, since the wire wants lowercase. */
static int add_extra(char *arg)
{
	char *colon = strchr(arg, ':'), *c, *v;

	if (!colon || colon == arg || colon - arg > 127 || n_extra == MAX_EXTRA)
		return -1;
	*colon = '\0';
	for (c = arg; *c; c++) {
		*c = (char)tolower((unsigned char)*c);
		if (!is_name_char(*c))
			return -1;
	}
	for (v = colon + 1; *v == ' ' || *v == '\t'; v++)
		;
	if (strpbrk(v, "\r\n"))
		return -1;
	extra[n_extra].name = arg;
	extra[n_extra].value = v;
	n_extra++;
	return 0;
}

/*
 * Walks the header fields in p[0..n), printing them for -v / -I and keeping what
 * we need. r is NULL when we're only tracing our own request. Returns -1 if the
 * block is malformed.
 */
static int read_fields(const uint8_t *p, size_t n, char dir, struct reply *r)
{
	const char *name;
	unsigned first, idx = 0;
	size_t nlen, vlen, i;
	char *end, num[24];

	while (n > 0) {
		first = *p++;
		n--;
		name = NULL;
		nlen = 0;
		if (first & 0x80) {
			idx = first & 0x7f;
			if (idx == 0)
				return -1;
			if (idx <= STATIC_COUNT) {
				name = static_table[idx];
				nlen = strlen(name);
			}
		} else {
			nlen = first;
			if (nlen == 0 || nlen > n)
				return -1;
			for (i = 0; i < nlen; i++)
				if (!is_name_char(p[i]))
					return -1;
			name = (const char *)p;
			p += nlen;
			n -= nlen;
		}
		if (n < 2)
			return -1;
		vlen = get16(p);
		p += 2;
		n -= 2;
		if (vlen > n)
			return -1;
		for (i = 0; i < vlen; i++)
			if (p[i] == '\r' || p[i] == '\n' || p[i] == '\0')
				return -1;

		if (!name) {
			/* index past our table: skip the field, as the spec says */
			if (verbose)
				fprintf(stderr, "%c   (field #%u, not in our table, skipped)\n", dir, idx);
		} else {
			if (verbose)
				fprintf(stderr, "%c   %.*s: %.*s\n", dir, (int)nlen, name, (int)vlen, (const char *)p);
			if (r && show_headers)
				printf("%.*s: %.*s\n", (int)nlen, name, (int)vlen, (const char *)p);
			if (r && nlen == 14 && memcmp(name, "content-length", 14) == 0) {
				if (vlen == 0 || vlen >= sizeof num)
					return -1;
				memcpy(num, p, vlen);
				num[vlen] = '\0';
				errno = 0;
				r->length = strtoll(num, &end, 10);
				if (errno || *end || !isdigit((unsigned char)num[0]))
					return -1;
			}
		}
		p += vlen;
		n -= vlen;
	}
	return 0;
}

/* ---- the exchange ---- */

static int send_request(int fd, uint32_t stream, int method, const char *path, const char *host)
{
	int i;

	out_len = 0;
	out_overflow = 0;
	out_u8((unsigned)method);
	out_u16(strlen(path));
	out_bytes(path, strlen(path));
	if (!has_extra("host"))
		out_field("host", host);
	if (!has_extra("user-agent"))
		out_field("user-agent", USER_AGENT);
	if (!has_extra("accept"))
		out_field("accept", "*/*");
	for (i = 0; i < n_extra; i++)
		out_field(extra[i].name, extra[i].value);

	if (out_overflow) {
		fprintf(stderr, "bcurl: request does not fit in one frame\n");
		return -EXIT_USAGE;
	}
	if (send_frame(fd, T_REQUEST, END_STREAM, stream) < 0) {
		perror("bcurl: write");
		return -EXIT_NET;
	}
	if (verbose) {
		if (method >= M_GET && method <= M_DELETE)
			fprintf(stderr, "> %s %s\n", methods[method], path);
		else
			fprintf(stderr, "> method %d %s\n", method, path);
		read_fields(out + FRAME_HDR + 3 + strlen(path), out_len - 3 - strlen(path), '>', NULL);
	}
	return 0;
}

/* Tells the server why we're hanging up (SPEC §5), then gives up. */
static int protocol_error(int fd, const char *what)
{
	fprintf(stderr, "bcurl: protocol error: %s\n", what);
	send_goaway(fd, 1, what);       /* PROTOCOL_ERROR */
	return -EXIT_PROTO;
}

/*
 * Sends one request and reads its response, body to stdout.
 * Returns the status, or minus the exit code to give up with.
 */
static int fetch(int fd, uint32_t stream, int method, const char *path, const char *host)
{
	struct reply r = { 0, -1 };
	long long got = 0;
	const uint8_t *p;
	unsigned code;
	uint32_t sid;
	size_t len;
	int rc;

	if ((rc = send_request(fd, stream, method, path, host)) < 0)
		return rc;

	for (;;) {
		if ((rc = read_frame(fd)) < 0)
			return rc;
		len = get16(in);
		sid = get32(in + 4);
		p = in + FRAME_HDR;

		switch (in[2]) {
		case T_RESPONSE:
			if (sid != stream || r.status)
				return protocol_error(fd, "RESPONSE for a request we are not waiting on");
			if (len < 2)
				return protocol_error(fd, "RESPONSE too short to hold a status");
			r.status = (int)get16(p);
			if (r.status < 200 || r.status > 599)
				return protocol_error(fd, "status out of range (v1 has no 1xx)");
			if (verbose)
				fprintf(stderr, "< %d %s\n", r.status, reason(r.status));
			if (show_headers)
				printf("%d %s\n", r.status, reason(r.status));
			if (read_fields(p + 2, len - 2, '<', &r) < 0)
				return protocol_error(fd, "malformed header block in RESPONSE");
			break;
		case T_DATA:
			if (sid != stream || !r.status)
				return protocol_error(fd, "DATA that does not belong to our response");
			if (method == M_HEAD || r.status == 304)
				return protocol_error(fd, "DATA in a response that has no body");
			if (len && fwrite(p, 1, len, stdout) != len) {
				perror("bcurl: stdout");
				return -EXIT_NET;
			}
			got += (long long)len;
			break;
		case T_GOAWAY:
			code = len >= 2 ? get16(p) : 1;
			fprintf(stderr, "bcurl: server sent GOAWAY %s",
				code < 4 ? goaway_codes[code] : "(unknown code)");
			if (len > 2)
				fprintf(stderr, ": %.*s", (int)(len - 2), (const char *)p + 2);
			fputc('\n', stderr);
			return code == 0 ? -EXIT_NET : -EXIT_PROTO;
		case T_REQUEST:
			return protocol_error(fd, "server sent a REQUEST");
		default:
			continue;       /* unknown type: payload already consumed, so skip it */
		}
		if (in[3] & END_STREAM)
			break;
	}
	fflush(stdout);

	/* HEAD and 304 describe a body without sending it. */
	if (r.length >= 0 && method != M_HEAD && r.status != 304 && got != r.length) {
		char why[96];
		snprintf(why, sizeof why, "content-length was %lld but %lld bytes arrived", r.length, got);
		return protocol_error(fd, why);
	}
	return r.status;
}

/* host[:port][/path], with an optional bhtp:// in front and [v6] literals allowed */
static int parse_url(const char *s, struct url *u)
{
	const char *p, *end;
	size_t n;

	if (strncmp(s, "bhtp://", 7) == 0)
		s += 7;
	if (*s == '[') {
		if (!(end = strchr(s, ']')))
			return -1;
		n = (size_t)(end - s - 1);
		p = end + 1;
		s++;
	} else {
		n = strcspn(s, ":/");
		p = s + n;
	}
	if (n == 0 || n >= sizeof u->host)
		return -1;
	memcpy(u->host, s, n);
	u->host[n] = '\0';

	if (*p == ':') {
		p++;
		n = strspn(p, "0123456789");
		if (n == 0 || n > 5)
			return -1;
		memcpy(u->port, p, n);
		u->port[n] = '\0';
		p += n;
	} else {
		strcpy(u->port, DEFAULT_PORT);
	}
	if (*p != '/' && *p != '\0')
		return -1;
	u->path = *p ? p : "/";
	return 0;
}

static int parse_method(const char *s)
{
	char *end;
	long code;
	int i;

	for (i = M_GET; i <= M_DELETE; i++)
		if (strcmp(s, methods[i]) == 0)
			return i;
	code = strtol(s, &end, 0);
	if (*s && !*end && code >= 0 && code <= 255)
		return (int)code;
	return -1;
}

static int dial(const struct url *u)
{
	struct addrinfo hints, *res, *ai;
	struct timeval tv = { READ_TIMEOUT, 0 };
	int fd = -1, err, saved = 0, on = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((err = getaddrinfo(u->host, u->port, &hints, &res)) != 0) {
		fprintf(stderr, "bcurl: %s: %s\n", u->host, gai_strerror(err));
		return -1;
	}
	/*
	 * localhost can come back as both ::1 and 127.0.0.1. We stop at the first
	 * address that accepts, so there is still only ever one connection.
	 */
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		saved = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		fprintf(stderr, "bcurl: can't connect to %s port %s: %s\n",
			u->host, u->port, strerror(saved));
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
	return fd;
}

int main(int argc, char **argv)
{
	struct url first, other;
	char host[300];
	const char *path;
	int opt, method = M_GET, fd, i, status, worst = 0;
	uint32_t stream = 0;

	while ((opt = getopt(argc, argv, "vIX:H:h")) != -1) {
		switch (opt) {
		case 'v':
			verbose = 1;
			break;
		case 'I':
			method = M_HEAD;
			show_headers = 1;
			break;
		case 'X':
			if ((method = parse_method(optarg)) < 0) {
				fprintf(stderr, "bcurl: unknown method '%s'\n", optarg);
				return EXIT_USAGE;
			}
			break;
		case 'H':
			if (add_extra(optarg) < 0) {
				fprintf(stderr, "bcurl: bad header '%s' (want 'name: value')\n", optarg);
				return EXIT_USAGE;
			}
			break;
		default:
			usage();
			return EXIT_USAGE;
		}
	}
	if (optind == argc) {
		usage();
		return EXIT_USAGE;
	}
	if (parse_url(argv[optind], &first) < 0) {
		fprintf(stderr, "bcurl: can't parse URL '%s'\n", argv[optind]);
		return EXIT_USAGE;
	}
	/* Check every target up front: anything on another server would need a
	 * second connection, and we don't open those. */
	for (i = optind + 1; i < argc; i++) {
		if (argv[i][0] == '/')
			continue;
		if (parse_url(argv[i], &other) < 0) {
			fprintf(stderr, "bcurl: can't parse URL '%s'\n", argv[i]);
			return EXIT_USAGE;
		}
		if (strcmp(other.host, first.host) != 0 || strcmp(other.port, first.port) != 0) {
			fprintf(stderr, "bcurl: %s is on a different server; bcurl uses one connection\n", argv[i]);
			return EXIT_USAGE;
		}
	}
	if (strchr(first.host, ':'))
		snprintf(host, sizeof host, "[%s]:%s", first.host, first.port);
	else
		snprintf(host, sizeof host, "%s:%s", first.host, first.port);

	signal(SIGPIPE, SIG_IGN);
	if ((fd = dial(&first)) < 0)
		return EXIT_NET;

	if (verbose) {
		fprintf(stderr, "> preface\n");
		hexdump('>', (const uint8_t *)PREFACE, 8);
	}
	if (write_all(fd, PREFACE, 8) < 0) {
		perror("bcurl: write");
		return EXIT_NET;
	}

	for (i = optind; i < argc; i++) {
		if (argv[i][0] == '/')
			path = argv[i];
		else {
			parse_url(argv[i], &other);
			path = other.path;
		}
		status = fetch(fd, ++stream, method, path, host);
		if (status < 0) {
			close(fd);
			return -status;
		}
		if (status >= 400 && !verbose)
			fprintf(stderr, "bcurl: %s: %d %s\n", path, status, reason(status));
		if (status > worst)
			worst = status;
	}
	close(fd);
	return worst >= 500 ? 5 : worst >= 400 ? 4 : 0;
}
