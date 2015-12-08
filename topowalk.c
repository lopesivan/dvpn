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
#include <getopt.h>
#include <gnutls/x509.h>
#include <iv.h>
#include <iv_list.h>
#include <stdint.h>
#include <string.h>
#include "conf.h"
#include "cspf.h"
#include "spf.h"
#include "util.h"
#include "x509.h"

struct node
{
	struct iv_list_head	list;
	uint8_t			id[20];
	struct iv_list_head	edges;

	struct cspf_node	node;
};

struct edge
{
	struct iv_list_head	list;
	struct node		*to;
	enum cspf_edge_type	type;

	struct cspf_edge	edge;
};

static struct iv_list_head nodes;
static struct iv_list_head edges;

static struct node *find_node(uint8_t *id)
{
	struct iv_list_head *lh;
	struct node *n;

	iv_list_for_each (lh, &nodes) {
		n = iv_container_of(lh, struct node, list);
		if (!memcmp(n->id, id, 20))
			return n;
	}

	n = malloc(sizeof(*n));
	if (n == NULL)
		return NULL;

	iv_list_add_tail(&n->list, &nodes);
	memcpy(n->id, id, 20);
	INIT_IV_LIST_HEAD(&n->edges);

	return n;
}

static void
add_edge(struct node *from, struct node *to, enum cspf_edge_type type)
{
	struct edge *edge;

	edge = malloc(sizeof(*edge));
	if (edge == NULL)
		abort();

	iv_list_add_tail(&edge->list, &from->edges);
	edge->to = to;
	edge->type = type;
}

static void query_node(int fd, struct node *n)
{
	struct sockaddr_in6 addr;
	uint8_t buf[2048];
	int ret;
	socklen_t addrlen;
	int off;

	fprintf(stderr, "- ");
	printhex(stderr, n->id, 20);
	fprintf(stderr, "...");

	buf[0] = 0x20;
	buf[1] = 0x01;
	buf[2] = 0x00;
	buf[3] = 0x2f;
	memcpy(buf + 4, n->id + 4, 12);

	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons(19275);
	addr.sin6_flowinfo = 0;
	memcpy(&addr.sin6_addr, buf, 16);
	addr.sin6_scope_id = 0;

	ret = sendto(fd, buf, 0, 0, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		perror(" sendto");
		abort();
	}

	addrlen = sizeof(addr);

	ret = recvfrom(fd, buf, sizeof(buf), 0,
			(struct sockaddr *)&addr, &addrlen);
	if (ret < 0) {
		perror(" recvfrom");
		abort();
	}

	if (memcmp(n->id, buf, 20)) {
		fprintf(stderr, " node ID mismatch\n");
		return;
	}

	off = 20;
	while (off + 22 <= ret) {
		struct node *to;
		int type;

		to = find_node(buf + off);
		type = ntohs(*((uint16_t *)(buf + off + 20)));
		off += 22;

		if (type == 0)
			add_edge(n, to, EDGE_TYPE_EPEER);
		else if (type == 1)
			add_edge(n, to, EDGE_TYPE_CUSTOMER);
		else if (type == 2)
			add_edge(n, to, EDGE_TYPE_TRANSIT);
		else if (type == 3)
			add_edge(n, to, EDGE_TYPE_IPEER);
	}

	fprintf(stderr, " done\n");
}

static void scan(uint8_t *initial_id)
{
	int fd;
	struct iv_list_head *lh;

	INIT_IV_LIST_HEAD(&nodes);
	INIT_IV_LIST_HEAD(&edges);

	find_node(initial_id);

	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		abort();
	}

	fprintf(stderr, "querying nodes\n");
	iv_list_for_each (lh, &nodes) {
		struct node *n;

		n = iv_container_of(lh, struct node, list);
		query_node(fd, n);
	}
	fprintf(stderr, "\n");

	close(fd);
}

static void print_nodes(FILE *fp)
{
	struct iv_list_head *lh;

	iv_list_for_each (lh, &nodes) {
		struct node *n;
		struct iv_list_head *lh2;

		n = iv_container_of(lh, struct node, list);

		fprintf(fp, "node ");
		printhex(fp, n->id, 20);
		fprintf(fp, "\n");

		iv_list_for_each (lh2, &n->edges) {
			struct edge *edge;

			edge = iv_container_of(lh2, struct edge, list);

			fprintf(fp, "  => ");
			printhex(fp, edge->to->id, 20);
			fprintf(fp, " (%s)\n", cspf_edge_type_name(edge->type));
		}

		fprintf(fp, "\n");
	}
}

