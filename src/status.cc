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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/file.h>

#include "status.hh"

static int status_fd;
static status_t *status;

status_t *
mapStatus() {
  status_fd = open("status", O_RDWR);

  if (status_fd < 0 && errno == ENOENT) {
    status_fd = open("status", O_RDWR | O_CREAT | O_EXCL, 00600);
    if (status_fd < 0 && errno == EEXIST) {
      status_fd = open("status", O_RDWR);
    } else
    if (status_fd < 0) {
      perror("failed to open 'status' file");
      goto error0;
    } else {
      char buffer[4096];
      memset(buffer, 0, sizeof(buffer));
      if (write(status_fd, buffer, 4096)!=4096) {
	perror("failed to write initial 4k into 'status' file");
        close(status_fd);
        unlink("status");
        goto error0;
      }
    }
  }
  if (status_fd < 0) {
    perror("failed to open status file");
    goto error0;
  }
  status = (status_t*) mmap(0, 4096, PROT_READ|PROT_WRITE, MAP_SHARED, status_fd, 0);
  if (!status) {
    perror("failed to mmap status file");
    goto error1;
  }
  return status;
  
error1:
  close(status_fd);
error0:
  exit(EXIT_FAILURE);
}

void
unmapStatus()
{
  if (munmap(status, 4096)<0) {
    perror("failed to munmap status file");
    goto error1;
  }
  
  if (close(status_fd) < 0) {
    perror("failed to close status file");
    goto error0;
  }
  return;
  
error1:
  close(status_fd);
error0:
  exit(EXIT_FAILURE);
}

void
lockStatus()
{
  if (flock(status_fd, LOCK_EX)==0)
    return;
  perror("failed to lock status file");
  munmap(status, 4096);
  close(status_fd);
  exit(EXIT_FAILURE);
}

void
unlockStatus()
{
  if (msync(status, 4096, MS_SYNC)!=0) {
    perror("failed to sync status file");
    goto error;
  }

  if (flock(status_fd, LOCK_UN)!=0) {
    perror("failed to unlock status file");
    goto error;
  }
  return;

error:
  perror("failed to unlock status file");
  munmap(status, 4096);
  close(status_fd);
  exit(EXIT_FAILURE);
}
