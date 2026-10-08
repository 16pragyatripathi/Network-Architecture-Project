/*
 * bserve - serve the files under a directory over BHTP/1 (see SPEC.md).
 *
 *     ./bserve ./www 9000
 *
 * Every connection gets its own process. The child reads frames until the client
 * hangs up, answering each REQUEST in the order it arrived; the connection is
 * never closed just because a response is finished.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PREFACE         "BHTP/1\r\n"
#define PREFACE_LEN     8
#define FRAME_HDR       8
#define MAX_PAYLOAD     65535
#define DATA_CHUNK      16384
#define IDLE_TIMEOUT    60          /* seconds without a frame before we hang up */
#define SERVER_NAME     "bserve/1.0"

/* frame types and flags, SPEC §2 */
#define T_REQUEST       0x01
#define T_RESPONSE      0x02
#define T_DATA          0x03
#define T_GOAWAY        0x04
#define END_STREAM      0x01

/* GOAWAY codes, SPEC §5 */
#define NO_ERROR        0
#define PROTOCOL_ERROR  1
#define BAD_PREFACE     2

enum { M_GET = 1, M_HEAD, M_POST, M_PUT, M_DELETE };

static const char *method_names[] = { NULL, "GET", "HEAD", "POST", "PUT", "DELETE" };

/* SPEC §3. Index 0 is not a valid index on the wire. */
static const char *static_table[] = {
	NULL, "host", "user-agent", "accept", "if-none-match", "server",
	"date", "content-type", "content-length", "last-modified", "etag",
};
#define STATIC_COUNT 10

static const struct {
	const char *ext, *type;
} mime_types[] = {
	{ "html", "text/html; charset=utf-8" },
	{ "htm",  "text/html; charset=utf-8" },
	{ "css",  "text/css; charset=utf-8" },
	{ "js",   "text/javascript; charset=utf-8" },
	{ "txt",  "text/plain; charset=utf-8" },
	{ "md",   "text/markdown; charset=utf-8" },
	{ "json", "application/json" },
	{ "svg",  "image/svg+xml" },
	{ "png",  "image/png" },
	{ "jpg",  "image/jpeg" },
	{ "jpeg", "image/jpeg" },
	{ "gif",  "image/gif" },
	{ "ico",  "image/x-icon" },
	{ "pdf",  "application/pdf" },
};

struct request {
	uint32_t stream;
	int method;
	char target[MAX_PAYLOAD];       /* the path field, NUL-terminated */
	char if_none_match[256];
	int status;                     /* what we answered, for the log */
	long long sent;                 /* body bytes sent, for the log */
};

/* A payload being assembled in outbuf, just after room for the frame header. */
struct block {
	size_t len;
	int overflow;
};

static char root[PATH_MAX];
static size_t root_len;
static char peer[NI_MAXHOST + NI_MAXSERV + 4];

/* One connection per process, so plain statics are fine. */
static uint8_t inbuf[MAX_PAYLOAD];
static uint8_t outbuf[FRAME_HDR + MAX_PAYLOAD];
static struct request rq;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xff;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = (v >> 16) & 0xff;
	p[2] = (v >> 8) & 0xff;
	p[3] = v & 0xff;
}

