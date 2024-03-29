/*
 * dpkg - main program for package management
 * filesdb.h - management of database of files installed on system
 *
 * Copyright © 1995 Ian Jackson <ian@chiark.greenend.org.uk>
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

#ifndef FILESDB_H
#define FILESDB_H

#include <dpkg/file.h>

/*
 * Data structure here is as follows:
 *
 * For each package we have a ‘struct fileinlist *’, the head of a list of
 * files in that package. They are in ‘forwards’ order. Each entry has a
 * pointer to the ‘struct filenamenode’.
 *
 * The struct filenamenodes are in a hash table, indexed by name.
 * (This hash table is not visible to callers.)
 *
 * Each filenamenode has a (possibly empty) list of ‘struct filepackage’,
 * giving a list of the packages listing that filename.
 *
 * When we read files contained info about a particular package we set the
 * ‘files’ member of the clientdata struct to the appropriate thing. When
 * not yet set the files pointer is made to point to ‘fileslist_uninited’
 * (this is available only internally, within filesdb.c - the published
 * interface is ensure_*_available).
 */

struct pkginfo;

/* Flags to findnamenode(). */
enum fnnflags {
    /* Do not need to copy filename. */
    fnn_nocopy = 000001,
    /* findnamenode may return NULL. */
    fnn_nonew = 000002,
};

struct filenamenode {
  struct filenamenode *next;
  const char *name;
  struct filepackages *packages;
  struct diversion *divert;

  /* We allow the administrator to override the owner, group and mode of
   * a file. If such an override is present we use that instead of the
   * stat information stored in the archive.
   *
   * This functionality used to be in the suidmanager package. */
  struct file_stat *statoverride;

  /*
   * Fields from here on are used by archives.c &c, and cleared by
   * filesdbinit.
   */

  /* Set to zero when a new node is created. */
  enum {
    /* In the newconffiles list. */
    fnnf_new_conff = 000001,
    /* In the new filesystem archive. */
    fnnf_new_inarchive = 000002,
    /* In the old package's conffiles list. */
    fnnf_old_conff = 000004,
    /* Obsolete conffile. */
    fnnf_obs_conff = 000100,
    /* Must remove from other packages' lists. */
    fnnf_elide_other_lists = 000010,
    /* >= 1 instance is a dir, cannot rename over. */
    fnnf_no_atomic_overwrite = 000020,
    /* New file has been placed on the disk. */
    fnnf_placed_on_disk = 000040,
    fnnf_deferred_fsync = 000200,
    fnnf_deferred_rename = 000400,
    /* Path being filtered. */
    fnnf_filtered = 001000,
  } flags;

  /* Valid iff this namenode is in the newconffiles list. */
  const char *oldhash;

  struct stat *filestat;
  struct trigfileint *trig_interested;
};

struct fileinlist {
  struct fileinlist *next;
  struct filenamenode *namenode;
};

/*
 * When we deal with an ‘overridden’ file, every package except the
 * overriding one is considered to contain the other file instead. Both
 * files have entries in the filesdb database, and they refer to each other
 * via these diversion structures.
 *
 * The contested filename's filenamenode has an diversion entry with
 * useinstead set to point to the redirected filename's filenamenode; the
 * redirected filenamenode has camefrom set to the contested filenamenode.
 * Both sides' diversion entries will have pkg set to the package (if any)
 * which is allowed to use the contended filename.
 *
 * Packages that contain either version of the file will all refer to the
 * contested filenamenode in their per-file package lists (both in core and
 * on disk). References are redirected to the other filenamenode's filename
 * where appropriate.
 */
struct diversion {
  struct filenamenode *useinstead;
  struct filenamenode *camefrom;
  struct pkgset *pkgset;

  /* The ‘contested’ halves are in this list for easy cleanup. */
  struct diversion *next;
};

const char *pkgadmindir(void);
const char *pkgadminfile(struct pkginfo *pkg, struct pkgbin *pkgbin,
                         const char *filetype);

struct filepackages_iterator;
struct filepackages_iterator *filepackages_iter_new(struct filenamenode *fnn);
struct pkginfo *filepackages_iter_next(struct filepackages_iterator *i);
void filepackages_iter_free(struct filepackages_iterator *i);

void filesdbinit(void);

struct fileiterator;
struct fileiterator *iterfilestart(void);
struct filenamenode *iterfilenext(struct fileiterator *i);
void iterfileend(struct fileiterator *i);

void ensure_package_clientdata(struct pkginfo *pkg);

void ensure_diversions(void);

uid_t statdb_parse_uid(const char *str);
gid_t statdb_parse_gid(const char *str);
mode_t statdb_parse_mode(const char *str);
void ensure_statoverrides(void);

#define LISTFILE           "list"

void ensure_packagefiles_available(struct pkginfo *pkg);
void ensure_allinstfiles_available(void);
void ensure_allinstfiles_available_quiet(void);
void note_must_reread_files_inpackage(struct pkginfo *pkg);
struct filenamenode *findnamenode(const char *filename, enum fnnflags flags);
void write_filelist_except(struct pkginfo *pkg, struct pkgbin *pkgbin,
                           struct fileinlist *list, enum fnnflags mask);

struct reversefilelistiter { struct fileinlist *todo; };

void reversefilelist_init(struct reversefilelistiter *iterptr, struct fileinlist *files);
struct filenamenode *reversefilelist_next(struct reversefilelistiter *iterptr);
void reversefilelist_abort(struct reversefilelistiter *iterptr);

#endif /* FILESDB_H */
