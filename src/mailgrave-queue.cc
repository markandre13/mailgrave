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

/**
 *
 * the file 'status' is used for queue management (last file, first file)
 * and accessed via mmap for maximal speed
 *
 * \li read mail from fd 0
 * \li read envelope from fd 1
 *     F<mail>\0T<mail>\0...
 *
 */

//#define _ISOC99_SOURCE 1
#define __STDC_VERSION__ 199901L

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/file.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "status.hh"
#include "createsocket.hh"
#include "opensocket.hh"
#include "cug.hh"

unsigned long long createTail();
bool pushQueue(FILE *in);
bool copyfile(int out, int in);
static bool copystream(int fd, FILE *in, bool null);

static void usage()
{
  printf(
    "mailgrave-queue\n"
    "Copyright (C) 2006, 2007 Mark-André Hopf <mhopf@mark13.org>\n"
    "Visit http://mark13.org/mailgrave/ for full details.\n"
    "\n"
    "Usage:\n"
    "  This daemon creates the unix domain socket 'queue.ctrl' and waits for\n"
    "  incoming mail from the mailgrave-inject and mailgrave-smtpd programs and\n"
    "  stores the inside the mail queue.\n"
    "\n"
    "  --in <socket>\n"
    "    UNIX domain socket to listen on. Defaults to 'queue.ctrl'.\n"
    "  --out <socket>\n"
    "    UNIX domain socket to open and close after a new mail was queued.\n"
    "    Defaults to 'send.ctrl'.\n"
    "  --chroot <directory>\n"
    "    change root directory after setup\n"
    "  --user <user>[:<group>]\n"
    "    change user/group after setup\n"
    "  --help\n"
    "    Show this help text.\n"
  );
}

const char *in = "queue.ctrl";
const char *out = "send.ctrl";

int
main(int argc, char **argv)
{
  setvbuf(stdout, 0, _IONBF, 0);
  
  const char *in = "queue.ctrl";
  cug_t cug;

  // parse argument list
  for(int i=1; i<argc; ++i) {
    if (strcmp(argv[i], "--help")==0) {
      usage();
      return EXIT_SUCCESS;
    } else
    if (strcmp(argv[i], "--in")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      in = argv[++i];
    } else
    if (strcmp(argv[i], "--out")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      out = argv[++i];
    } else
    if (chrootOrUser(argc, argv, &i, &cug)) {
    } else {
      fprintf(stderr, "%s: unknown argument %s, please try --help\n",
                      argv[0], argv[i]);
      return EXIT_FAILURE;
    }
  }
  
  // create socket
  int sock = createUNIXSocket(in);
  if (sock<0) {
    perror ("failed to create unix domain socket");
    return EXIT_FAILURE;
  }
  
  if (listen(sock, 5) < 0) {
    perror("control listen");
    close(sock);
    return EXIT_FAILURE;
  }
  
  if (chown(in, cug.uid, cug.gid)!=0) {
    perror("chown");
    close(sock);
    return EXIT_FAILURE;
  }

  // change root, uid, gid
  if (!setChrootUidGid(&cug))
    return EXIT_FAILURE;
    
  printf("mailgrave-queue started\n");

  // message loop
  while(true) {
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    printf("waiting for message\n");
    int client = accept(sock, (struct sockaddr*) &addr, &addrlen);
    if (client<0) {
      fprintf(stderr, "accept failed", strerror(errno));
      continue;
    }
    // we should fork here...
    FILE *in = fdopen(client, "r");
    if (!in) {
      close(client);
      fprintf(stderr, "fdopen failed", strerror(errno));
      continue;
    }
    printf("awoke\n");
    if (pushQueue(in)) {
      char x = 1;
      write(client, &x, 1);
      printf("got message, triggering mailgrave-send via '%s'\n", out);
      int trigger = openUNIXSocket(out);
      if (trigger>=0)
        close(trigger);
      else
        printf("%s: failed to trigger mailgrave-send via '%s': %s\n",
               argv[0], out, strerror(errno));
    } else {
      char x = 0;
      write(client, &x, 1);
      printf("%s: push queue failed\n", argv[0]);
    }
    fclose(in);
  }
  return EXIT_SUCCESS;
}

bool
pushQueue(FILE *unixfd)
{
  // create new filenames
  time_t now;
  unsigned long long id = createTail();
  char datname[64];
  char envname[64];
  int dfd=-1, efd=-1;

  snprintf(datname, sizeof(datname), "%020llX.dat", id);
  snprintf(envname, sizeof(envname), "%020llX.env", id);

  // create 'Received:' header
  char received[1024];
  char str[256];
  now = time(NULL);
  if (strftime(str, sizeof(str), "%a, %d %b %Y %T %z",
               localtime(&now)) == 0)
  {
    perror("strftime");
    goto error;
  }
  snprintf(received, sizeof(received),
    "Received: (mailgrave-queue %u invoked by uuid %u);\r\n"
    "     %s\r\n",
    getpid(), getuid(), str);
  
  // create files
  dfd = open(datname, O_RDWR | O_CREAT | O_EXCL, 00600);
  if (dfd<0) {
    perror("failed to create queue data file");
    goto error;
  }

  efd = open(envname, O_RDWR | O_CREAT | O_EXCL, 00600);
  if (efd<0) {
    perror("failed to create envelope file");
    goto error;
  }
  
  // store envelope
  if (write(efd, "\0\0\0\0\0\0\0\0", 8)!=8) {
    perror("failed top write to envelope");
    goto error;
  }
  if (!copystream(efd, unixfd, true)) {
    perror("failed to copy envelope");
    goto error;
  }

  // copy data
  write(dfd, received, strlen(received));
  if (!copystream(dfd, unixfd, false)) {
    perror("failed to copy data");
    goto error;
  }
  
  return true;

error:
  if (efd!=-1) close(efd);
  if (dfd!=-1) close(dfd);
  unlink(envname);
  unlink(datname);
  return false;
}

unsigned long long
createTail()
{
  int tail;
  // getpagesize()
  status_t *status = mapStatus();
  lockStatus();
  
  if (status->tail == ULLONG_MAX) {
    tail = status->tail = 0;
  } else {
    tail = status->tail++;
  }
  if (status->head == status->tail) {
    fprintf(stderr, "queue is full\n");
    unlockStatus();
    unmapStatus();
    exit(EXIT_FAILURE);
  }
  
  unlockStatus();
  unmapStatus();
  
  return tail;
}

/**
 * Copy stream.
 *
 * \param fd
 *   output stream, which is closed when leaving this function
 * \param in
 *   input stream
 * \param null
 *   when true, stop copying when two '\0' characters appeared in 
 *   the input stream, otherwise copying is stopped on EOF of input stream
 */
bool
copystream(int fd, FILE *in, bool null)
{
  char buffer[4096];
  size_t n=0;
  bool secondnull=false;
  while(true) {
    int c = getc_unlocked(in);
    if (null) {
      if (c==0) {
        if (secondnull)
          break;
        secondnull = true;
      } else {
        secondnull = false;
      }
    }
    if (c==EOF) {
      if (!null) {
        break;
      }
      close(fd);
      return false;
    }
    buffer[n++]=c;
    if (n==4096) {
      write(fd, buffer, n);
      n = 0;
    }
  }
  write(fd, buffer, n);
  return close(fd)==0;
}
