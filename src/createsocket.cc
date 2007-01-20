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

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

int
createUNIXSocket(const char *name)
{
  struct sockaddr_un control;
  control.sun_family = AF_UNIX;
  if (strlen(name) >= sizeof(control.sun_path)) {
    fprintf(stderr, "path name for control socket is too long.\n");
    return -1;
  }
  strcpy(control.sun_path, name);

  unlink(control.sun_path);
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock<0) {
    perror ("failed to create unix domain socket");
    return -1;
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    
  mode_t om = umask(0117);
  if (bind(sock, (struct sockaddr *) &control,
                  offsetof(struct sockaddr_un, sun_path) +
                  strlen(control.sun_path)) < 0)
  {
    perror("control bind");
    close(sock);
    sock = -1;
  }
  umask(om);
  return sock;
}
