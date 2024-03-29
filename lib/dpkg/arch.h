/*
 * libdpkg - Debian packaging suite library routines
 * arch.h - architecture database functions
 *
 * Copyright © 2011 Linaro Limited
 * Copyright © 2011 Raphaël Hertzog <hertzog@debian.org>
 * Copyright © 2011-2012 Guillem Jover <guillem@debian.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBDPKG_ARCH_H
#define LIBDPKG_ARCH_H

#include <dpkg/macros.h>
#include <dpkg/varbuf.h>

DPKG_BEGIN_DECLS

enum dpkg_arch_type {
	arch_none,
	arch_empty,
	arch_illegal,
	arch_wildcard,
	arch_all,
	arch_native,
	arch_foreign,
	arch_unknown,
};

struct dpkg_arch {
	struct dpkg_arch *next;
	const char *name;
	enum dpkg_arch_type type;
};

const char *dpkg_arch_name_is_illegal(const char *name) DPKG_ATTR_NONNULL(1);
struct dpkg_arch *dpkg_arch_find(const char *name);
struct dpkg_arch *dpkg_arch_get(enum dpkg_arch_type type);
struct dpkg_arch *dpkg_arch_get_list(void);
void dpkg_arch_reset_list(void);

struct dpkg_arch *dpkg_arch_add(const char *name);
void dpkg_arch_unmark(struct dpkg_arch *arch);
void dpkg_arch_load_list(void);
void dpkg_arch_save_list(void);

void varbuf_add_archqual(struct varbuf *vb, const struct dpkg_arch *arch);

DPKG_END_DECLS

#endif /* LIBDPKG_ARCH_H */
