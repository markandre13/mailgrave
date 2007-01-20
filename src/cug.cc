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

#include "cug.hh"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

bool
chrootOrUser(int argc, char **argv, int *i, cug_t *cug)
{
  if (strcmp(argv[*i], "--chroot")==0) {
    if (*i+1 >= argc) {
      fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[*i]);
      exit(EXIT_FAILURE);
    }
    cug->rootdir = argv[++(*i)];
  } else
  if (strcmp(argv[*i], "--user")==0) {
    if (*i+1 >= argc) {
      fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[*i]);
      exit(EXIT_FAILURE);
    }
    ++(*i);
    char *user = argv[*i];
    char *group = strchr(user, ':');
    if (group) {
      *group = 0;
      ++group;
    }
    if (user) {
    	struct passwd *pw = getpwnam(user);
    	if (!pw) {
    	  fprintf(stderr, "unknown user '%s'\n", user);
    	  exit(EXIT_FAILURE);
    	}
    	cug->uid = pw->pw_uid;
    	cug->gid = pw->pw_gid;
    }
    if (group) {
    	struct group *gr = getgrnam(group);
    	if (!gr) {
    	  fprintf(stderr, "unknown group '%s'\n", group);
    	  exit(EXIT_FAILURE);
    	}
    	cug->gid = gr->gr_gid;
    }
  } else {
    return false;
  }
  return true;
}

bool  
setChrootUidGid(cug_t *cug)
{
  if (cug->rootdir) {
    if (chdir(cug->rootdir)!=0) {
      perror("chdir");
      return false;
    }
    if (chroot(".")!=0) {
      perror("chroot");
      return false;
    }
  }
  
  if (cug->gid!=(gid_t)-1) {
    if (setgid(cug->gid)!=0) {
      perror("setgid");
      return false;
    }
  }
  if (cug->uid!=(uid_t)-1) {
    if (setuid(cug->uid)!=0) {
      perror("setuid");
      return false;
    }
  }
  return true;
}
