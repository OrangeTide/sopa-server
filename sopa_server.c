/* Copyright (c) 2012 Jon Mayo <jon@cobra-kai.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* port serve */
#define HTTP_PORT 80
/* timeout in seconds for idle connections */
#define HTTP_TIMEOUT 5
/* maximum size of a header, this is pretty important. */
#define HTTP_HDRMAX 512
/* buffer length for processing requests, just a performance parameter */
#define HTTP_BUFSIZE 64

#define perror_and_die(reason) do { \
		perror(reason); \
		exit(EXIT_FAILURE); \
	} while(0)

struct client {
	int fd;
	int state;
	struct client *next;
	time_t last;
	unsigned write_ofs;
};

static struct client *reader_head;
static struct client *writer_head; /* list of client waiting to write */
static int fd_max;
static time_t youngest;
static fd_set rfds, wfds;
static char hdr[HTTP_HDRMAX];
static size_t hdr_len;
static char *msg;
static size_t msg_len;

static void log_info(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

#ifndef NDEBUG
static void log_debug(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
#else
#define log_debug(...)
#endif

#ifndef NDEBUG
static void dump_fdset(fd_set *fds)
{
	int i;

	printf("(%p)", fds);
	for (i = 0; i <= fd_max; i++) {
		if (FD_ISSET(i, fds))
			printf(" %d", i);
	}
	printf("\n");
}

static void dump_list(const struct client *head)
{
	while (head) {
		printf(" %d", head->fd);
		head = head->next;
	}
	printf("\n");
}
#endif

static void client_free(struct client *cl, struct client **prev)
{
	assert(cl != NULL && prev != NULL);
	assert(*prev == cl);
	assert(cl->fd != -1);
	*prev = cl->next;
	cl->next = NULL;
	log_debug("freeing client fd %d (%p)\n", cl->fd, cl);
	close(cl->fd);
	FD_CLR(cl->fd, &rfds);
	FD_CLR(cl->fd, &wfds);
	cl->fd = -1;
	free(cl);
}

static void client_accept(int listen_fd)
{
	int newfd;
	struct sockaddr_in sin;
	socklen_t len = sizeof(sin);
	struct client *new;

	newfd = accept(listen_fd, (struct sockaddr*)&sin, &len);
	if (newfd < 0) {
		if (errno != EAGAIN)
			perror_and_die("accept");
		return;
	}
	log_info("new client fd %d\n", newfd);
	new = calloc(1, sizeof(*new));
	new->fd = newfd;
	time(&new->last);
	new->next = reader_head;
	reader_head = new;
	FD_SET(new->fd, &rfds);
	if (new->fd > fd_max)
		fd_max = newfd;
	assert(new != new->next);
}

static int client_write(struct client *cl,
	struct client **prev __attribute__((unused)))
{
	const char *buf;
	ssize_t buflen;
	ssize_t res;

	switch (cl->state) {
		case 0:
			buf = hdr;
			buflen = hdr_len;
			break;
		case 1:
			buf = msg;
			buflen = msg_len;
			break;
		default:
			log_info("closing fd %d:completed\n", cl->fd);
			return 0;
	}

	assert(buf != NULL);
	assert(cl->fd != -1);
	res = write(cl->fd, buf + cl->write_ofs, buflen - cl->write_ofs);
	if (res < 0) {
		log_info("closing fd %d:%s\n", cl->fd, strerror(errno));
		return 0;
	}
	cl->write_ofs += res;
	assert(cl->write_ofs <= buflen);
	if (cl->write_ofs == buflen) {
		cl->state++;
		cl->write_ofs = 0;
	}
	time(&cl->last);
	return 1;
}

static void do_get(struct client *cl, struct client **prev)
{
	assert(cl != cl->next);
	log_debug("%s():%d:HERE\n", __func__, __LINE__);
	/* transistion to a writer */
	*prev = cl->next;
	cl->next = writer_head;
	writer_head = cl;
	/* clear rfd bit and set wfd bit */
	FD_CLR(cl->fd, &rfds);
	FD_SET(cl->fd, &wfds);
	cl->write_ofs = 0;
	assert(cl != cl->next);
	cl->state = 0;
}

static int client_read(struct client *cl, struct client **prev)
{
	char _buf[HTTP_BUFSIZE];
	char *buf = _buf;
	ssize_t len = sizeof(_buf);

	len = read(cl->fd, buf, len);
	if (len <= 0) {
		log_info("closing fd %d:%s\n", cl->fd, strerror(errno));
		return 0;
	}
	log_debug("%s():fd %d read %d bytes\n", __func__, cl->fd, len);
	time(&cl->last);
	while (len > 0) {
		log_debug("read fd %d:state=%d ch='%c' (%#x)\n",
			cl->fd, cl->state, *buf, *buf);
		switch (cl->state) {
		case 0:
			if (*buf == 'G')
				cl->state++;
			else
				return 0;
			break;
		case 1:
			if (*buf == 'E')
				cl->state++;
			else
				return 0;
			break;
		case 2:
			if (*buf == 'T')
				cl->state++;
			else
				return 0;
			break;
		case 3:
			if (*buf == ' ')
				cl->state++;
			else
				return 0;
			break;
		case 4:
			log_debug("%s():%d:HERE\n", __func__, __LINE__);
			if (*buf == '\n')
				cl->state++;
			break;
		case 5: /* detect blank line - starts with \r or \n */
			log_debug("%s():%d:HERE\n", __func__, __LINE__);
			if (*buf == '\r' || *buf == '\n') {
				do_get(cl, prev);
				return 1;
			} else {
				cl->state++;
			}
			break;
		case 6: /* loop through arguments */
			if (*buf == '\n')
				cl->state = 5;
			break;
		}
		buf++;
		len--;
	}

	return 1;
}

static void process_generic(struct client **head, fd_set *fds,
	int (*foreach)(struct client *cl, struct client **prev))
{
	struct client *curr, **prev;
	time_t timeout;

	time(&timeout);
	timeout -= HTTP_TIMEOUT;
	for (prev = head; (curr = *prev); ) {
		if (curr->last < youngest) {
			/* log_debug("fd %d:%d is younger than %d\n",
				curr->fd, curr->last, youngest); */
			youngest = curr->last;
		}
		if (curr->last <= timeout) {
			log_info("closing fd %d, timeout\n", curr->fd);
			client_free(curr, prev);
			continue;
		}
		log_debug("%s():checking fd %d (set=%d)\n", __func__,
			curr->fd, FD_ISSET(curr->fd, fds));
		if (FD_ISSET(curr->fd, fds) && !foreach(curr, prev)) {
			log_info("closing fd %d, disconnect\n", curr->fd);
			client_free(curr, prev);
			continue;
		}
		log_debug("add fd %d back to fdset %p\n", curr->fd, fds);
		FD_SET(curr->fd, fds); /* set for next select() */
		prev = &curr->next;
	}
}

static void process_readers(void)
{
	process_generic(&reader_head, &rfds, client_read);
}

static void process_writers(void)
{
	process_generic(&writer_head, &wfds, client_write);
}

static void encode_hdr(void)
{
	char timebuf[128];
	time_t t;
	struct tm *tm;

	time(&t);
	tm = gmtime(&t);
	strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %T %z", tm);

	hdr_len = snprintf(hdr, sizeof(hdr),
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html; charset=UTF-8\r\n"
		"Date: %s\r\n"
		"Content-Length: %zu\r\n"
		"\r\n", timebuf, msg_len);
	if (hdr_len >= sizeof(hdr))
		perror_and_die("snprint");
}

static void load_file(const char *filename)
{
	int fd;
	int e;
	ssize_t len;
	struct stat st;
	int tries = 10;

	fd = open(filename, O_RDONLY);
	if (fd == -1)
		perror_and_die(filename);
	do {
		e = fstat(fd, &st);
		if (e)
			perror_and_die(filename);
		free(msg);
		msg_len = st.st_size;
		msg = calloc(1, st.st_size + 1);
		len = read(fd, msg, msg_len);
		if (len < 0)
			perror_and_die(filename);
		if (!tries--) { /* give up if we fail the race too many times */
			log_info("%s:unable to determine size of file\n",
				filename);
			abort();
		}
	} while ((size_t)len != msg_len);
	close(fd);
	encode_hdr();
}

int main()
{
	int listen_fd;
	struct sockaddr_in sin;
	int e;
	int op = 1;

	load_file("sopa.html");
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0)
		perror_and_die("socket");
	e = setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &op, sizeof(op));
	if (e)
		perror_and_die("SO_REUSEADDR");
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(HTTP_PORT);
	e = bind(listen_fd, (struct sockaddr*)&sin, sizeof(sin));
	if (e)
		perror_and_die("bind");
	e = fcntl(listen_fd, F_SETFL, O_NONBLOCK);
	if (e)
		perror_and_die("fctnl");
	e = listen(listen_fd, SOMAXCONN);
	if (e)
		perror_and_die("listen");
	fd_max = listen_fd;
	youngest = INT_MAX;

	memset(&rfds, 0, sizeof(rfds));
	memset(&wfds, 0, sizeof(wfds));
	FD_SET(listen_fd, &rfds);

	while (1) {
		struct timeval _tv = { 0, 0 };
		struct timeval *tv;

		if (youngest != INT_MAX) {
			time_t now, expire = youngest + HTTP_TIMEOUT;

			time(&now);
			tv = &_tv;
			tv->tv_sec = (expire <= now) ? 0 : expire - now;
			log_debug("wait for %d seconds\n", tv->tv_sec);
		} else {
			tv = NULL;
			log_debug("waiting for new connections\n");
		}
#ifndef NDEBUG
		printf("rfds: "); dump_fdset(&rfds);
		printf("wfds: "); dump_fdset(&wfds);
		printf("readers: "); dump_list(reader_head);
		printf("writers: "); dump_list(writer_head);
#endif
		e = select(fd_max + 1, &rfds, &wfds, NULL, tv);
		if (e < 0)
			perror_and_die("");
		if (FD_ISSET(listen_fd, &rfds)) {
			client_accept(listen_fd);
			e--;
		}
		FD_SET(listen_fd, &rfds);
		youngest = INT_MAX; /* processing will update this */
		process_readers();
		process_writers();
	}
	close(listen_fd);
	return 0;
}
