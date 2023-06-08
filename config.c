/*
 * Copyright (c) 2023 Omar Polo <op@omarpolo.com>
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

#include "gmid.h"

#include <sys/stat.h>

#include <fcntl.h>
#include <limits.h>
#include <string.h>

#include "log.h"
#include "proc.h"

void
config_init(void)
{
	TAILQ_INIT(&hosts);

	conf.port = 1965;
	conf.ipv6 = 0;
	conf.protos = TLS_PROTOCOL_TLSv1_2 | TLS_PROTOCOL_TLSv1_3;

	init_mime(&conf.mime);

	conf.prefork = 3;

	conf.sock4 = -1;
	conf.sock6 = -1;
}

void
config_free(void)
{
	struct privsep *ps;
	struct vhost *h, *th;
	struct location *l, *tl;
	struct proxy *p, *tp;
	struct envlist *e, *te;
	struct alist *a, *ta;

	ps = conf.ps;

	if (conf.sock4 != -1) {
		event_del(&conf.evsock4);
		close(conf.sock4);
	}

	if (conf.sock6 != -1) {
		event_del(&conf.evsock6);
		close(conf.sock6);
	}

	free_mime(&conf.mime);
	memset(&conf, 0, sizeof(conf));

	conf.ps = ps;
	conf.sock4 = conf.sock6 = -1;
	conf.protos = TLS_PROTOCOL_TLSv1_2 | TLS_PROTOCOL_TLSv1_3;
	init_mime(&conf.mime);

	TAILQ_FOREACH_SAFE(h, &hosts, vhosts, th) {
		free(h->cert);
		free(h->key);
		free(h->ocsp);

		TAILQ_FOREACH_SAFE(l, &h->locations, locations, tl) {
			TAILQ_REMOVE(&h->locations, l, locations);

			if (l->dirfd != -1)
				close(l->dirfd);

			free(l);
		}

		TAILQ_FOREACH_SAFE(e, &h->params, envs, te) {
			TAILQ_REMOVE(&h->params, e, envs);
			free(e);
		}

		TAILQ_FOREACH_SAFE(a, &h->aliases, aliases, ta) {
			TAILQ_REMOVE(&h->aliases, a, aliases);
			free(a);
		}

		TAILQ_FOREACH_SAFE(p, &h->proxies, proxies, tp) {
			TAILQ_REMOVE(&h->proxies, p, proxies);
			tls_unload_file(p->cert, p->certlen);
			tls_unload_file(p->key, p->keylen);
			free(p);
		}

		TAILQ_REMOVE(&hosts, h, vhosts);
		free(h);
	}

	memset(fcgi, 0, sizeof(fcgi));
}

static int
config_send_file(struct privsep *ps, int fd, int type)
{
	int	 n, m, id, d;

	id = PROC_SERVER;
	n = -1;
	proc_range(ps, id, &n, &m);
	for (n = 0; n < m; ++n) {
		if ((d = dup(fd)) == -1)
			fatal("dup");
		if (proc_compose_imsg(ps, id, n, type, -1, d, NULL, 0)
		    == -1)
			return -1;
	}

	close(fd);
	return 0;
}

static int
make_socket(int port, int family)
{
	int sock, v;
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	struct sockaddr *addr;
	socklen_t len;

	switch (family) {
	case AF_INET:
		memset(&addr4, 0, sizeof(addr4));
		addr4.sin_family = family;
		addr4.sin_port = htons(port);
		addr4.sin_addr.s_addr = INADDR_ANY;
		addr = (struct sockaddr*)&addr4;
		len = sizeof(addr4);
		break;

	case AF_INET6:
		memset(&addr6, 0, sizeof(addr6));
		addr6.sin6_family = AF_INET6;
		addr6.sin6_port = htons(port);
		addr6.sin6_addr = in6addr_any;
		addr = (struct sockaddr*)&addr6;
		len = sizeof(addr6);
		break;

	default:
		/* unreachable */
		abort();
	}

	if ((sock = socket(family, SOCK_STREAM, 0)) == -1)
		fatal("socket");

	v = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == -1)
		fatal("setsockopt(SO_REUSEADDR)");

	v = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &v, sizeof(v)) == -1)
		fatal("setsockopt(SO_REUSEPORT)");

	mark_nonblock(sock);

	if (bind(sock, addr, len) == -1)
		fatal("bind");

	if (listen(sock, 16) == -1)
		fatal("listen");

	return sock;
}

static int
config_send_socks(struct conf *conf)
{
	struct privsep	*ps = conf->ps;
	int		 sock;

	if ((sock = make_socket(conf->port, AF_INET)) == -1)
		return -1;

	if (config_send_file(ps, sock, IMSG_RECONF_SOCK4) == -1)
		return -1;

	if (!conf->ipv6)
		return 0;

	if ((sock = make_socket(conf->port, AF_INET6)) == -1)
		return -1;

	if (config_send_file(ps, sock, IMSG_RECONF_SOCK6) == -1)
		return -1;

	return 0;
}

