/*
 * dpkg - main program for package management
 * archives.c - actions that process archive files, mainly unpack
 *
 * Copyright © 1994,1995 Ian Jackson <ian@chiark.greenend.org.uk>
 * Copyright © 2000 Wichert Akkerman <wakkerma@debian.org>
 * Copyright © 2007-2012 Guillem Jover <guillem@debian.org>
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

#include <config.h>
#include <compat.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <obstack.h>
#define obstack_chunk_alloc m_malloc
#define obstack_chunk_free free

#include <dpkg/i18n.h>
#include <dpkg/dpkg.h>
#include <dpkg/dpkg-db.h>
#include <dpkg/pkg.h>
#include <dpkg/path.h>
#include <dpkg/fdio.h>
#include <dpkg/buffer.h>
#include <dpkg/subproc.h>
#include <dpkg/command.h>
#include <dpkg/file.h>
#include <dpkg/tarfn.h>
#include <dpkg/options.h>
#include <dpkg/triglib.h>

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#endif

#include "filesdb.h"
#include "main.h"
#include "archives.h"
#include "filters.h"

static inline void
fd_writeback_init(int fd)
{
  /* Ignore the return code as it should be considered equivalent to an
   * asynchronous hint for the kernel, we are doing an fsync() later on
   * anyway. */
#if defined(SYNC_FILE_RANGE_WRITE)
  sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE);
#elif defined(HAVE_POSIX_FADVISE)
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
}

static struct obstack tar_obs;
static bool tarobs_init = false;

/**
 * Ensure the obstack is properly initialized.
 */
static void ensureobstackinit(void) {

  if (!tarobs_init) {
    obstack_init(&tar_obs);
    tarobs_init = true;
  }
}

/**
 * Destroy the obstack.
 */
static void destroyobstack(void) {
  if (tarobs_init) {
    obstack_free(&tar_obs, NULL);
    tarobs_init = false;
  }
}

/**
 * Check if a file or directory will save a package from disappearance.
 *
 * A package can only be saved by a file or directory which is part
 * only of itself - it must be neither part of the new package being
 * installed nor part of any 3rd package (this is important so that
 * shared directories don't stop packages from disappearing).
 */
bool
filesavespackage(struct fileinlist *file,
                 struct pkginfo *pkgtobesaved,
                 struct pkginfo *pkgbeinginstalled)
{
  struct filepackages_iterator *iter;
  struct pkgset *divpkgset;
  struct pkginfo *thirdpkg;

  debug(dbg_eachfiledetail,"filesavespackage file `%s' package %s",
        file->namenode->name, pkg_name(pkgtobesaved, pnaw_nonambig));

  /* If the file is a contended one and it's overridden by either
   * the package we're considering disappearing or the package
   * we're installing then they're not actually the same file, so
   * we can't disappear the package - it is saved by this file. */
  if (file->namenode->divert && file->namenode->divert->useinstead) {
    divpkgset = file->namenode->divert->pkgset;
    if (divpkgset == pkgtobesaved->set || divpkgset == pkgbeinginstalled->set) {
      debug(dbg_eachfiledetail,"filesavespackage ... diverted -- save!");
      return true;
    }
  }
  /* Is the file in the package being installed? If so then it can't save. */
  if (file->namenode->flags & fnnf_new_inarchive) {
    debug(dbg_eachfiledetail,"filesavespackage ... in new archive -- no save");
    return false;
  }
  /* Look for a 3rd package which can take over the file (in case
   * it's a directory which is shared by many packages. */
  iter = filepackages_iter_new(file->namenode);
  while ((thirdpkg = filepackages_iter_next(iter))) {
    debug(dbg_eachfiledetail, "filesavespackage ... also in %s",
          pkg_name(thirdpkg, pnaw_nonambig));

    /* Is this not the package being installed or the one being
     * checked for disappearance? */
    if (thirdpkg == pkgbeinginstalled || thirdpkg == pkgtobesaved)
      continue;

    /* If !fileslistvalid then we've already disappeared this one, so
     * we shouldn't try to make it take over this shared directory. */
    debug(dbg_eachfiledetail,"filesavespackage ...  is 3rd package");

    if (!thirdpkg->clientdata->fileslistvalid) {
      debug(dbg_eachfiledetail, "process_archive ... already disappeared!");
      continue;
    }

    /* We've found a package that can take this file. */
    debug(dbg_eachfiledetail, "filesavespackage ...  taken -- no save");
    return false;
  }
  filepackages_iter_free(iter);

  debug(dbg_eachfiledetail, "filesavespackage ... not taken -- save !");
  return true;
}

void cu_pathname(int argc, void **argv) {
  ensure_pathname_nonexisting((char*)(argv[0]));
}

int tarfileread(void *ud, char *buf, int len) {
  struct tarcontext *tc= (struct tarcontext*)ud;
  int r;

  r = fd_read(tc->backendpipe, buf, len);
  if (r < 0)
    ohshite(_("error reading from dpkg-deb pipe"));
  return r;
}

static void
tarfile_skip_one_forward(struct tarcontext *tc, struct tar_entry *ti)
{
  size_t r;
  char databuf[TARBLKSZ];

  /* We need to advance the tar file to the next object, so read the
   * file data and set it to oblivion. */
  if (ti->type == tar_filetype_file) {
    char fnamebuf[256];

    fd_skip(tc->backendpipe, ti->size,
            _("skipped unpacking file '%.255s' (replaced or excluded?)"),
            path_quote_filename(fnamebuf, ti->name, 256));
    r = ti->size % TARBLKSZ;
    if (r > 0)
      if (fd_read(tc->backendpipe, databuf, TARBLKSZ - r) < 0)
        ohshite(_("error reading from dpkg-deb pipe"));
  }
}

int fnameidlu;
struct varbuf fnamevb;
struct varbuf fnametmpvb;
struct varbuf fnamenewvb;
struct pkg_deconf_list *deconfigure = NULL;

static time_t currenttime;

static int
does_replace(struct pkginfo *newpigp, struct pkgbin *newpifp,
             struct pkginfo *oldpigp, struct pkgbin *oldpifp)
{
  struct dependency *dep;

