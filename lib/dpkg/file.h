/*
 * libdpkg - Debian packaging suite library routines
 * file.h - file handling routines
 *
 * Copyright © 2008-2010 Guillem Jover <guillem@debian.org>
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

#ifndef LIBDPKG_FILE_H
#define LIBDPKG_FILE_H

#include <sys/types.h>

#include <stdbool.h>

#include <dpkg/macros.h>

DPKG_BEGIN_DECLS

struct file_stat {
	uid_t uid;
	gid_t gid;
	mode_t mode;
};

void file_copy_perms(const char *src, const char *dst);

enum file_lock_flags {
	FILE_LOCK_NOWAIT,
	FILE_LOCK_WAIT,
};

bool file_is_locked(int lockfd, const char *filename);
void file_lock(int *lockfd, enum file_lock_flags flags, const char *filename,
               const char *desc);
void file_unlock(int fd, const char *desc);

DPKG_END_DECLS

#endif /* LIBDPKG_FILE_H */
