/*
 * libdpkg - Debian packaging suite library routines
 * options.h - option parsing functions
 *
 * Copyright © 1994,1995 Ian Jackson <ian@chiark.greenend.org.uk>
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

#ifndef LIBDPKG_OPTIONS_H
#define LIBDPKG_OPTIONS_H

#include <dpkg/macros.h>

DPKG_BEGIN_DECLS

typedef int action_func(const char *const *argv);

struct cmdinfo {
  const char *olong;
  char oshort;

  /*
   * 0 = Normal				(-o, --option)
   * 1 = Standard value			(-o=value, --option=value or
   *					 -o value, --option value)
   * 2 = Option string continued	(--option-value)
   */
  int takesvalue;
  int *iassignto;
  const char **sassignto;
  void (*call)(const struct cmdinfo*, const char *value);

  int arg_int;
  void *arg_ptr;

  action_func *action;
};

void badusage(const char *fmt, ...) DPKG_ATTR_NORET DPKG_ATTR_PRINTF(1);

#define MAX_CONFIG_LINE 1024

void myfileopt(const char* fn, const struct cmdinfo* cmdinfos);
void myopt(const char *const **argvp, const struct cmdinfo *cmdinfos,
           const char *help_str);
void loadcfgfile(const char *prog, const struct cmdinfo *cmdinfos);

/**
 * Current cmdinfo action.
 */
extern const struct cmdinfo *cipaction;

void setaction(const struct cmdinfo *cip, const char *value);
void setobsolete(const struct cmdinfo *cip, const char *value);

#define ACTION(longopt, shortopt, code, func) \
 { longopt, shortopt, 0, NULL, NULL, setaction, code, NULL, func }
#define OBSOLETE(longopt, shortopt) \
 { longopt, shortopt, 0, NULL, NULL, setobsolete, 0, NULL, NULL }

DPKG_END_DECLS

#endif /* LIBDPKG_OPTIONS_H */
