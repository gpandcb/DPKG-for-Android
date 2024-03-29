/*
 * libdpkg - Debian packaging suite library routines
 * pkg-format.c - customizable package formatting
 *
 * Copyright © 2001 Wichert Akkerman <wakkerma@debian.org>
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

#ifndef LIBDPKG_PKG_FORMAT_H
#define LIBDPKG_PKG_FORMAT_H

#include <dpkg/macros.h>
#include <dpkg/dpkg-db.h>

DPKG_BEGIN_DECLS

struct pkg_format_node;

struct pkg_format_node *pkg_format_parse(const char *fmt);
void pkg_format_free(struct pkg_format_node *head);
void pkg_format_show(const struct pkg_format_node *head,
                     struct pkginfo *pkg, struct pkgbin *pif);

DPKG_END_DECLS

#endif /* LIBDPKG_PKG_FORMAT_H */
