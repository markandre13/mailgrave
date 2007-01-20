/*
 * MailGrave -- a simple smtpd daemon influenced by qmail
 * Copyright (C) 2006, 2007 by Mark-André Hopf <mhopf@mark13.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or   
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <pwd.h>
#include <grp.h>

struct cug_t
{
  cug_t() {
    rootdir = 0;
    uid = (uid_t)-1;
    gid = (gid_t)-1;
  }
  const char *rootdir;
  uid_t uid;
  gid_t gid;
};

bool chrootOrUser(int argc, char **argv, int *i, cug_t *cug);
bool setChrootUidGid(cug_t *cug);
