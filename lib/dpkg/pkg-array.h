/*
 * libdpkg - Debian packaging suite library routines
 * pkg-array.h - primitives for pkg array handling
 *
 * Copyright © 2009 Guillem Jover <guillem@debian.org>
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

#ifndef LIBDPKG_PKG_ARRAY_H
#define LIBDPKG_PKG_ARRAY_H

#include <dpkg/dpkg-db.h>
#include <dpkg/pkg.h>

DPKG_BEGIN_DECLS

/**
 * Holds an array of pointers to package data.
 */
struct pkg_array {
	int n_pkgs;
	struct pkginfo **pkgs;
};

void pkg_array_init_from_db(struct pkg_array *a);
void pkg_array_sort(struct pkg_array *a, pkg_sorter_func *pkg_sort);
void pkg_array_destroy(struct pkg_array *a);

DPKG_END_DECLS

#endif /* LIBDPKG_PKG_ARRAY_H */
