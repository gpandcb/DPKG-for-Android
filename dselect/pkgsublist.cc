/*
 * dselect - Debian package maintenance user interface
 * pkgsublist.cc - status modification and recursive package list handling
 *
 * Copyright © 1995 Ian Jackson <ian@chiark.greenend.org.uk>
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

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <dpkg/i18n.h>
#include <dpkg/dpkg.h>
#include <dpkg/dpkg-db.h>

#include "dselect.h"
#include "bindings.h"

void packagelist::add(pkginfo *pkg) {
  debug(dbg_general, "packagelist[%p]::add(pkginfo %s)", this, pkg->set->name);
  if (!recursive ||  // never add things to top level
      !pkg->clientdata ||  // don't add pure virtual packages
      pkg->clientdata->uprec)  // don't add ones already in the recursive list
    return;
  debug(dbg_general, "packagelist[%p]::add(pkginfo %s) adding",
        this, pkg->set->name);
  perpackagestate *state= &datatable[nitems];
  state->pkg= pkg;
  state->direct= state->original= pkg->clientdata->selected;
  state->suggested= state->selected= pkg->clientdata->selected;
  state->spriority= sp_inherit; state->dpriority= dp_none;
  state->uprec= pkg->clientdata;
  state->relations.init();
  pkg->clientdata= state;
  table[nitems]= state;
  nitems++;
}

void packagelist::add(pkginfo *pkg, pkginfo::pkgwant nw) {
  debug(dbg_general, "packagelist[%p]::add(pkginfo %s, %s)",
        this, pkg->set->name, wantstrings[nw]);
  add(pkg);  if (!pkg->clientdata) return;
  pkg->clientdata->direct= nw;
  selpriority np;
  np= would_like_to_install(nw,pkg) ? sp_selecting : sp_deselecting;
  if (pkg->clientdata->spriority > np) return;
  debug(dbg_general, "packagelist[%p]::add(pkginfo %s, %s) setting",
        this, pkg->set->name, wantstrings[nw]);
  pkg->clientdata->suggested= pkg->clientdata->selected= nw;
  pkg->clientdata->spriority= np;
}

void packagelist::add(pkginfo *pkg, const char *extrainfo, showpriority showimp) {
  debug(dbg_general, "packagelist[%p]::add(pkginfo %s, ..., showpriority %d)",
        this, pkg->set->name, showimp);
  add(pkg);  if (!pkg->clientdata) return;
  if (pkg->clientdata->dpriority < showimp) pkg->clientdata->dpriority= showimp;
  pkg->clientdata->relations(extrainfo);
  pkg->clientdata->relations.terminate();
}

bool
packagelist::alreadydone(doneent **done, void *check)
{
  doneent *search = *done;

  while (search && search->dep != check)
    search = search->next;
  if (search)
    return true;
  debug(dbg_general, "packagelist[%p]::alreadydone(%p, %p) new",
        this, done, check);
  search= new doneent;
  search->next= *done;
  search->dep= check;
  *done= search;
  return false;
}

void packagelist::addunavailable(deppossi *possi) {
  debug(dbg_general, "packagelist[%p]::addunavail(%p)", this, possi);

  if (!recursive) return;
  if (alreadydone(&unavdone,possi)) return;

  assert(possi->up->up->clientdata);
  assert(possi->up->up->clientdata->uprec);

  varbuf& vb= possi->up->up->clientdata->relations;
  vb(possi->ed->name);
  vb(_(" does not appear to be available\n"));
}

bool
packagelist::add(dependency *depends, showpriority displayimportance)
{
  debug(dbg_general, "packagelist[%p]::add(dependency[%p])", this, depends);

  if (alreadydone(&depsdone, depends))
    return false;

  const char *comma= "";
  varbuf info;
  info(depends->up->set->name);
  info(' ');
  info(gettext(relatestrings[depends->type]));
  info(' ');
  deppossi *possi;
  for (possi=depends->list;
       possi;
       possi=possi->next, comma=(possi && possi->next ? ", " : _(" or "))) {
    info(comma);
    info(possi->ed->name);
    if (possi->verrel != dvr_none) {
      switch (possi->verrel) {
      case dvr_earlierequal:  info(" (<= "); break;
      case dvr_laterequal:    info(" (>= "); break;
      case dvr_earlierstrict: info(" (<< "); break;
      case dvr_laterstrict:   info(" (>> "); break;
      case dvr_exact:         info(" (= "); break;
      default: internerr("unknown verrel");
      }
      info(versiondescribe(&possi->version, vdew_nonambig));
      info(")");
    }
  }
  info('\n');
  add(depends->up,info.string(),displayimportance);
  for (possi=depends->list; possi; possi=possi->next) {
    add(&possi->ed->pkg, info.string(), displayimportance);
    if (possi->verrel == dvr_none && depends->type != dep_provides) {
      // providers aren't relevant if a version was specified, or
      // if we're looking at a provider relationship already
      deppossi *provider;
      for (provider = possi->ed->depended.available;
           provider;
           provider = provider->rev_next) {
        if (provider->up->type != dep_provides) continue;
        add(provider->up->up,info.string(),displayimportance);
        add(provider->up,displayimportance);
      }
    }
  }
  return true;
}

void repeatedlydisplay(packagelist *sub,
                       showpriority initial,
                       packagelist *unredisplay) {
  pkginfo **newl;
  keybindings *kb;

  debug(dbg_general, "repeatedlydisplay(packagelist[%p])", sub);
  if (sub->resolvesuggest() != 0 && sub->deletelessimp_anyleft(initial)) {
    debug(dbg_general, "repeatedlydisplay(packagelist[%p]) once", sub);
    if (unredisplay) unredisplay->enddisplay();
    for (;;) {
      manual_install = 0; /* Remove flag now that resolvesuggest has seen it. */
      newl= sub->display();
      if (!newl) break;
      debug(dbg_general, "repeatedlydisplay(packagelist[%p]) newl", sub);
      kb= sub->bindings; delete sub;
      sub= new packagelist(kb,newl);
      if (sub->resolvesuggest() <= 1) break;
      if (!sub->deletelessimp_anyleft(dp_must)) break;
      debug(dbg_general, "repeatedlydisplay(packagelist[%p]) again", sub);
    }
    if (unredisplay) unredisplay->startdisplay();
  }
  debug(dbg_general, "repeatedlydisplay(packagelist[%p]) done", sub);
  delete sub;
}

int packagelist::deletelessimp_anyleft(showpriority than) {
  debug(dbg_general, "packagelist[%p]::dli_al(%d): nitems=%d",
        this, than, nitems);
  int insat, runthr;
  for (runthr=0, insat=0;
       runthr < nitems;
       runthr++) {
    if (table[runthr]->dpriority < than) {
      table[runthr]->free(recursive);
    } else {
      if (insat != runthr) table[insat]= table[runthr];
      insat++;
    }
  }
  nitems= insat;
  debug(dbg_general, "packagelist[%p]::dli_al(%d) done; nitems=%d",
        this, than, nitems);
  return nitems;
}
