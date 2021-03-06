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

#ifndef __LSA_PRINT_H
#define __LSA_PRINT_H

#include "loc_rib.h"
#include "lsa.h"

void lsa_attr_print_type_name(FILE *fp, int parent_type, struct lsa_attr *attr);
int lsa_print_id_name(FILE *fp, uint8_t *id, struct loc_rib *name_hints);
void lsa_attr_print_key(FILE *fp, int parent_type, struct lsa_attr *attr,
			struct loc_rib *name_hints);
void lsa_attr_print_data(FILE *fp, int parent_type, struct lsa_attr *attr,
			 struct loc_rib *name_hints);
void lsa_print(FILE *fp, struct lsa *lsa, struct loc_rib *name_hints);


#endif