int
config_send(struct conf *conf, struct fcgi *fcgi, struct vhosthead *hosts)
{
	struct privsep	*ps = conf->ps;
	struct etm	*m;
	struct vhost	*h;
	struct location	*l;
	struct proxy	*p;
	struct envlist	*e;
	struct alist	*a;
	size_t		 i;
	int		 fd;

	for (i = 0; i < conf->mime.len; ++i) {
		m = &conf->mime.t[i];
		if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_MIME,
		    m, sizeof(*m)) == -1)
			return -1;
	}

	if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_PROTOS,
	    &conf->protos, sizeof(conf->protos)) == -1)
		return -1;

	if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_PORT,
	    &conf->port, sizeof(conf->port)) == -1)
		return -1;

	if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
		return -1;

	if (config_send_socks(conf) == -1)
		return -1;

	if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
		return -1;

	for (i = 0; i < FCGI_MAX; ++i) {
		if (*fcgi[i].path == '\0')
			break;
		if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_FCGI,
		    &fcgi[i], sizeof(fcgi[i])) == -1)
			return -1;
	}

	TAILQ_FOREACH(h, hosts, vhosts) {
		log_debug("sending host %s", h->domain);

		if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_HOST,
		    h, sizeof(*h)) == -1)
			return -1;

		log_debug("sending certificate %s", h->cert_path);
		if ((fd = open(h->cert_path, O_RDONLY)) == -1)
			fatal("can't open %s", h->cert_path);
		if (config_send_file(ps, fd, IMSG_RECONF_CERT) == -1)
			return -1;

		log_debug("sending key %s", h->key_path);
		if ((fd = open(h->key_path, O_RDONLY)) == -1)
			fatal("can't open %s", h->key_path);
		if (config_send_file(ps, fd, IMSG_RECONF_KEY) == -1)
			return -1;

		if (*h->ocsp_path != '\0') {
			log_debug("sending ocsp %s", h->ocsp_path);
			if ((fd = open(h->ocsp_path, O_RDONLY)) == -1)
				fatal("can't open %s", h->ocsp_path);
			if (config_send_file(ps, fd, IMSG_RECONF_OCSP) == -1)
				return -1;
		}

		TAILQ_FOREACH(l, &h->locations, locations) {
			if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_LOC,
			    l, sizeof(*l)) == -1)
				return -1;
		}

		if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
			return -1;

		TAILQ_FOREACH(e, &h->params, envs) {
			if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_ENV,
			    e, sizeof(*e)) == -1)
				return -1;
		}

		if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
			return -1;

		TAILQ_FOREACH(a, &h->aliases, aliases) {
			if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_ALIAS,
			    a, sizeof(*a)) == -1)
				return -1;
		}

		if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
			return -1;

		TAILQ_FOREACH(p, &h->proxies, proxies) {
			if (proc_compose(ps, PROC_SERVER, IMSG_RECONF_PROXY,
			    p, sizeof(*p)) == -1)
				return -1;
		}

		if (proc_flush_imsg(ps, PROC_SERVER, -1) == -1)
			return -1;
	}

	return 0;
}

static int
load_file(int fd, uint8_t **data, size_t *len)
{
	struct stat	 sb;
	FILE		*fp;
	size_t		 r;

	if (fstat(fd, &sb) == -1)
		fatal("fstat");

	if ((fp = fdopen(fd, "r")) == NULL)
		fatal("fdopen");

	if (sb.st_size < 0 /* || sb.st_size > SIZE_MAX */) {
		log_warnx("file too large");
		fclose(fp);
		return -1;
	}
	*len = sb.st_size;

	if ((*data = malloc(*len)) == NULL)
		fatal("malloc");

	r = fread(*data, 1, *len, fp);
	if (r != *len) {
		log_warn("read");
		fclose(fp);
		free(*data);
		return -1;
	}

	fclose(fp);
	return 0;
}

