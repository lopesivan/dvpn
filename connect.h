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

#ifndef __CONNECT_H
#define __CONNECT_H

#include <gnutls/x509.h>
#include <iv.h>
#include <netdb.h>
#include "iv_getaddrinfo.h"
#include "pconn.h"
#include "tun.h"

struct server_peer
{
	char			*tunitf;
	char			*name;
	char			*hostname;
	char			*port;
	gnutls_x509_privkey_t	key;
	uint8_t			fingerprint[20];
	int			is_peer;

	int			state;
	struct tun_interface	tun;
	struct iv_timer		rx_timeout;
	union {
		struct {
			struct addrinfo		hints;
			struct iv_getaddrinfo	addrinfo;
		};
		struct {
			struct addrinfo		*res;
			struct addrinfo		*rp;
			struct iv_fd		connectfd;
		};
		struct {
			struct pconn		pconn;
			struct iv_timer		keepalive_timer;
		};
	};
};

int server_peer_register(struct server_peer *sp);
void server_peer_unregister(struct server_peer *sp);


#endif