  debug(dbg_depcon,"does_replace new=%s old=%s (%s)",
        pkgbin_name(newpigp, newpifp, pnaw_nonambig),
        pkgbin_name(oldpigp, oldpifp, pnaw_nonambig),
        versiondescribe(&oldpifp->version, vdew_always));
  for (dep= newpifp->depends; dep; dep= dep->next) {
    if (dep->type != dep_replaces || dep->list->ed != oldpigp->set)
      continue;
    debug(dbg_depcondetail,"does_replace ... found old, version %s",
          versiondescribe(&dep->list->version,vdew_always));
    if (!versionsatisfied(oldpifp, dep->list))
      continue;
    debug(dbg_depcon,"does_replace ... yes");
    return true;
  }
  debug(dbg_depcon,"does_replace ... no");
  return false;
}

static void
tarobject_set_mtime(struct tar_entry *te, const char *path)
{
  struct timeval tv[2];

  tv[0].tv_sec = currenttime;
  tv[0].tv_usec = 0;
  tv[1].tv_sec = te->mtime;
  tv[1].tv_usec = 0;

  if (te->type == tar_filetype_symlink) {
#ifdef HAVE_LUTIMES
    if (lutimes(path, tv) && errno != ENOSYS)
      ohshite(_("error setting timestamps of `%.255s'"), path);
#endif
  } else {
    if (utimes(path, tv))
      ohshite(_("error setting timestamps of `%.255s'"), path);
  }
}

static void
tarobject_set_perms(struct tar_entry *te, const char *path, struct file_stat *st)
{
  if (te->type == tar_filetype_file)
    return; /* Already handled using the file descriptor. */

  if (te->type == tar_filetype_symlink) {
    if (lchown(path, st->uid, st->gid))
      ohshite(_("error setting ownership of symlink `%.255s'"), path);
  } else {
    if (chown(path, st->uid, st->gid))
      ohshite(_("error setting ownership of `%.255s'"), path);
    if (chmod(path, st->mode & ~S_IFMT))
      ohshite(_("error setting permissions of `%.255s'"), path);
  }
}

static void
tarobject_set_se_context(const char *matchpath, const char *path, mode_t mode)
{
#ifdef WITH_SELINUX
  static int selinux_enabled = -1;
  security_context_t scontext = NULL;
  int ret;

  /* If there's no file type, just give up. */
  if ((mode & S_IFMT) == 0)
    return;

  /* Set selinux_enabled if it is not already set (singleton). */
  if (selinux_enabled < 0)
    selinux_enabled = (is_selinux_enabled() > 0);

  /* If SE Linux is not enabled just do nothing. */
  if (!selinux_enabled)
    return;

  /* XXX: Well, we could use set_matchpathcon_printf() to redirect the
   * errors from the following bit, but that seems too much effort. */

  /* Do nothing if we can't figure out what the context is, or if it has
   * no context; in which case the default context shall be applied. */
  ret = matchpathcon(matchpath, mode & S_IFMT, &scontext);
  if (ret == -1 || (ret == 0 && scontext == NULL))
    return;

  if (strcmp(scontext, "<<none>>") != 0) {
    if (lsetfilecon(path, scontext) < 0)
      /* XXX: This might need to be fatal instead!? */
      perror("Error setting security context for next file object:");
  }

  freecon(scontext);
#endif /* WITH_SELINUX */
}

void setupfnamevbs(const char *filename) {
  varbuf_trunc(&fnamevb, fnameidlu);
  varbuf_add_str(&fnamevb, filename);
  varbuf_end_str(&fnamevb);

  varbuf_trunc(&fnametmpvb, fnameidlu);
  varbuf_add_str(&fnametmpvb, filename);
  varbuf_add_str(&fnametmpvb, DPKGTEMPEXT);
  varbuf_end_str(&fnametmpvb);

  varbuf_trunc(&fnamenewvb, fnameidlu);
  varbuf_add_str(&fnamenewvb, filename);
  varbuf_add_str(&fnamenewvb, DPKGNEWEXT);
  varbuf_end_str(&fnamenewvb);

  debug(dbg_eachfiledetail, "setupvnamevbs main=`%s' tmp=`%s' new=`%s'",
        fnamevb.buf, fnametmpvb.buf, fnamenewvb.buf);
}

/**
 * Securely remove a pathname.
 *
 * This is a secure version of remove(3) using secure_unlink() instead of
 * unlink(2).
 *
 * @retval  0 On success.
 * @retval -1 On failure, just like unlink(2) & rmdir(2).
 */
int
secure_remove(const char *filename)
{
  int r, e;

  if (!rmdir(filename)) {
    debug(dbg_eachfiledetail, "secure_remove '%s' rmdir OK", filename);
    return 0;
  }

  if (errno != ENOTDIR) {
    e= errno;
    debug(dbg_eachfiledetail, "secure_remove '%s' rmdir %s", filename,
          strerror(e));
    errno= e; return -1;
  }

  r = secure_unlink(filename);
  e = errno;
  debug(dbg_eachfiledetail, "secure_remove '%s' unlink %s",
        filename, r ? strerror(e) : "OK");
  errno= e; return r;
}

struct fileinlist *addfiletolist(struct tarcontext *tc,
				 struct filenamenode *namenode) {
  struct fileinlist *nifd;

  nifd= obstack_alloc(&tar_obs, sizeof(struct fileinlist));
  nifd->namenode= namenode;
  nifd->next = NULL;
  *tc->newfilesp = nifd;
  tc->newfilesp = &nifd->next;
  return nifd;
}

static void
remove_file_from_list(struct tarcontext *tc, struct tar_entry *ti,
                      struct fileinlist **oldnifd,
                      struct fileinlist *nifd)
{
  obstack_free(&tar_obs, nifd);
  tc->newfilesp = oldnifd;
  *oldnifd = NULL;
}

static bool
linktosameexistingdir(const struct tar_entry *ti, const char *fname,
                      struct varbuf *symlinkfn)
{
  struct stat oldstab, newstab;
  int statr;
  const char *lastslash;

  statr= stat(fname, &oldstab);
  if (statr) {
    if (!(errno == ENOENT || errno == ELOOP || errno == ENOTDIR))
      ohshite(_("failed to stat (dereference) existing symlink `%.250s'"),
              fname);
    return false;
  }
  if (!S_ISDIR(oldstab.st_mode))
    return false;

  /* But is it to the same dir? */
  varbuf_reset(symlinkfn);
  if (ti->linkname[0] == '/') {
    varbuf_add_str(symlinkfn, instdir);
  } else {
    lastslash= strrchr(fname, '/');
    assert(lastslash);
    varbuf_add_buf(symlinkfn, fname, (lastslash - fname) + 1);
  }
  varbuf_add_str(symlinkfn, ti->linkname);
  varbuf_end_str(symlinkfn);