/* 1 = got all n bytes, 0 = EOF before the first byte, -1 = error, timeout or short read */
static int read_all(int fd, void *buf, size_t n)
{
	size_t got = 0;

	while (got < n) {
		ssize_t r = read(fd, (char *)buf + got, n - got);
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
	const char *p = buf;

	while (n > 0) {
		ssize_t w = write(fd, p, n);
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

/* The payload is already sitting in outbuf; fill in the header in front of it and send. */
static int send_frame(int fd, uint8_t type, uint8_t flags, uint32_t stream, size_t len)
{
	put16(outbuf, (uint16_t)len);
	outbuf[2] = type;
	outbuf[3] = flags;
	put32(outbuf + 4, stream);
	return write_all(fd, outbuf, FRAME_HDR + len);
}

static void emit(struct block *b, const void *src, size_t n)
{
	if (b->overflow || b->len + n > MAX_PAYLOAD) {
		b->overflow = 1;
		return;
	}
	memcpy(outbuf + FRAME_HDR + b->len, src, n);
	b->len += n;
}

static void emit16(struct block *b, uint16_t v)
{
	uint8_t tmp[2];

	put16(tmp, v);
	emit(b, tmp, 2);
}

static void add_field(struct block *b, const char *name, const char *value)
{
	size_t nlen = strlen(name), vlen = strlen(value);
	uint8_t first;
	int i;

	for (i = 1; i <= STATIC_COUNT; i++)
		if (strcmp(static_table[i], name) == 0)
			break;
	if (i <= STATIC_COUNT) {
		first = 0x80 | i;
		emit(b, &first, 1);
	} else {
		first = (uint8_t)nlen;          /* our own names are all short */
		emit(b, &first, 1);
		emit(b, name, nlen);
	}
	emit16(b, (uint16_t)vlen);
	emit(b, value, vlen);
}

static void send_goaway(int fd, uint16_t code, const char *why)
{
	struct block b = { 0, 0 };

	emit16(&b, code);
	emit(&b, why, strlen(why));
	send_frame(fd, T_GOAWAY, 0, 0, b.len);
	fprintf(stderr, "%s goaway %u: %s\n", peer, code, why);
}

static const char *reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 304: return "Not Modified";
	case 400: return "Bad Request";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	}
	return "";
}

static void http_date(time_t t, char *buf, size_t n)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, n, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

static const char *mime_type(const char *path)
{
	const char *dot = strrchr(path, '.');
	size_t i;

	if (dot && !strchr(dot, '/'))
		for (i = 0; i < sizeof mime_types / sizeof mime_types[0]; i++)
			if (strcasecmp(dot + 1, mime_types[i].ext) == 0)
				return mime_types[i].type;
	return "application/octet-stream";
}

static int is_name_char(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
	       (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/*
 * Parses a REQUEST payload (SPEC §3). Returns NULL if it is well formed, otherwise
 * a short description of the first problem, which goes back in the 400 body.
 */
static const char *parse_request(const uint8_t *p, size_t n, struct request *r)
{
	size_t plen, i;

	if (n < 3)
		return "payload too short for method and path length";
	r->method = p[0];
	plen = get16(p + 1);
	p += 3;
	n -= 3;
	if (plen > n)
		return "path length runs past the end of the payload";
	if (plen == 0 || p[0] != '/')
		return "path must start with '/'";
	if (memchr(p, '\0', plen))
		return "NUL byte in path";
	memcpy(r->target, p, plen);
	r->target[plen] = '\0';
	p += plen;
	n -= plen;

	while (n > 0) {
		const char *name = NULL;
		uint8_t first = *p++;
		size_t vlen;

		n--;
		if (first & 0x80) {
			unsigned idx = first & 0x7f;
			if (idx == 0)
				return "header index 0";
			/* an index past our table is skipped below, not rejected (SPEC §3) */
			if (idx <= STATIC_COUNT)
				name = static_table[idx];
		} else {
			size_t nlen = first;
			if (nlen == 0)
				return "empty header name";
			if (nlen > n)
				return "header name runs past the end of the payload";
			for (i = 0; i < nlen; i++)
				if (!is_name_char(p[i]))
					return "header name is not a lowercase token";
			/* a literal is allowed to spell out a name that has an index */
			for (i = 1; i <= STATIC_COUNT; i++)
				if (strlen(static_table[i]) == nlen && memcmp(static_table[i], p, nlen) == 0)
					name = static_table[i];
			p += nlen;
			n -= nlen;
		}
		if (n < 2)
			return "header value length missing";
		vlen = get16(p);
		p += 2;
		n -= 2;
		if (vlen > n)
			return "header value runs past the end of the payload";
		for (i = 0; i < vlen; i++)
			if (p[i] == '\r' || p[i] == '\n' || p[i] == '\0')
				return "CR, LF or NUL in header value";
		if (name && strcmp(name, "if-none-match") == 0 && vlen < sizeof r->if_none_match) {
			memcpy(r->if_none_match, p, vlen);
			r->if_none_match[vlen] = '\0';
		}
		p += vlen;
		n -= vlen;
	}
	return NULL;
}

/*
 * Turns a request target into a plain path, in place: drops the query, undoes
 * percent-escapes, and refuses anything that tries to climb out with "..".
 * Returns NULL on success, otherwise why the path is no good.
 */
static const char *clean_path(char *path)
{
	char *in, *out, *seg;

	path[strcspn(path, "?")] = '\0';
	for (in = out = path; *in; in++, out++) {
		if (*in == '%') {
			int hi = hexval(in[1]), lo;
			if (hi < 0 || (lo = hexval(in[2])) < 0)
				return "bad percent-escape in path";
			*out = (char)(hi << 4 | lo);
			if (*out == '\0')
				return "%00 in path";
			in += 2;
		} else {
			*out = *in;
		}
	}
	*out = '\0';

	/* Simpler and safer to refuse ".." outright than to normalise it. */
	for (seg = path; seg; seg = strchr(seg + 1, '/'))
		if (strncmp(seg, "/..", 3) == 0 && (seg[3] == '/' || seg[3] == '\0'))
			return "'..' in path";
	return NULL;
}

/*
 * Opens the file a cleaned path names. Directories mean their index.html.
 * Returns an fd, or minus the status to answer with. real gets the resolved path.
 */
static int open_target(const char *path, char *real, struct stat *st)
{
	char want[PATH_MAX];
	size_t n;
	int fd;

	if (snprintf(want, sizeof want, "%s%s", root, path) >= (int)sizeof want)
		return -404;
	if (stat(want, st) == 0 && S_ISDIR(st->st_mode)) {
		n = strlen(want);
		if (snprintf(want + n, sizeof want - n, "%sindex.html",
			     want[n - 1] == '/' ? "" : "/") >= (int)(sizeof want - n))
			return -404;
	} else if (want[strlen(want) - 1] == '/') {
		return -404;                    /* "/file.txt/" names a directory that isn't one */
	}
	if (!realpath(want, real))
		return errno == EACCES ? -403 : -404;

	/* Symlinks can point anywhere. Only serve what really lives under root. */
	if (strncmp(real, root, root_len) != 0 || real[root_len] != '/')
		return -404;

	/* O_NONBLOCK so a FIFO dropped into the tree can't hang us in open() */
	fd = open(real, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return errno == EACCES ? -403 : errno == ENOENT ? -404 : -500;
	if (fstat(fd, st) < 0 || !S_ISREG(st->st_mode)) {
		close(fd);
		return -404;
	}
	return fd;
}

/* Starts a RESPONSE payload with the fields every response carries. */
static void begin_response(struct block *b, int status)
{
	char date[64];

	b->len = 0;
	b->overflow = 0;
	emit16(b, (uint16_t)status);
	add_field(b, "server", SERVER_NAME);
	http_date(time(NULL), date, sizeof date);
	add_field(b, "date", date);
}

static int send_error(int fd, struct request *r, int status, const char *detail)
{
	char body[512], length[32];
	struct block b;
	int n;

	n = snprintf(body, sizeof body, "%d %s%s%s\n", status, reason(status),
		     detail ? ": " : "", detail ? detail : "");
	if (n >= (int)sizeof body)
		n = sizeof body - 1;
	snprintf(length, sizeof length, "%d", n);

	begin_response(&b, status);
	add_field(&b, "content-type", "text/plain; charset=utf-8");
	add_field(&b, "content-length", length);
	if (status == 405)
		add_field(&b, "allow", "GET, HEAD");    /* rare enough to stay out of the table */
	r->status = status;

	if (r->method == M_HEAD)
		return send_frame(fd, T_RESPONSE, END_STREAM, r->stream, b.len);
	if (send_frame(fd, T_RESPONSE, 0, r->stream, b.len) < 0)
		return -1;
	memcpy(outbuf + FRAME_HDR, body, (size_t)n);
	r->sent = n;
	return send_frame(fd, T_DATA, END_STREAM, r->stream, (size_t)n);
}

static int send_file(int fd, struct request *r, int file, const struct stat *st, const char *path)
{
	char etag[64], modified[64], length[32];
	struct block b;
	off_t left = st->st_size;
	ssize_t got;
	size_t want;

	snprintf(etag, sizeof etag, "\"%llx-%llx\"",
		 (unsigned long long)st->st_mtime, (unsigned long long)st->st_size);
	http_date(st->st_mtime, modified, sizeof modified);

	/* Etags are quoted, so finding ours as a substring is enough for "a", "b" lists. */
	if (r->if_none_match[0] &&
	    (strcmp(r->if_none_match, "*") == 0 || strstr(r->if_none_match, etag))) {
		begin_response(&b, 304);
		add_field(&b, "last-modified", modified);
		add_field(&b, "etag", etag);
		r->status = 304;
		return send_frame(fd, T_RESPONSE, END_STREAM, r->stream, b.len);
	}

	snprintf(length, sizeof length, "%lld", (long long)st->st_size);
	begin_response(&b, 200);
	add_field(&b, "content-type", mime_type(path));
	add_field(&b, "content-length", length);
	add_field(&b, "last-modified", modified);
	add_field(&b, "etag", etag);
	r->status = 200;

	if (r->method == M_HEAD || left == 0)
		return send_frame(fd, T_RESPONSE, END_STREAM, r->stream, b.len);
	if (send_frame(fd, T_RESPONSE, 0, r->stream, b.len) < 0)
		return -1;

	while (left > 0) {
		want = left < DATA_CHUNK ? (size_t)left : DATA_CHUNK;
		got = read(file, outbuf + FRAME_HDR, want);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0) {
			/*
			 * The file shrank while we were sending it. content-length has
			 * already gone out, so all we can do is end the stream short and
			 * let the client notice.
			 */
			return send_frame(fd, T_DATA, END_STREAM, r->stream, 0);
		}
		left -= got;
		r->sent += got;
		if (send_frame(fd, T_DATA, left > 0 ? 0 : END_STREAM, r->stream, (size_t)got) < 0)
			return -1;
	}
	return 0;
}

static void log_request(const struct request *r)
{
	char m[16];

	if (r->method >= M_GET && r->method <= M_DELETE)
		snprintf(m, sizeof m, "%s", method_names[r->method]);
	else
		snprintf(m, sizeof m, "method-%d", r->method);
	fprintf(stderr, "%s #%u %s %s %d %lld\n", peer, r->stream, m,
		r->target[0] ? r->target : "-", r->status, r->sent);
}

static int handle_request(int fd, uint32_t stream, const uint8_t *payload, size_t len)
{
	static char path[MAX_PAYLOAD];
	char real[PATH_MAX];
	const char *bad;
	struct stat st;
	int file, rc;

	memset(&rq, 0, sizeof rq);
	rq.stream = stream;

	if ((bad = parse_request(payload, len, &rq)) != NULL) {
		rc = send_error(fd, &rq, 400, bad);
	} else if (rq.method == M_POST || rq.method == M_PUT || rq.method == M_DELETE) {
		rc = send_error(fd, &rq, 405, "this server only reads files");
	} else if (rq.method != M_GET && rq.method != M_HEAD) {
		rc = send_error(fd, &rq, 501, "unknown method code");
	} else {
		strcpy(path, rq.target);
		if ((bad = clean_path(path)) != NULL)
			rc = send_error(fd, &rq, 400, bad);
		else if ((file = open_target(path, real, &st)) < 0)
			rc = send_error(fd, &rq, -file, NULL);
		else {
			rc = send_file(fd, &rq, file, &st, real);
			close(file);
		}
	}
	log_request(&rq);
	return rc;
}

static void serve_connection(int fd)
{
	uint8_t hdr[FRAME_HDR];
	uint32_t last_stream = 0, stream;
	unsigned requests = 0;
	size_t len;
	int r;

	errno = 0;
	r = read_all(fd, hdr, PREFACE_LEN);
	if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		send_goaway(fd, NO_ERROR, "idle timeout");
	if (r != 1)
		return;
	if (memcmp(hdr, PREFACE, PREFACE_LEN) != 0) {
		send_goaway(fd, BAD_PREFACE, "expected the BHTP/1 preface");
		return;
	}

	for (;;) {
		errno = 0;
		r = read_all(fd, hdr, FRAME_HDR);
		if (r == 0)
			break;                  /* client hung up between frames: the normal way out */
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				send_goaway(fd, NO_ERROR, "idle timeout");
			break;
		}
		len = get16(hdr);
		stream = get32(hdr + 4);
		if (read_all(fd, inbuf, len) != 1)
			break;

		switch (hdr[2]) {
		case T_REQUEST:
			if (stream == 0 || stream <= last_stream) {
				send_goaway(fd, PROTOCOL_ERROR, "stream ids must go up");
				goto out;
			}
			last_stream = stream;
			requests++;
			if (handle_request(fd, stream, inbuf, len) < 0)
				goto out;
			break;
		case T_DATA:
			break;                  /* nothing we serve takes a body; drop it */
		case T_GOAWAY:
			goto out;
		case T_RESPONSE:
			send_goaway(fd, PROTOCOL_ERROR, "RESPONSE sent to a server");
			goto out;
		default:
			/* Unknown type. Its payload is already read, so skipping it is just
			 * carrying on (SPEC §2). */
			break;
		}
	}
out:
	fprintf(stderr, "%s closed after %u request%s\n", peer, requests, requests == 1 ? "" : "s");
}

static int listen_on(int port)
{
	struct sockaddr_in6 a6;
	struct sockaddr_in a4;
	int fd, on = 1, off = 0;

	/* One IPv6 socket that also takes IPv4, so "localhost" works whichever
	 * address the client tries first. */
	fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd >= 0) {
		memset(&a6, 0, sizeof a6);
		a6.sin6_family = AF_INET6;
		a6.sin6_addr = in6addr_any;
		a6.sin6_port = htons(port);
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
		setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off);
		if (bind(fd, (struct sockaddr *)&a6, sizeof a6) == 0 && listen(fd, 64) == 0)
			return fd;
		close(fd);
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("bserve: socket");
		return -1;
	}
	memset(&a4, 0, sizeof a4);
	a4.sin_family = AF_INET;
	a4.sin_addr.s_addr = htonl(INADDR_ANY);
	a4.sin_port = htons(port);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
	if (bind(fd, (struct sockaddr *)&a4, sizeof a4) < 0 || listen(fd, 64) < 0) {
		perror("bserve: bind");
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	struct timeval tv = { IDLE_TIMEOUT, 0 };
	char host[NI_MAXHOST], serv[NI_MAXSERV], *end;
	struct stat st;
	long port;
	int lfd, fd, on = 1;
	pid_t pid;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <root-dir> <port>\n", argv[0]);
		return 1;
	}
	if (!realpath(argv[1], root) || stat(root, &st) < 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "bserve: %s is not a directory\n", argv[1]);
		return 1;
	}
	if (strcmp(root, "/") == 0)
		root[0] = '\0';                 /* so root + "/x" is "/x", not "//x" */
	root_len = strlen(root);

	errno = 0;
	port = strtol(argv[2], &end, 10);
	if (errno || *end || port < 1 || port > 65535) {
		fprintf(stderr, "bserve: bad port '%s'\n", argv[2]);
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);       /* a vanished client shows up as a write error */
	signal(SIGCHLD, SIG_IGN);       /* children reap themselves */

	if ((lfd = listen_on((int)port)) < 0)
		return 1;
	fprintf(stderr, "bserve: serving %s on port %ld\n", root[0] ? root : "/", port);

	for (;;) {
		addrlen = sizeof addr;
		fd = accept(lfd, (struct sockaddr *)&addr, &addrlen);
		if (fd < 0) {
			if (errno != EINTR)
				perror("bserve: accept");
			continue;
		}
		pid = fork();
		if (pid < 0) {
			perror("bserve: fork");
			close(fd);
			continue;
		}
		if (pid > 0) {
			close(fd);
			continue;
		}

		close(lfd);
		if (getnameinfo((struct sockaddr *)&addr, addrlen, host, sizeof host,
				serv, sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) == 0)
			snprintf(peer, sizeof peer, "%s:%s", host, serv);
		else
			snprintf(peer, sizeof peer, "?");

		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		/* A response is a small RESPONSE frame then DATA. With Nagle on, the
		 * DATA would sit waiting for the client's delayed ACK. */
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);

		serve_connection(fd);
		close(fd);
		_exit(0);
	}
}