int
config_recv(struct conf *conf, struct imsg *imsg)
{
	static struct vhost *h;
	struct privsep	*ps = conf->ps;
	struct etm	 m;
	struct fcgi	*f;
	struct vhost	*vh, vht;
	struct location	*loc;
	struct envlist	*env;
	struct alist	*alias;
	struct proxy	*proxy;
	size_t		 i, datalen;

	datalen = IMSG_DATA_SIZE(imsg);

	switch (imsg->hdr.type) {
	case IMSG_RECONF_START:
		config_free();
		h = NULL;
		break;

	case IMSG_RECONF_MIME:
		IMSG_SIZE_CHECK(imsg, &m);
		memcpy(&m, imsg->data, datalen);
		if (m.mime[sizeof(m.mime) - 1] != '\0' ||
		    m.ext[sizeof(m.ext) - 1] != '\0')
			fatal("received corrupted IMSG_RECONF_MIME");
		if (add_mime(&conf->mime, m.mime, m.ext) == -1)
			fatal("failed to add mime mapping %s -> %s",
			    m.mime, m.ext);
		break;

	case IMSG_RECONF_PROTOS:
		IMSG_SIZE_CHECK(imsg, &conf->protos);
		memcpy(&conf->protos, imsg->data, datalen);
		break;

	case IMSG_RECONF_PORT:
		IMSG_SIZE_CHECK(imsg, &conf->port);
		memcpy(&conf->port, imsg->data, datalen);
		break;

	case IMSG_RECONF_SOCK4:
		if (conf->sock4 != -1)
			fatalx("socket ipv4 already recv'd");
		if (imsg->fd == -1)
			fatalx("missing socket for IMSG_RECONF_SOCK4");
		conf->sock4 = imsg->fd;
		event_set(&conf->evsock4, conf->sock4, EV_READ|EV_PERSIST,
		    do_accept, NULL);
		break;

	case IMSG_RECONF_SOCK6:
		if (conf->sock6 != -1)
			fatalx("socket ipv6 already recv'd");
		if (imsg->fd == -1)
			fatalx("missing socket for IMSG_RECONF_SOCK6");
		conf->sock6 = imsg->fd;
		event_set(&conf->evsock6, conf->sock6, EV_READ|EV_PERSIST,
		    do_accept, NULL);
		break;

	case IMSG_RECONF_FCGI:
		for (i = 0; i < FCGI_MAX; ++i) {
			f = &fcgi[i];
			if (*f->path != '\0')
				continue;
			IMSG_SIZE_CHECK(imsg, f);
			memcpy(f, imsg->data, datalen);
			break;
		}
		if (i == FCGI_MAX)
			fatalx("recv too many fcgi");
		break;

	case IMSG_RECONF_HOST:
		IMSG_SIZE_CHECK(imsg, &vht);
		memcpy(&vht, imsg->data, datalen);
		vh = new_vhost();
		strlcpy(vh->domain, vht.domain, sizeof(vh->domain));
		h = vh;
		TAILQ_INSERT_TAIL(&hosts, h, vhosts);
		break;

	case IMSG_RECONF_CERT:
		log_debug("receiving cert");
		if (h == NULL)
			fatalx("recv'd cert without host");
		if (h->cert != NULL)
			fatalx("cert already received");
		if (imsg->fd == -1)
			fatalx("no fd for IMSG_RECONF_CERT");
		if (load_file(imsg->fd, &h->cert, &h->certlen) == -1)
			fatalx("failed to load cert for %s",
			    h->domain);
		break;

	case IMSG_RECONF_KEY:
		log_debug("receiving key");
		if (h == NULL)
			fatalx("recv'd key without host");
		if (h->key != NULL)
			fatalx("key already received");
		if (imsg->fd == -1)
			fatalx("no fd for IMSG_RECONF_KEY");
		if (load_file(imsg->fd, &h->key, &h->keylen) == -1)
			fatalx("failed to load key for %s",
			    h->domain);
		break;

	case IMSG_RECONF_OCSP:
		log_debug("receiving ocsp");
		if (h == NULL)
			fatalx("recv'd ocsp without host");
		if (h->ocsp != NULL)
			fatalx("ocsp already received");
		if (imsg->fd == -1)
			fatalx("no fd for IMSG_RECONF_OCSP");
		if (load_file(imsg->fd, &h->ocsp, &h->ocsplen) == -1)
			fatalx("failed to load ocsp for %s",
			    h->domain);
		break;

	case IMSG_RECONF_LOC:
		if (h == NULL)
			fatalx("recv'd location without host");
		IMSG_SIZE_CHECK(imsg, loc);

		//loc = new_location();
		loc = xcalloc(1, sizeof(*loc));
		loc->dirfd = -1;
		loc->fcgi = -1;

		memcpy(loc, imsg->data, datalen);
		loc->dirfd = -1; /* XXX */
		loc->reqca = NULL; /* XXX */
		TAILQ_INSERT_TAIL(&h->locations, loc, locations);
		break;

	case IMSG_RECONF_ENV:
		if (h == NULL)
			fatalx("recv'd env without host");
		IMSG_SIZE_CHECK(imsg, env);
		env = xcalloc(1, sizeof(*env));
		memcpy(env, imsg->data, datalen);
		TAILQ_INSERT_TAIL(&h->params, env, envs);
		break;

	case IMSG_RECONF_ALIAS:
		if (h == NULL)
			fatalx("recv'd alias without host");
		IMSG_SIZE_CHECK(imsg, alias);
		alias = xcalloc(1, sizeof(*alias));
		memcpy(alias, imsg->data, datalen);
		TAILQ_INSERT_TAIL(&h->aliases, alias, aliases);
		break;

	case IMSG_RECONF_PROXY:
		log_debug("receiving proxy");
		if (h == NULL)
			fatalx("recv'd proxy without host");
		IMSG_SIZE_CHECK(imsg, proxy);
		proxy = xcalloc(1, sizeof(*proxy));
		memcpy(proxy, imsg->data, datalen);
		proxy->reqca = NULL; /* XXX */
		proxy->cert = proxy->key = NULL; /* XXX */
		proxy->certlen = proxy->keylen = 0; /* XXX */
		TAILQ_INSERT_TAIL(&h->proxies, proxy, proxies);
		break;

	case IMSG_RECONF_END:
		if (proc_compose(ps, PROC_PARENT, IMSG_RECONF_DONE,
		    NULL, 0) == -1)
			return -1;
		break;

	default:
		return -1;
	}

	return 0;
}