static struct edge *find_edge(struct node *from, struct node *to)
{
	struct iv_list_head *lh;

	iv_list_for_each (lh, &from->edges) {
		struct edge *edge;

		edge = iv_container_of(lh, struct edge, list);
		if (edge->to == to)
			return edge;
	}

	return NULL;
}

static int map_edge_type(int forward, int reverse)
{
	if (forward == EDGE_TYPE_IPEER && reverse == EDGE_TYPE_IPEER)
		return EDGE_TYPE_IPEER;

	if ((forward == EDGE_TYPE_CUSTOMER || forward == EDGE_TYPE_IPEER) &&
	    (reverse == EDGE_TYPE_TRANSIT || reverse == EDGE_TYPE_IPEER))
		return EDGE_TYPE_CUSTOMER;

	if ((forward == EDGE_TYPE_TRANSIT || forward == EDGE_TYPE_IPEER) &&
	    (reverse == EDGE_TYPE_CUSTOMER || reverse == EDGE_TYPE_IPEER))
		return EDGE_TYPE_TRANSIT;

	return EDGE_TYPE_EPEER;
}

static void prep_cspf(struct spf_context *spf)
{
	struct iv_list_head *lh;

	spf_init(spf);

	iv_list_for_each (lh, &nodes) {
		struct node *n;
		struct iv_list_head *lh2;

		n = iv_container_of(lh, struct node, list);

		n->node.cookie = n;
		cspf_node_add(spf, &n->node);

		iv_list_for_each (lh2, &n->edges) {
			struct edge *e;
			struct edge *rev;

			e = iv_container_of(lh2, struct edge, list);

			rev = find_edge(e->to, n);
			if (rev != NULL) {
				enum cspf_edge_type type;

				type = map_edge_type(e->type, rev->type);
				cspf_edge_add(spf, &e->edge, &n->node,
					      &e->to->node, type, 1);
			}
		}
	}
}

static void print_graphviz(FILE *fp)
{
	struct iv_list_head *lh;

	fprintf(fp, "digraph g {\n");
	fprintf(fp, "\trankdir = LR;\n");

	iv_list_for_each (lh, &nodes) {
		struct node *n;
		struct node *p;

		n = iv_list_entry(lh, struct node, list);

		fprintf(fp, "\t\"");
		printhex(fp, n->id, 20);
		fprintf(fp, "\" [ label = \"");
		printhex(fp, n->id, 20);
		fprintf(fp, "\\ncost: %d\", shape = \"record\" ];\n",
			cspf_node_cost(&n->node));

		p = cspf_node_parent(&n->node);
		if (p == NULL)
			continue;

		fprintf(fp, "\t\"");
		printhex(fp, p->id, 20);
		fprintf(fp, "\" -> \"");
		printhex(fp, n->id, 20);
		fprintf(fp, "\" [ label = \"%s, %d\" ];\n",
			cspf_edge_type_name(find_edge(p, n)->type),
			cspf_node_cost(&n->node) - cspf_node_cost(&p->node));
	}

	fprintf(fp, "}\n");
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{ "config-file", required_argument, 0, 'c' },
		{ 0, 0, 0, 0, },
	};
	const char *config = "/etc/dvpn.ini";
	struct conf *conf;
	gnutls_x509_privkey_t key;
	uint8_t id[20];
	struct spf_context spf;

	while (1) {
		int c;

		c = getopt_long(argc, argv, "c:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			config = optarg;
			break;

		case '?':
			fprintf(stderr, "syntax: %s [-c <config.ini>]\n",
				argv[0]);
			return 1;

		default:
			abort();
		}
	}

	conf = parse_config(config);
	if (conf == NULL)
		return 1;

	gnutls_global_init();

	if (x509_read_privkey(&key, conf->private_key) < 0)
		return 1;

	x509_get_key_id(id, sizeof(id), key);

	gnutls_x509_privkey_deinit(key);

	gnutls_global_deinit();

	free_config(conf);

	scan(id);
	print_nodes(stderr);

	prep_cspf(&spf);
	cspf_run(&spf, &find_node(id)->node);
	print_graphviz(stdout);

	return 0;
}