  statr= stat(symlinkfn->buf, &newstab);
  if (statr) {
    if (!(errno == ENOENT || errno == ELOOP || errno == ENOTDIR))
      ohshite(_("failed to stat (dereference) proposed new symlink target"
                " `%.250s' for symlink `%.250s'"), symlinkfn->buf, fname);
    return false;
  }
  if (!S_ISDIR(newstab.st_mode))
    return false;
  if (newstab.st_dev != oldstab.st_dev ||
      newstab.st_ino != oldstab.st_ino)
    return false;
  return true;
}

int
tarobject(void *ctx, struct tar_entry *ti)
{
  static struct varbuf conffderefn, hardlinkfn, symlinkfn;
  static int fd;
  const char *usename;
  struct filenamenode *usenode;
  struct filenamenode *linknode;

  struct conffile *conff;
  struct tarcontext *tc = ctx;
  bool existingdir, keepexisting;
  int statr;
  ssize_t r;
  struct stat stab, stabtmp;
  char databuf[TARBLKSZ];
  struct file_stat *st;
  struct fileinlist *nifd, **oldnifd;
  struct pkgset *divpkgset;
  struct pkginfo *otherpkg;

  ensureobstackinit();

  /* Append to list of files.
   * The trailing ‘/’ put on the end of names in tarfiles has already
   * been stripped by tar_extractor(). */
  oldnifd= tc->newfilesp;
  nifd= addfiletolist(tc, findnamenode(ti->name, 0));
  nifd->namenode->flags |= fnnf_new_inarchive;

  debug(dbg_eachfile,
        "tarobject ti->name='%s' mode=%lo owner=%u.%u type=%d(%c)"
        " ti->linkname='%s' namenode='%s' flags=%o instead='%s'",
        ti->name, (long)ti->stat.mode,
        (unsigned)ti->stat.uid, (unsigned)ti->stat.gid,
        ti->type,
        ti->type >= '0' && ti->type <= '6' ? "-hlcbdp"[ti->type - '0'] : '?',
        ti->linkname,
        nifd->namenode->name, nifd->namenode->flags,
        nifd->namenode->divert && nifd->namenode->divert->useinstead
        ? nifd->namenode->divert->useinstead->name : "<none>");

  if (nifd->namenode->divert && nifd->namenode->divert->camefrom) {
    divpkgset = nifd->namenode->divert->pkgset;

    if (divpkgset) {
      forcibleerr(fc_overwritediverted,
                  _("trying to overwrite `%.250s', which is the "
                    "diverted version of `%.250s' (package: %.100s)"),
                  nifd->namenode->name, nifd->namenode->divert->camefrom->name,
                  divpkgset->name);
    } else {
      forcibleerr(fc_overwritediverted,
                  _("trying to overwrite `%.250s', which is the "
                    "diverted version of `%.250s'"),
                  nifd->namenode->name, nifd->namenode->divert->camefrom->name);
    }
  }

  if (nifd->namenode->statoverride)
    st = nifd->namenode->statoverride;
  else
    st = &ti->stat;

  usenode = namenodetouse(nifd->namenode, tc->pkg, &tc->pkg->available);
  usename = usenode->name + 1; /* Skip the leading '/'. */

  trig_file_activate(usenode, tc->pkg);

  if (nifd->namenode->flags & fnnf_new_conff) {
    /* If it's a conffile we have to extract it next to the installed
     * version (i.e. we do the usual link-following). */
    if (conffderef(tc->pkg, &conffderefn, usename))
      usename= conffderefn.buf;
    debug(dbg_conff,"tarobject fnnf_new_conff deref=`%s'",usename);
  }

  setupfnamevbs(usename);

  statr= lstat(fnamevb.buf,&stab);
  if (statr) {
    /* The lstat failed. */
    if (errno != ENOENT && errno != ENOTDIR)
      ohshite(_("unable to stat `%.255s' (which I was about to install)"),
              ti->name);
    /* OK, so it doesn't exist.
     * However, it's possible that we were in the middle of some other
     * backup/restore operation and were rudely interrupted.
     * So, we see if we have .dpkg-tmp, and if so we restore it. */
    if (rename(fnametmpvb.buf,fnamevb.buf)) {
      if (errno != ENOENT && errno != ENOTDIR)
        ohshite(_("unable to clean up mess surrounding `%.255s' before "
                  "installing another version"), ti->name);
      debug(dbg_eachfiledetail,"tarobject nonexistent");
    } else {
      debug(dbg_eachfiledetail,"tarobject restored tmp to main");
      statr= lstat(fnamevb.buf,&stab);
      if (statr) ohshite(_("unable to stat restored `%.255s' before installing"
                           " another version"), ti->name);
    }
  } else {
    debug(dbg_eachfiledetail,"tarobject already exists");
  }

  /* Check to see if it's a directory or link to one and we don't need to
   * do anything. This has to be done now so that we don't die due to
   * a file overwriting conflict. */
  existingdir = false;
  switch (ti->type) {
  case tar_filetype_symlink:
    /* If it's already an existing directory, do nothing. */
    if (!statr && S_ISDIR(stab.st_mode)) {
      debug(dbg_eachfiledetail, "tarobject symlink exists as directory");
      existingdir = true;
    } else if (!statr && S_ISLNK(stab.st_mode)) {
      if (linktosameexistingdir(ti, fnamevb.buf, &symlinkfn))
        existingdir = true;
    }
    break;
  case tar_filetype_dir:
    /* If it's already an existing directory, do nothing. */
    if (!stat(fnamevb.buf,&stabtmp) && S_ISDIR(stabtmp.st_mode)) {
      debug(dbg_eachfiledetail, "tarobject directory exists");
      existingdir = true;
    }
    break;
  case tar_filetype_file:
  case tar_filetype_chardev:
  case tar_filetype_blockdev:
  case tar_filetype_fifo:
  case tar_filetype_hardlink:
    break;
  default:
    ohshit(_("archive contained object `%.255s' of unknown type 0x%x"),
           ti->name, ti->type);
  }

  keepexisting = false;
  if (!existingdir) {
    struct filepackages_iterator *iter;

    iter = filepackages_iter_new(nifd->namenode);
    while ((otherpkg = filepackages_iter_next(iter))) {
      if (otherpkg == tc->pkg)
        continue;
      debug(dbg_eachfile, "tarobject ... found in %s",
            pkg_name(otherpkg, pnaw_nonambig));

      if (nifd->namenode->divert && nifd->namenode->divert->useinstead) {
        /* Right, so we may be diverting this file. This makes the conflict
         * OK iff one of us is the diverting package (we don't need to
         * check for both being the diverting package, obviously). */
        divpkgset = nifd->namenode->divert->pkgset;
        debug(dbg_eachfile, "tarobject ... diverted, divpkgset=%s",
              divpkgset ? divpkgset->name : "<none>");
        if (otherpkg->set == divpkgset || tc->pkg->set == divpkgset)
          continue;
      }

      /* If the new object is a directory and the previous object does
       * not exist assume it's also a directory and skip further checks.
       * XXX: Ideally with more information about the installed files we
       * could perform more clever checks. */
      if (statr != 0 && ti->type == tar_filetype_dir) {
        debug(dbg_eachfile, "tarobject ... assuming shared directory");
        continue;
      }

      /* Nope? Hmm, file conflict, perhaps. Check Replaces. */
      switch (otherpkg->clientdata->replacingfilesandsaid) {
      case 2:
        keepexisting = true;
      case 1:
        continue;
      }

      /* Is the package with the conflicting file in the “config files only”
       * state? If so it must be a config file and we can silenty take it
       * over. */
      if (otherpkg->status == stat_configfiles)
        continue;

      /* Perhaps we're removing a conflicting package? */
      if (otherpkg->clientdata->istobe == itb_remove)
        continue;

      /* Is the file an obsolete conffile in the other package
       * and a conffile in the new package? */
      if ((nifd->namenode->flags & fnnf_new_conff) &&
          !statr && S_ISREG(stab.st_mode)) {
        for (conff = otherpkg->installed.conffiles;
             conff;
             conff = conff->next) {
          if (!conff->obsolete)
            continue;
          if (stat(conff->name, &stabtmp)) {
            if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP)
              continue;
            else
              ohshite(_("cannot stat file '%s'"), conff->name);
          }
          if (stabtmp.st_dev == stab.st_dev &&
              stabtmp.st_ino == stab.st_ino)
            break;
        }
        if (conff) {
          debug(dbg_eachfiledetail, "tarobject other's obsolete conffile");
          /* process_archive() will have copied its hash already. */
          continue;
        }
      }

      if (does_replace(tc->pkg, &tc->pkg->available,
                       otherpkg, &otherpkg->installed)) {
        printf(_("Replacing files in old package %s ...\n"),
               pkg_name(otherpkg, pnaw_nonambig));
        otherpkg->clientdata->replacingfilesandsaid = 1;
      } else if (does_replace(otherpkg, &otherpkg->installed,
                              tc->pkg, &tc->pkg->available)) {
        printf(_("Replaced by files in installed package %s ...\n"),
               pkg_name(otherpkg, pnaw_nonambig));
        otherpkg->clientdata->replacingfilesandsaid = 2;
        nifd->namenode->flags &= ~fnnf_new_inarchive;
        keepexisting = true;
      } else {
        /* At this point we are replacing something without a Replaces. */
        if (!statr && S_ISDIR(stab.st_mode)) {
          forcibleerr(fc_overwritedir,
                      _("trying to overwrite directory '%.250s' "
                        "in package %.250s %.250s with nondirectory"),
                      nifd->namenode->name, pkg_name(otherpkg, pnaw_nonambig),
                      versiondescribe(&otherpkg->installed.version,
                                      vdew_nonambig));
        } else {
          forcibleerr(fc_overwrite,
                      _("trying to overwrite '%.250s', "
                        "which is also in package %.250s %.250s"),
                      nifd->namenode->name, pkg_name(otherpkg, pnaw_nonambig),
                      versiondescribe(&otherpkg->installed.version,
                                      vdew_nonambig));
        }
      }
    }
    filepackages_iter_free(iter);
  }

  if (keepexisting) {
    remove_file_from_list(tc, ti, oldnifd, nifd);
    tarfile_skip_one_forward(tc, ti);
    return 0;
  }

  if (filter_should_skip(ti)) {
    nifd->namenode->flags &= ~fnnf_new_inarchive;
    nifd->namenode->flags |= fnnf_filtered;
    tarfile_skip_one_forward(tc, ti);

    return 0;
  }

  if (existingdir)
    return 0;

  /* Now, at this stage we want to make sure neither of .dpkg-new and
   * .dpkg-tmp are hanging around. */
  ensure_pathname_nonexisting(fnamenewvb.buf);
  ensure_pathname_nonexisting(fnametmpvb.buf);

  /* Now we start to do things that we need to be able to undo
   * if something goes wrong. Watch out for the CLEANUP comments to
   * keep an eye on what's installed on the disk at each point. */
  push_cleanup(cu_installnew, ~ehflag_normaltidy, NULL, 0, 1, (void *)nifd);

  /*
   * CLEANUP: Now we either have the old file on the disk, or not, in
   * its original filename.
   */

  /* Extract whatever it is as .dpkg-new ... */
  switch (ti->type) {
  case tar_filetype_file:
    /* We create the file with mode 0 to make sure nobody can do anything with
     * it until we apply the proper mode, which might be a statoverride. */
    fd= open(fnamenewvb.buf, (O_CREAT|O_EXCL|O_WRONLY), 0);
    if (fd < 0)
      ohshite(_("unable to create `%.255s' (while processing `%.255s')"),
              fnamenewvb.buf, ti->name);
    push_cleanup(cu_closefd, ehflag_bombout, NULL, 0, 1, &fd);
    debug(dbg_eachfiledetail, "tarobject file open size=%jd",
          (intmax_t)ti->size);
    { char fnamebuf[256];
    fd_fd_copy(tc->backendpipe, fd, ti->size,
               _("backend dpkg-deb during `%.255s'"),
               path_quote_filename(fnamebuf, ti->name, 256));
    }
    r = ti->size % TARBLKSZ;
    if (r > 0)
      if (fd_read(tc->backendpipe, databuf, TARBLKSZ - r) < 0)
        ohshite(_("error reading from dpkg-deb pipe"));

    fd_writeback_init(fd);

    if (nifd->namenode->statoverride)
      debug(dbg_eachfile, "tarobject ... stat override, uid=%d, gid=%d, mode=%04o",
			  nifd->namenode->statoverride->uid,
			  nifd->namenode->statoverride->gid,
			  nifd->namenode->statoverride->mode);
    if (fchown(fd, st->uid, st->gid))
      ohshite(_("error setting ownership of `%.255s'"), ti->name);
    if (fchmod(fd, st->mode & ~S_IFMT))
      ohshite(_("error setting permissions of `%.255s'"), ti->name);

    /* Postpone the fsync, to try to avoid massive I/O degradation. */
    if (!fc_unsafe_io)
      nifd->namenode->flags |= fnnf_deferred_fsync;

    pop_cleanup(ehflag_normaltidy); /* fd = open(fnamenewvb.buf) */
    if (close(fd))
      ohshite(_("error closing/writing `%.255s'"), ti->name);
    break;
  case tar_filetype_fifo:
    if (mkfifo(fnamenewvb.buf,0))
      ohshite(_("error creating pipe `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject fifo");
    break;
  case tar_filetype_chardev:
    if (mknod(fnamenewvb.buf, S_IFCHR, ti->dev))
      ohshite(_("error creating device `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject chardev");
    break;
  case tar_filetype_blockdev:
    if (mknod(fnamenewvb.buf, S_IFBLK, ti->dev))
      ohshite(_("error creating device `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject blockdev");
    break;
  case tar_filetype_hardlink:
    varbuf_reset(&hardlinkfn);
    varbuf_add_str(&hardlinkfn, instdir);
    varbuf_add_char(&hardlinkfn, '/');
    linknode = findnamenode(ti->linkname, 0);
    varbuf_add_str(&hardlinkfn,
                   namenodetouse(linknode, tc->pkg, &tc->pkg->available)->name);
    if (linknode->flags & (fnnf_deferred_rename|fnnf_new_conff))
      varbuf_add_str(&hardlinkfn, DPKGNEWEXT);
    varbuf_end_str(&hardlinkfn);
    if (link(hardlinkfn.buf,fnamenewvb.buf))
      ohshite(_("error creating hard link `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject hardlink");
    break;
  case tar_filetype_symlink:
    /* We've already checked for an existing directory. */
    if (symlink(ti->linkname, fnamenewvb.buf))
      ohshite(_("error creating symbolic link `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject symlink creating");
    break;
  case tar_filetype_dir:
    /* We've already checked for an existing directory. */
    if (mkdir(fnamenewvb.buf,0))
      ohshite(_("error creating directory `%.255s'"), ti->name);
    debug(dbg_eachfiledetail, "tarobject directory creating");
    break;
  default:
    internerr("unknown tar type '%d', but already checked", ti->type);
  }

  tarobject_set_perms(ti, fnamenewvb.buf, st);
  tarobject_set_mtime(ti, fnamenewvb.buf);
  tarobject_set_se_context(fnamevb.buf, fnamenewvb.buf, st->mode);

  /*
   * CLEANUP: Now we have extracted the new object in .dpkg-new (or,
   * if the file already exists as a directory and we were trying to
   * extract a directory or symlink, we returned earlier, so we don't
   * need to worry about that here).
   *
   * The old file is still in the original filename,
   */

  /* First, check to see if it's a conffile. If so we don't install
   * it now - we leave it in .dpkg-new for --configure to take care of. */
  if (nifd->namenode->flags & fnnf_new_conff) {
    debug(dbg_conffdetail,"tarobject conffile extracted");
    nifd->namenode->flags |= fnnf_elide_other_lists;
    return 0;
  }

  /* Now we move the old file out of the way, the backup file will
   * be deleted later. */
  if (statr) {
    /* Don't try to back it up if it didn't exist. */
    debug(dbg_eachfiledetail,"tarobject new - no backup");
  } else {
    if (ti->type == tar_filetype_dir || S_ISDIR(stab.st_mode)) {
      /* One of the two is a directory - can't do atomic install. */
      debug(dbg_eachfiledetail,"tarobject directory, nonatomic");
      nifd->namenode->flags |= fnnf_no_atomic_overwrite;
      if (rename(fnamevb.buf,fnametmpvb.buf))
        ohshite(_("unable to move aside `%.255s' to install new version"),
                ti->name);
    } else if (S_ISLNK(stab.st_mode)) {
      /* We can't make a symlink with two hardlinks, so we'll have to
       * copy it. (Pretend that making a copy of a symlink is the same
       * as linking to it.) */
      varbuf_reset(&symlinkfn);
      varbuf_grow(&symlinkfn, stab.st_size + 1);
      r = readlink(fnamevb.buf, symlinkfn.buf, symlinkfn.size);
      if (r < 0)
        ohshite(_("unable to read link `%.255s'"), ti->name);
      else if (r != stab.st_size)
        ohshit(_("symbolic link '%.250s' size has changed from %jd to %zd"),
               fnamevb.buf, stab.st_size, r);
      varbuf_trunc(&symlinkfn, r);
      varbuf_end_str(&symlinkfn);
      if (symlink(symlinkfn.buf,fnametmpvb.buf))
        ohshite(_("unable to make backup symlink for `%.255s'"), ti->name);
      if (lchown(fnametmpvb.buf,stab.st_uid,stab.st_gid))
        ohshite(_("unable to chown backup symlink for `%.255s'"), ti->name);
      tarobject_set_se_context(fnamevb.buf, fnametmpvb.buf, stab.st_mode);
    } else {
      debug(dbg_eachfiledetail,"tarobject nondirectory, `link' backup");
      if (link(fnamevb.buf,fnametmpvb.buf))
        ohshite(_("unable to make backup link of `%.255s' before installing new version"),
                ti->name);
    }
  }

  /*
   * CLEANUP: Now the old file is in .dpkg-tmp, and the new file is still
   * in .dpkg-new.
   */

  if (ti->type == tar_filetype_file || ti->type == tar_filetype_hardlink ||
      ti->type == tar_filetype_symlink) {
    nifd->namenode->flags |= fnnf_deferred_rename;

    debug(dbg_eachfiledetail, "tarobject done and installation deferred");
  } else {
    if (rename(fnamenewvb.buf, fnamevb.buf))
      ohshite(_("unable to install new version of `%.255s'"), ti->name);

    /*
     * CLEANUP: Now the new file is in the destination file, and the
     * old file is in .dpkg-tmp to be cleaned up later. We now need
     * to take a different attitude to cleanup, because we need to
     * remove the new file.
     */

    nifd->namenode->flags |= fnnf_placed_on_disk;
    nifd->namenode->flags |= fnnf_elide_other_lists;

    debug(dbg_eachfiledetail, "tarobject done and installed");
  }

  return 0;
}

#if defined(SYNC_FILE_RANGE_WAIT_BEFORE)
static void
tar_writeback_barrier(struct fileinlist *files, struct pkginfo *pkg)
{
  struct fileinlist *cfile;

  for (cfile = files; cfile; cfile = cfile->next) {
    struct filenamenode *usenode;
    const char *usename;
    int fd;

    if (!(cfile->namenode->flags & fnnf_deferred_fsync))
      continue;

    usenode = namenodetouse(cfile->namenode, pkg, &pkg->available);
    usename = usenode->name + 1; /* Skip the leading '/'. */

    setupfnamevbs(usename);

    fd = open(fnamenewvb.buf, O_WRONLY);
    if (fd < 0)
      ohshite(_("unable to open '%.255s'"), fnamenewvb.buf);
    /* Ignore the return code as it should be considered equivalent to an
     * asynchronous hint for the kernel, we are doing an fsync() later on
     * anyway. */
    sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WAIT_BEFORE);
    if (close(fd))
      ohshite(_("error closing/writing `%.255s'"), fnamenewvb.buf);
  }
}
#else
static void
tar_writeback_barrier(struct fileinlist *files, struct pkginfo *pkg)
{
}
#endif

void
tar_deferred_extract(struct fileinlist *files, struct pkginfo *pkg)
{
  struct fileinlist *cfile;
  struct filenamenode *usenode;
  const char *usename;

  tar_writeback_barrier(files, pkg);

  for (cfile = files; cfile; cfile = cfile->next) {
    debug(dbg_eachfile, "deferred extract of '%.255s'", cfile->namenode->name);

    if (!(cfile->namenode->flags & fnnf_deferred_rename))
      continue;

    usenode = namenodetouse(cfile->namenode, pkg, &pkg->available);
    usename = usenode->name + 1; /* Skip the leading '/'. */

    setupfnamevbs(usename);

    if (cfile->namenode->flags & fnnf_deferred_fsync) {
      int fd;

      debug(dbg_eachfiledetail, "deferred extract needs fsync");

      fd = open(fnamenewvb.buf, O_WRONLY);
      if (fd < 0)
        ohshite(_("unable to open '%.255s'"), fnamenewvb.buf);
      if (fsync(fd))
        ohshite(_("unable to sync file '%.255s'"), fnamenewvb.buf);
      if (close(fd))
        ohshite(_("error closing/writing `%.255s'"), fnamenewvb.buf);

      cfile->namenode->flags &= ~fnnf_deferred_fsync;
    }

    debug(dbg_eachfiledetail, "deferred extract needs rename");

    if (rename(fnamenewvb.buf, fnamevb.buf))
      ohshite(_("unable to install new version of `%.255s'"),
              cfile->namenode->name);

    cfile->namenode->flags &= ~fnnf_deferred_rename;

    /*
     * CLEANUP: Now the new file is in the destination file, and the
     * old file is in .dpkg-tmp to be cleaned up later. We now need
     * to take a different attitude to cleanup, because we need to
     * remove the new file.
     */

    cfile->namenode->flags |= fnnf_placed_on_disk;
    cfile->namenode->flags |= fnnf_elide_other_lists;

    debug(dbg_eachfiledetail, "deferred extract done and installed");
  }
}

void
enqueue_deconfigure(struct pkginfo *pkg, struct pkginfo *pkg_removal)
{
  struct pkg_deconf_list *newdeconf;

  ensure_package_clientdata(pkg);
  pkg->clientdata->istobe = itb_deconfigure;
  newdeconf = m_malloc(sizeof(struct pkg_deconf_list));
  newdeconf->next = deconfigure;
  newdeconf->pkg = pkg;
  newdeconf->pkg_removal = pkg_removal;
  deconfigure = newdeconf;
}

/**
 * Try if we can deconfigure the package and queue it if so.
 *
 * Also checks whether the pdep is forced, first, according to force_p.
 * force_p may be NULL in which case nothing is considered forced.
 *
 * Action is a string describing the action which causes the
 * deconfiguration:
 *
 *   "removal of <package>"       (due to Conflicts+Depends; removal != NULL)
 *   "installation of <package>"  (due to Breaks;            removal == NULL)
 *
 * @retval 0 Not possible (why is printed).
 * @retval 1 Deconfiguration queued ok (no message printed).
 * @retval 2 Forced (no deconfiguration needed, why is printed).
 */
static int
try_deconfigure_can(bool (*force_p)(struct deppossi *), struct pkginfo *pkg,
                    struct deppossi *pdep, const char *action,
                    struct pkginfo *removal, const char *why)
{
  if (force_p && force_p(pdep)) {
    warning(_("ignoring dependency problem with %s:\n%s"), action, why);
    return 2;
  } else if (f_autodeconf) {
    if (pkg->installed.essential) {
      if (fc_removeessential) {
        warning(_("considering deconfiguration of essential\n"
                  " package %s, to enable %s."),
                pkg_name(pkg, pnaw_nonambig), action);
      } else {
        fprintf(stderr, _("dpkg: no, %s is essential, will not deconfigure\n"
                          " it in order to enable %s.\n"),
                pkg_name(pkg, pnaw_nonambig), action);
        return 0;
      }
    }
    enqueue_deconfigure(pkg, removal);
    return 1;
  } else {
    fprintf(stderr, _("dpkg: no, cannot proceed with %s (--auto-deconfigure will help):\n%s"),
            action, why);
    return 0;
  }
}

static int try_remove_can(struct deppossi *pdep,
                          struct pkginfo *fixbyrm,
                          const char *why) {
  char action[512];
  sprintf(action, _("removal of %.250s"), pkg_name(fixbyrm, pnaw_nonambig));
  return try_deconfigure_can(force_depends, pdep->up->up, pdep,
                             action, fixbyrm, why);
}

void check_breaks(struct dependency *dep, struct pkginfo *pkg,
                  const char *pfilename) {
  struct pkginfo *fixbydeconf;
  struct varbuf why = VARBUF_INIT;
  int ok;

  fixbydeconf = NULL;
  if (depisok(dep, &why, &fixbydeconf, NULL, false)) {
    varbuf_destroy(&why);
    return;
  }

  varbuf_end_str(&why);

  if (fixbydeconf && f_autodeconf) {
    char action[512];

    ensure_package_clientdata(fixbydeconf);
    assert(fixbydeconf->clientdata->istobe == itb_normal);

    sprintf(action, _("installation of %.250s"),
            pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
    fprintf(stderr, _("dpkg: considering deconfiguration of %s,"
                      " which would be broken by %s ...\n"),
            pkg_name(fixbydeconf, pnaw_nonambig), action);

    ok= try_deconfigure_can(force_breaks, fixbydeconf, dep->list,
                            action, NULL, why.buf);
    if (ok == 1) {
      fprintf(stderr, _("dpkg: yes, will deconfigure %s (broken by %s).\n"),
              pkg_name(fixbydeconf, pnaw_nonambig),
              pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
    }
  } else {
    fprintf(stderr, _("dpkg: regarding %s containing %s:\n%s"),
            pfilename, pkgbin_name(pkg, &pkg->available, pnaw_nonambig),
            why.buf);
    ok= 0;
  }
  varbuf_destroy(&why);
  if (ok > 0) return;

  if (force_breaks(dep->list)) {
    warning(_("ignoring breakage, may proceed anyway!"));
    return;
  }

  if (fixbydeconf && !f_autodeconf) {
    ohshit(_("installing %.250s would break %.250s, and\n"
             " deconfiguration is not permitted (--auto-deconfigure might help)"),
           pkgbin_name(pkg, &pkg->available, pnaw_nonambig),
           pkg_name(fixbydeconf, pnaw_nonambig));
  } else {
    ohshit(_("installing %.250s would break existing software"),
           pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
  }
}

void check_conflict(struct dependency *dep, struct pkginfo *pkg,
                    const char *pfilename) {
  struct pkginfo *fixbyrm;
  struct deppossi *pdep, flagdeppossi;
  struct varbuf conflictwhy = VARBUF_INIT, removalwhy = VARBUF_INIT;
  struct dependency *providecheck;

  fixbyrm = NULL;
  if (depisok(dep, &conflictwhy, &fixbyrm, NULL, false)) {
    varbuf_destroy(&conflictwhy);
    varbuf_destroy(&removalwhy);
    return;
  }
  if (fixbyrm) {
    ensure_package_clientdata(fixbyrm);
    if (fixbyrm->clientdata->istobe == itb_installnew) {
      fixbyrm= dep->up;
      ensure_package_clientdata(fixbyrm);
    }
    if (((pkg->available.essential && fixbyrm->installed.essential) ||
         (((fixbyrm->want != want_install && fixbyrm->want != want_hold) ||
           does_replace(pkg, &pkg->available, fixbyrm, &fixbyrm->installed)) &&
          (!fixbyrm->installed.essential || fc_removeessential)))) {
      assert(fixbyrm->clientdata->istobe == itb_normal || fixbyrm->clientdata->istobe == itb_deconfigure);
      fixbyrm->clientdata->istobe= itb_remove;
      fprintf(stderr, _("dpkg: considering removing %s in favour of %s ...\n"),
              pkg_name(fixbyrm, pnaw_nonambig),
              pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
      if (!(fixbyrm->status == stat_installed ||
            fixbyrm->status == stat_triggerspending ||
            fixbyrm->status == stat_triggersawaited)) {
        fprintf(stderr,
                _("%s is not properly installed - ignoring any dependencies on it.\n"),
                pkg_name(fixbyrm, pnaw_nonambig));
        pdep = NULL;
      } else {
        for (pdep = fixbyrm->set->depended.installed;
             pdep;
             pdep = pdep->rev_next) {
          if (pdep->up->type != dep_depends && pdep->up->type != dep_predepends)
            continue;
          if (depisok(pdep->up, &removalwhy, NULL, NULL, false))
            continue;
          varbuf_end_str(&removalwhy);
          if (!try_remove_can(pdep,fixbyrm,removalwhy.buf))
            break;
        }
        if (!pdep) {
          /* If we haven't found a reason not to yet, let's look some more. */
          for (providecheck= fixbyrm->installed.depends;
               providecheck;
               providecheck= providecheck->next) {
            if (providecheck->type != dep_provides) continue;
            for (pdep = providecheck->list->ed->depended.installed;
                 pdep;
                 pdep = pdep->rev_next) {
              if (pdep->up->type != dep_depends && pdep->up->type != dep_predepends)
                continue;
              if (depisok(pdep->up, &removalwhy, NULL, NULL, false))
                continue;
              varbuf_end_str(&removalwhy);
              fprintf(stderr, _("dpkg"
                      ": may have trouble removing %s, as it provides %s ...\n"),
                      pkg_name(fixbyrm, pnaw_nonambig),
                      providecheck->list->ed->name);
              if (!try_remove_can(pdep,fixbyrm,removalwhy.buf))
                goto break_from_both_loops_at_once;
            }
          }
        break_from_both_loops_at_once:;
        }
      }
      if (!pdep && skip_due_to_hold(fixbyrm)) {
        pdep= &flagdeppossi;
      }
      if (!pdep && (fixbyrm->eflag & eflag_reinstreq)) {
        if (fc_removereinstreq) {
          fprintf(stderr, _("dpkg: package %s requires reinstallation, but will"
                  " remove anyway as you requested.\n"),
                  pkg_name(fixbyrm, pnaw_nonambig));
        } else {
          fprintf(stderr, _("dpkg: package %s requires reinstallation, "
                  "will not remove.\n"), pkg_name(fixbyrm, pnaw_nonambig));
          pdep= &flagdeppossi;
        }
      }
      if (!pdep) {
        /* This conflict is OK - we'll remove the conflictor. */
        enqueue_conflictor(pkg, fixbyrm);
        varbuf_destroy(&conflictwhy); varbuf_destroy(&removalwhy);
        fprintf(stderr, _("dpkg: yes, will remove %s in favour of %s.\n"),
                pkg_name(fixbyrm, pnaw_nonambig),
                pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
        return;
      }
      /* Put it back. */
      fixbyrm->clientdata->istobe = itb_normal;
    }
  }
  varbuf_end_str(&conflictwhy);
  fprintf(stderr, _("dpkg: regarding %s containing %s:\n%s"),
          pfilename, pkgbin_name(pkg, &pkg->available, pnaw_nonambig),
          conflictwhy.buf);
  if (!force_conflicts(dep->list))
    ohshit(_("conflicting packages - not installing %.250s"),
           pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
  warning(_("ignoring conflict, may proceed anyway!"));
  varbuf_destroy(&conflictwhy);

  return;
}

void cu_cidir(int argc, void **argv) {
  char *cidir= (char*)argv[0];
  char *cidirrest= (char*)argv[1];
  cidirrest[-1] = '\0';
  ensure_pathname_nonexisting(cidir);
}

void cu_fileslist(int argc, void **argv) {
  destroyobstack();
}

int
archivefiles(const char *const *argv)
{
  const char *volatile thisarg;
  const char *const *volatile argp;
  jmp_buf ejbuf;

  trigproc_install_hooks();

  modstatdb_open(f_noact ?                          msdbrw_readonly :
                 (cipaction->arg_int == act_avail ? msdbrw_readonly :
                  fc_nonroot ?                      msdbrw_write :
                                                    msdbrw_needsuperuser) |
                 msdbrw_available_write);

  checkpath();
  log_message("startup archives %s", cipaction->olong);

  if (f_recursive) {
    int pi[2], nfiles, c, i, r;
    pid_t pid;
    FILE *pf;
    static struct varbuf findoutput;
    const char **arglist;
    char *p;

    if (!*argv)
      badusage(_("--%s --recursive needs at least one path argument"),cipaction->olong);

    m_pipe(pi);
    pid = subproc_fork();
    if (pid == 0) {
      struct command cmd;
      const char *const *ap;

      m_dup2(pi[1],1); close(pi[0]); close(pi[1]);

      command_init(&cmd, FIND, _("find for dpkg --recursive"));
      command_add_args(&cmd, FIND, "-L", NULL);

      for (ap = argv; *ap; ap++) {
        if (strchr(FIND_EXPRSTARTCHARS,(*ap)[0])) {
          char *a;

          m_asprintf(&a, "./%s", *ap);
          command_add_arg(&cmd, a);
        } else {
          command_add_arg(&cmd, (const char *)*ap);
        }
      }

      command_add_args(&cmd, "-name", "*.deb", "-type", "f", "-print0", NULL);

      command_exec(&cmd);
    }
    close(pi[1]);

    nfiles= 0;
    pf= fdopen(pi[0],"r");  if (!pf) ohshite(_("failed to fdopen find's pipe"));
    varbuf_reset(&findoutput);
    while ((c= fgetc(pf)) != EOF) {
      varbuf_add_char(&findoutput, c);
      if (!c) nfiles++;
    }
    if (ferror(pf)) ohshite(_("error reading find's pipe"));
    if (fclose(pf)) ohshite(_("error closing find's pipe"));
    r = subproc_wait_check(pid, "find", PROCNOERR);
    if (r != 0)
      ohshit(_("find for --recursive returned unhandled error %i"),r);

    if (!nfiles)
      ohshit(_("searched, but found no packages (files matching *.deb)"));

    arglist= m_malloc(sizeof(char*)*(nfiles+1));
    p = findoutput.buf;
    for (i = 0; i < nfiles; i++) {
      arglist[i] = p;
      while (*p++ != '\0') ;
    }
    arglist[i] = NULL;
    argp= arglist;
  } else {
    if (!*argv) badusage(_("--%s needs at least one package archive file argument"),
                         cipaction->olong);
    argp= argv;
  }

  currenttime = time(NULL);

  /* Initialize fname variables contents. */

  varbuf_reset(&fnamevb);
  varbuf_reset(&fnametmpvb);
  varbuf_reset(&fnamenewvb);

  varbuf_add_str(&fnamevb, instdir);
  varbuf_add_char(&fnamevb, '/');
  varbuf_add_str(&fnametmpvb, instdir);
  varbuf_add_char(&fnametmpvb, '/');
  varbuf_add_str(&fnamenewvb, instdir);
  varbuf_add_char(&fnamenewvb, '/');
  fnameidlu= fnamevb.used;

  ensure_diversions();
  ensure_statoverrides();

  while ((thisarg = *argp++) != NULL) {
    if (setjmp(ejbuf)) {
      pop_error_context(ehflag_bombout);
      if (abort_processing)
        break;
      continue;
    }
    push_error_context_jump(&ejbuf, print_error_perpackage, thisarg);

    process_archive(thisarg);
    onerr_abort++;
    m_output(stdout, _("<standard output>"));
    m_output(stderr, _("<standard error>"));
    onerr_abort--;

    pop_error_context(ehflag_normaltidy);
  }

  switch (cipaction->arg_int) {
  case act_install:
  case act_configure:
  case act_triggers:
  case act_remove:
  case act_purge:
    process_queue();
  case act_unpack:
  case act_avail:
    break;
  default:
    internerr("unknown action '%d'", cipaction->arg_int);
  }

  trigproc_run_deferred();
  modstatdb_shutdown();

  return 0;
}

/**
 * Decide whether we want to install a new version of the package.
 *
 * @param pkg The package with the version we might want to install
 *
 * @retval true  If the package should be skipped.
 * @retval false If the package should be installed.
 */
bool
wanttoinstall(struct pkginfo *pkg)
{
  int r;

  if (pkg->want != want_install && pkg->want != want_hold) {
    if (f_alsoselect) {
      printf(_("Selecting previously unselected package %s.\n"),
             pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
      pkg_set_want(pkg, want_install);
      return true;
    } else {
      printf(_("Skipping unselected package %s.\n"),
             pkgbin_name(pkg, &pkg->available, pnaw_nonambig));
      return false;
    }
  }

  if (pkg->eflag & eflag_reinstreq)
    return true;
  if (pkg->status < stat_unpacked)
    return true;

  r = versioncompare(&pkg->available.version, &pkg->installed.version);
  if (r > 0) {
    return true;
  } else if (r == 0) {
    /* Same version fully installed. */
    if (f_skipsame) {
      fprintf(stderr, _("Version %.250s of %.250s already installed, "
                        "skipping.\n"),
              versiondescribe(&pkg->installed.version, vdew_nonambig),
              pkg_name(pkg, pnaw_nonambig));
      return false;
    } else {
      return true;
    }
  } else {
    if (fc_downgrade) {
      warning(_("downgrading %.250s from %.250s to %.250s."),
              pkg_name(pkg, pnaw_nonambig),
              versiondescribe(&pkg->installed.version, vdew_nonambig),
              versiondescribe(&pkg->available.version, vdew_nonambig));
      return true;
    } else {
      fprintf(stderr, _("Will not downgrade %.250s from version %.250s "
                        "to %.250s, skipping.\n"),
              pkg_name(pkg, pnaw_nonambig),
              versiondescribe(&pkg->installed.version, vdew_nonambig),
              versiondescribe(&pkg->available.version, vdew_nonambig));
      return false;
    }
  }
}

struct fileinlist *newconff_append(struct fileinlist ***newconffileslastp_io,
				   struct filenamenode *namenode) {
  struct fileinlist *newconff;

  newconff= m_malloc(sizeof(struct fileinlist));
  newconff->next = NULL;
  newconff->namenode= namenode;
  **newconffileslastp_io= newconff;
  *newconffileslastp_io= &newconff->next;
  return newconff;
}

/* vi: ts=8 sw=2
 */
