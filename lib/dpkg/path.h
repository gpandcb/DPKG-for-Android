/*
 * libdpkg - Debian packaging suite library routines
 * path.h - path handling routines
 *
 * Copyright © 2008-2011 Guillem Jover <guillem@debian.org>
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

#ifndef LIBDPKG_PATH_H
#define LIBDPKG_PATH_H

#include <stddef.h>

#include <dpkg/macros.h>

DPKG_BEGIN_DECLS

size_t path_trim_slash_slashdot(char *path);
const char *path_skip_slash_dotslash(const char *path);
const char *path_basename(const char *path);
char *path_quote_filename(char *dst, const char *src, size_t size);

char *path_make_temp_template(const char *suffix);

DPKG_END_DECLS

#endif /* LIBDPKG_PATH_H */
