/*
 * libdpkg - Debian packaging suite library routines
 * fdio.h - safe file descriptor based input/output
 *
 * Copyright © 2009-2010 Guillem Jover <guillem@debian.org>
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

#ifndef LIBDPKG_FDIO_H
#define LIBDPKG_FDIO_H

#include <sys/types.h>

#include <dpkg/macros.h>

DPKG_BEGIN_DECLS

ssize_t fd_read(int fd, void *buf, size_t len);
ssize_t fd_write(int fd, const void *buf, size_t len);

DPKG_END_DECLS

#endif /* LIBDPKG_FDIO_H */
