/*
 * libdpkg - Debian packaging suite library routines
 * error.h - error message reporting
 *
 * Copyright © 2011 Guillem Jover <guillem@debian.org>
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

#ifndef LIBDPKG_ERROR_H
#define LIBDPKG_ERROR_H

#include <dpkg/macros.h>

DPKG_BEGIN_DECLS

struct dpkg_error {
	enum dpkg_msg_type {
		DPKG_MSG_NONE,
		DPKG_MSG_WARN,
		DPKG_MSG_ERROR,
	} type;

	char *str;
};

int dpkg_put_warn(struct dpkg_error *err, const char *fmt, ...)
	DPKG_ATTR_PRINTF(2);
int dpkg_put_error(struct dpkg_error *err, const char *fmt, ...)
	DPKG_ATTR_PRINTF(2);
int dpkg_put_errno(struct dpkg_error *err, const char *fmt, ...)
	DPKG_ATTR_PRINTF(2);

void dpkg_error_destroy(struct dpkg_error *err);

DPKG_END_DECLS

#endif /* LIBDPKG_ERROR_H */
