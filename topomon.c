/*
 * dvpn, a multipoint vpn implementation
 * Copyright (C) 2016 Lennert Buytenhek
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License version 2.1 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License version 2.1 along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <gnutls/x509.h>
#include <iv.h>
#include <iv_signal.h>
#include <string.h>
#include "adj_rib.h"
#include "conf.h"
#include "lsa.h"
#include "lsa_deserialise.h"
#include "x509.h"

struct qpeer {
	struct iv_list_head	list;
	uint8_t			id[32];
	struct iv_fd		query_fd;
	struct sockaddr_in6	query_addr;
	struct iv_timer		query_timer;
	struct adj_rib		*adj_rib_in;
	struct rib_listener	*debug_listener;
};

static struct iv_list_head qpeers;
static struct iv_signal sigint;

static void got_response(void *_qpeer)
{
	struct qpeer *qpeer = _qpeer;
	uint8_t buf[65536];
	struct sockaddr_storage recvaddr;
	socklen_t addrlen;
	int ret;
	struct lsa *newlsa;

	addrlen = sizeof(recvaddr);

	ret = recvfrom(qpeer->query_fd.fd, buf, sizeof(buf), 0,
			(struct sockaddr *)&recvaddr, &addrlen);
	if (ret < 0) {
		perror("recvfrom");
		return;
	}

	newlsa = lsa_deserialise(buf, ret);
	if (newlsa == NULL) {
		fprintf(stderr, "error deserialising LSA\n");
		adj_rib_flush(qpeer->adj_rib_in);
		return;
	}

	if (memcmp(qpeer->id, newlsa->id, 32)) {
		fprintf(stderr, "node ID mismatch\n");
		lsa_put(newlsa);
		return;
	}

	adj_rib_add_lsa(qpeer->adj_rib_in, newlsa);

	lsa_put(newlsa);
}

static void query_timer_expiry(void *_qpeer)
{
	struct qpeer *qpeer = _qpeer;
	uint8_t buf[1];

	qpeer->query_timer.expires.tv_nsec += 100000000;
	if (qpeer->query_timer.expires.tv_nsec >= 1000000000) {
		qpeer->query_timer.expires.tv_sec++;
		qpeer->query_timer.expires.tv_nsec -= 1000000000;
	}
	iv_timer_register(&qpeer->query_timer);

	if (sendto(qpeer->query_fd.fd, buf, 0, 0,
		   (struct sockaddr *)&qpeer->query_addr,
		   sizeof(qpeer->query_addr)) < 0) {
		perror("sendto");
		return;
	}
}

static void qpeer_add(uint8_t *id)
{
	int fd;
	struct qpeer *qpeer;
	uint8_t addr[16];
	uint8_t zeroid[32];
	char name[128];

	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		return;
	}

	qpeer = malloc(sizeof(*qpeer));
	if (qpeer == NULL) {
		close(fd);
		return;
	}

	iv_list_add_tail(&qpeer->list, &qpeers);

	memcpy(qpeer->id, id, 32);

	IV_FD_INIT(&qpeer->query_fd);
	qpeer->query_fd.fd = fd;
	qpeer->query_fd.cookie = qpeer;
	qpeer->query_fd.handler_in = got_response;
	iv_fd_register(&qpeer->query_fd);

	v6_global_addr_from_key_id(addr, id, 32);

	qpeer->query_addr.sin6_family = AF_INET6;
	qpeer->query_addr.sin6_port = htons(19275);
	qpeer->query_addr.sin6_flowinfo = 0;
	memcpy(&qpeer->query_addr.sin6_addr, addr, 16);
	qpeer->query_addr.sin6_scope_id = 0;

	IV_TIMER_INIT(&qpeer->query_timer);
	iv_validate_now();
	qpeer->query_timer.expires = iv_now;
	qpeer->query_timer.cookie = qpeer;
	qpeer->query_timer.handler = query_timer_expiry;
	iv_timer_register(&qpeer->query_timer);

	memset(zeroid, 0, 32);
	qpeer->adj_rib_in = adj_rib_alloc(zeroid, id);

	snprintf(name, sizeof(name), "adj-rib-in-%p", qpeer);

	qpeer->debug_listener = debug_listener_new(name);
	adj_rib_listener_register(qpeer->adj_rib_in, qpeer->debug_listener);
}

static void qpeer_add_config(const char *config)
{
	struct conf *conf;
	gnutls_x509_privkey_t key;
	uint8_t id[32];

	conf = parse_config(config);
	if (conf == NULL)
		return;

	if (x509_read_privkey(&key, conf->private_key) < 0)
		return;

	free_config(conf);

	x509_get_key_id(id, sizeof(id), key);

	gnutls_x509_privkey_deinit(key);

	qpeer_add(id);
}

static void qpeers_zap(void)
{
	while (!iv_list_empty(&qpeers)) {
		struct qpeer *qpeer;

		qpeer = iv_container_of(qpeers.next, struct qpeer, list);

		iv_list_del(&qpeer->list);
		iv_fd_unregister(&qpeer->query_fd);
		iv_timer_unregister(&qpeer->query_timer);
		adj_rib_free(qpeer->adj_rib_in);
		debug_listener_free(qpeer->debug_listener);
		free(qpeer);
	}
}

static void got_sigint(void *_dummy)
{
	fprintf(stderr, "SIGINT received, shutting down\n");

	qpeers_zap();

	iv_signal_unregister(&sigint);
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{ "config-file", required_argument, 0, 'c' },
		{ 0, 0, 0, 0, },
	};

	INIT_IV_LIST_HEAD(&qpeers);

	gnutls_global_init();

	iv_init();

	while (1) {
		int c;

		c = getopt_long(argc, argv, "c:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			qpeer_add_config(optarg);
			break;

		case '?':
			fprintf(stderr, "syntax: %s [-c <config.ini>]\n",
				argv[0]);
			return 1;

		default:
			abort();
		}
	}

	gnutls_global_deinit();

	IV_SIGNAL_INIT(&sigint);
	sigint.signum = SIGINT;
	sigint.flags = 0;
	sigint.cookie = NULL;
	sigint.handler = got_sigint;
	iv_signal_register(&sigint);

	iv_main();

	iv_deinit();

	return 0;
}