/*
 * Copyright (c) 2021 Omar Polo <op@omarpolo.com>
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

/*
 * Test program for fastcgi.  It speaks the protocol over stdin.
 * Can't handle more than one request at the same time.
 */

#include "../config.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FCGI_VERSION_1	1

/* subset of records that matters to us */
#define FCGI_BEGIN_REQUEST	 1
#define FCGI_END_REQUEST	 3
#define FCGI_PARAMS		 4
#define FCGI_STDIN		 5
#define FCGI_STDOUT		 6

#define SUM(a, b) (((a) << 8) + (b))

struct fcgi_header {
	uint8_t version;
	uint8_t type;
	uint8_t req_id1;
	uint8_t req_id0;
	uint8_t content_len1;
	uint8_t content_len0;
	uint8_t padding;
	uint8_t reserved;
};

struct fcgi_end_req_body {
	unsigned char app_status3;
	unsigned char app_status2;
	unsigned char app_status1;
	unsigned char app_status0;
	unsigned char proto_status;
	unsigned char reserved[3];
};

static int
prepare_header(struct fcgi_header *h, int type, int id, size_t size,
    size_t padding)
{
	memset(h, 0, sizeof(*h));

	h->version = FCGI_VERSION_1;
        h->type = type;
	h->req_id1 = (id >> 8);
	h->req_id0 = (id & 0xFF);
	h->content_len1 = (size >> 8);
	h->content_len0 = (size & 0xFF);
	h->padding = padding;

	return 0;
}

static void
must_read(int sock, void *d, size_t len)
{
	uint8_t *data = d;
	ssize_t r;

	while (len > 0) {
		switch (r = read(sock, data, len)) {
		case -1:
			err(1, "read");
		case 0:
			errx(1, "EOF");
		default:
			len -= r;
			data += r;
		}
	}
}

static void
must_write(int sock, const void *d, size_t len)
{
	const uint8_t *data = d;
	ssize_t w;

	while (len > 0) {
		switch (w = write(sock, data, len)) {
		case -1:
			err(1, "write");
		case 0:
			errx(1, "EOF");
		default:
			len -= w;
			data += w;
		}
	}
}

static int
consume(int fd, size_t len)
{
	size_t	l;
	char	buf[64];

	while (len != 0) {
		if ((l = len) > sizeof(buf))
			l =  sizeof(buf);
		must_read(fd, buf, l);
		len -= l;
	}

	return 1;
}

static void
read_header(int fd, struct fcgi_header *hdr)
{
	must_read(fd, hdr, sizeof(*hdr));
}

/* read and consume a record of the given type */
static void
assert_record(int fd, int type)
{
	struct fcgi_header hdr;

	read_header(fd, &hdr);

	if (hdr.type != type)
		errx(1, "expected record type %d; got %d",
		    type, hdr.type);

	consume(fd, SUM(hdr.content_len1, hdr.content_len0));
	consume(fd, hdr.padding);
}

int
main(int argc, char **argv)
{
	struct fcgi_header	 hdr;
	struct fcgi_end_req_body end;
	struct sockaddr_un	 sun;
	const char		*path;
	const char		*msg;
	size_t			 len;
	int			 ch, sock, s;

	while ((ch = getopt(argc, argv, "")) != -1)
		errx(1, "wrong usage");
	argc -= optind;
	argv += optind;
	if (argc != 1)
		errx(1, "wrong usage");

	path = argv[0];

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		err(1, "socket");

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	(void)strlcpy(sun.sun_path, path, sizeof(sun.sun_path));

	if (unlink(path) == -1 && errno != ENOENT)
		err(1, "unlink %s", path);

	if (bind(sock, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		err(1, "bind");

	if (listen(sock, 5) == -1)
		err(1, "listen");

	for (;;) {
		if ((s = accept(sock, NULL, NULL)) == -1) {
			warn("retrying; accept failed");
			continue;
		}

		assert_record(s, FCGI_BEGIN_REQUEST);

		/* read params */
		for (;;) {
			read_header(s, &hdr);

			consume(s, SUM(hdr.content_len1, hdr.content_len0));
			consume(s, hdr.padding);

			if (hdr.type != FCGI_PARAMS)
				errx(1, "got %d; expecting PARAMS", hdr.type);

			if (hdr.content_len0 == 0 &&
			    hdr.content_len1 == 0 &&
			    hdr.padding == 0)
				break;
		}

		assert_record(s, FCGI_STDIN);

		msg = "20 text/gemini\r\n# hello from fastcgi!\n";
		len = strlen(msg);

		prepare_header(&hdr, FCGI_STDOUT, 1, len, 0);
		must_write(s, &hdr, sizeof(hdr));
		must_write(s, msg, len);

		msg = "some more content in the page...\n";
		len = strlen(msg);

		prepare_header(&hdr, FCGI_STDOUT, 1, len, 0);
		must_write(s, &hdr, sizeof(hdr));
		must_write(s, msg, len);

		prepare_header(&hdr, FCGI_END_REQUEST, 1, sizeof(end), 0);
		write(s, &hdr, sizeof(hdr));
		memset(&end, 0, sizeof(end));
		write(s, &end, sizeof(end));
	}
}
