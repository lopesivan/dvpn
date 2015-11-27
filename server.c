/*
 * dvpn, a multipoint vpn implementation
 * Copyright (C) 2015 Lennert Buytenhek
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
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <iv.h>
#include <iv_signal.h>
#include <netdb.h>
#include <string.h>
#include "itf.h"
#include "pconn.h"
#include "tun.h"
#include "x509.h"

struct client_conn
{
	int			state;
	struct tun_interface	tun;
	struct iv_timer		rx_timeout;
	struct pconn		pconn;
	struct iv_timer		keepalive_timer;
};

#define STATE_HANDSHAKE		1
#define STATE_CONNECTED		2

#define HANDSHAKE_TIMEOUT	10
#define KEEPALIVE_INTERVAL	30

static int serverport;
static const char *itfname;
static gnutls_x509_privkey_t key;

struct iv_fd listen_fd;
struct iv_signal sigint;

static void printhex(const uint8_t *a, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		printf("%.2x", a[i]);
		if (i < len - 1)
			printf(":");
	}
}

static void client_conn_kill(struct client_conn *cc)
{
	if (cc->state == STATE_CONNECTED)
		tun_interface_unregister(&cc->tun);

	if (iv_timer_registered(&cc->rx_timeout))
		iv_timer_unregister(&cc->rx_timeout);

	pconn_destroy(&cc->pconn);
	close(cc->pconn.fd);

	if (iv_timer_registered(&cc->keepalive_timer))
		iv_timer_unregister(&cc->keepalive_timer);

	free(cc);
}

static int verify_key_id(void *_cc, const uint8_t *id, int len)
{
	printf("key id: ");
	printhex(id, len);
	printf("\n");

	return 0;
}

static void handshake_done(void *_cc)
{
	struct client_conn *cc = _cc;
	uint8_t id[64];

	fprintf(stderr, "%p: handshake done\n", cc);

	if (tun_interface_register(&cc->tun) < 0) {
		client_conn_kill(cc);
		return;
	}

	cc->state = STATE_CONNECTED;

	iv_validate_now();

	iv_timer_unregister(&cc->rx_timeout);
	cc->rx_timeout.expires = iv_now;
	cc->rx_timeout.expires.tv_sec += 1.5 * KEEPALIVE_INTERVAL;
	iv_timer_register(&cc->rx_timeout);

	cc->keepalive_timer.expires = iv_now;
	cc->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	iv_timer_register(&cc->keepalive_timer);

	x509_get_key_id(id + 2, sizeof(id) - 2, key);

	id[0] = 0xfe;
	id[1] = 0x80;
	itf_add_v6(tun_interface_get_name(&cc->tun), id, 10);

	itf_set_state(tun_interface_get_name(&cc->tun), 1);
}

static void record_received(void *_cc, const uint8_t *rec, int len)
{
	struct client_conn *cc = _cc;
	int rlen;

	iv_validate_now();

	iv_timer_unregister(&cc->rx_timeout);
	cc->rx_timeout.expires = iv_now;
	cc->rx_timeout.expires.tv_sec += 1.5 * KEEPALIVE_INTERVAL;
	iv_timer_register(&cc->rx_timeout);

	if (len <= 2)
		return;

	rlen = (rec[0] << 8) | rec[1];
	if (rlen + 2 != len)
		return;

	if (tun_interface_send_packet(&cc->tun, rec + 2, rlen) < 0)
		client_conn_kill(cc);
}

static void connection_lost(void *_cc)
{
	struct client_conn *cc = _cc;

	fprintf(stderr, "%p: connection lost\n", cc);

	client_conn_kill(cc);
}

static void got_packet(void *_cc, uint8_t *buf, int len)
{
	struct client_conn *cc = _cc;
	uint8_t sndbuf[len + 2];

	iv_validate_now();

	iv_timer_unregister(&cc->keepalive_timer);
	cc->keepalive_timer.expires = iv_now;
	cc->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	iv_timer_register(&cc->keepalive_timer);

	sndbuf[0] = len >> 8;
	sndbuf[1] = len & 0xff;
	memcpy(sndbuf + 2, buf, len);

	if (pconn_record_send(&cc->pconn, sndbuf, len + 2))
		client_conn_kill(cc);
}

static void rx_timeout(void *_cc)
{
	struct client_conn *cc = _cc;

	fprintf(stderr, "%p: rx timeout\n", cc);

	client_conn_kill(cc);
}

static void send_keepalive(void *_cc)
{
	static uint8_t keepalive[] = { 0x00, 0x00 };
	struct client_conn *cc = _cc;

	fprintf(stderr, "%p: sending keepalive\n", cc);

	if (pconn_record_send(&cc->pconn, keepalive, 2)) {
		client_conn_kill(cc);
		return;
	}

	iv_validate_now();

	cc->keepalive_timer.expires = iv_now;
	cc->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	iv_timer_register(&cc->keepalive_timer);
}

static void got_connection(void *_dummy)
{
	struct sockaddr_in6 addr;
	socklen_t addrlen;
	int fd;
	struct client_conn *cc;

	addrlen = sizeof(addr);

	fd = accept(listen_fd.fd, (struct sockaddr *)&addr, &addrlen);
	if (fd < 0) {
		perror("accept");
		return;
	}

	cc = malloc(sizeof(*cc));
	if (cc == NULL) {
		close(fd);
		return;
	}

	cc->state = STATE_HANDSHAKE;

	cc->tun.itfname = itfname;
	cc->tun.cookie = cc;
	cc->tun.got_packet = got_packet;

	iv_validate_now();

	IV_TIMER_INIT(&cc->rx_timeout);
	cc->rx_timeout.expires = iv_now;
	cc->rx_timeout.expires.tv_sec += HANDSHAKE_TIMEOUT;
	cc->rx_timeout.cookie = cc;
	cc->rx_timeout.handler = rx_timeout;
	iv_timer_register(&cc->rx_timeout);

	cc->pconn.fd = fd;
	cc->pconn.role = PCONN_ROLE_SERVER;
	cc->pconn.key = key;
	cc->pconn.cookie = cc;
	cc->pconn.verify_key_id = verify_key_id;
	cc->pconn.handshake_done = handshake_done;
	cc->pconn.record_received = record_received;
	cc->pconn.connection_lost = connection_lost;

	IV_TIMER_INIT(&cc->keepalive_timer);
	cc->keepalive_timer.cookie = cc;
	cc->keepalive_timer.handler = send_keepalive;

	pconn_start(&cc->pconn);
}

static void got_sigint(void *_dummy)
{
	fprintf(stderr, "SIGINT received, shutting down\n");

	iv_fd_unregister(&listen_fd);
	close(listen_fd.fd);

	iv_signal_unregister(&sigint);
}

int main(void)
{
	int fd;
	struct sockaddr_in6 addr;
	int yes;

	gnutls_global_init();

	iv_init();

	serverport = 19275;
	itfname = "tap%d";
	if (x509_read_privkey(&key, "server.key") < 0)
		return 1;

	fd = socket(PF_INET6, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(serverport);
	addr.sin6_flowinfo = 0;
	addr.sin6_addr = in6addr_any;
	addr.sin6_scope_id = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	yes = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
		perror("setsockopt");
		return 1;
	}

	if (listen(fd, 100) < 0) {
		perror("listen");
		return 1;
	}

	IV_FD_INIT(&listen_fd);
	listen_fd.fd = fd;
	listen_fd.handler_in = got_connection;
	iv_fd_register(&listen_fd);

	IV_SIGNAL_INIT(&sigint);
	sigint.signum = SIGINT;
	sigint.flags = 0;
	sigint.cookie = NULL;
	sigint.handler = got_sigint;
	iv_signal_register(&sigint);

	iv_main();

	iv_deinit();

	gnutls_x509_privkey_deinit(key);

	gnutls_global_deinit();

	return 0;
}
