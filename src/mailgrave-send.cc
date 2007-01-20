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

#define __STDC_VERSION__ 199901L

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/time.h>

#include "cug.hh"
#include "status.hh"
#include "createsocket.hh"
#include "opensocket.hh"

#include <string>
#include <vector>
using std::string;
using std::vector;

static bool handleMail(unsigned long long);
static bool copyfile(FILE *out, int in);

static int verbose = 0;

const char *in = "send.ctrl";
const char *out = "remote.ctrl";

static void
usage()
{
  printf(
    "mailgrave-send\n"
    "Copyright (C) 2006, 2007 Mark-André Hopf <mhopf@mark13.org>\n"
    "Visit http://mark13.org/mailgrave/ for full details.\n"
    "\n"
    "Usage:\n"
    "  Copy an email from stdin to a SMTP server.\n"
    "Options:\n"
    "  --in <socket>\n"
    "    UNIX domain socket to listen on. Defaults to 'send.ctrl'.\n"
    "  --out <socket>\n"
    "    Defaults to 'remote.ctrl' for now...\n"
    "  --chroot <directory>\n"
    "    change root directory after setup\n"
    "  --user <user>[:<group>]\n"
    "    change user/group after setup\n"
    "  --verbose | -v\n"
    "    Be verbose\n"
    "  --help\n"
    "    show this help text\n"
    "\n"
  );
}

int
main(int argc, char **argv)
{
  setvbuf(stdout, 0, _IONBF, 0);

  cug_t cug;

  // parse argument list
  for(int i=1; i<argc; ++i) {
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
    } else
    if (strcmp(argv[i], "--verbose")==0 || strcmp(argv[i], "-v")==0) {
      ++verbose;
    } else
    if (strcmp(argv[i], "--help")==0) {
      usage();
      return EXIT_SUCCESS;
    } else {
       fprintf(stderr, "%s: unknown argument %s, please try --help\n",
                       argv[0], argv[i]);
       return EXIT_FAILURE;
    }
  }
  
  int sock = createUNIXSocket(in);
  if (sock<0) {
    perror("failed to create unix domain socket");
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

  status_t *status = mapStatus();
  
  unsigned long long head, tail;

  lockStatus();
  head = status->head;
  tail = status->tail;
  unlockStatus();
  
  printf("mailgrave-send started: head: %llu, tail: %llu, size: %llu\n",
         head, 
         tail,
         head<=tail ? tail-head : ULLONG_MAX-head+tail);

  timeval t0, t1;
  gettimeofday(&t0, NULL);
  unsigned long long oldtail = tail;
  bool timeout = true;
  while(true) {
    // try to send the queue
    if (!timeout) {
      gettimeofday(&t1, NULL);
      if (t1.tv_sec >= t0.tv_sec)
        timeout = true;
    }
    
    unsigned long long i = timeout ? head : oldtail;
    bool error = false;
    while(i != tail) {
//      printf("%s: handle mail %llu\n", argv[0], i);
      if (handleMail(i)) {
        if (!error)
          head = i+1; // advance head
      } else {
        error = true;
      }
      if (i == ULLONG_MAX)
        i = 0;
      else
        ++i;
    }
    timeout = false;
    
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(sock, &rd);

    // 4.5.4.1 Sending Strategy says that we should:
    // 30min, 30min, 2h, 2h, 2h, ... until 5 days, then give up
    timeval t1;
    gettimeofday(&t1, NULL);
    while(t1.tv_sec >= t0.tv_sec)
      t0.tv_sec += 30 * 60; 
    t1.tv_usec = 0;
    t1.tv_sec = t0.tv_sec - t1.tv_sec;
    
    printf("%s: waiting for socket (head=%llu, tail=%llu, %lus)\n",
           argv[0], head, tail, t1.tv_sec);

    int r;
    while(true) {
      r = select(sock+1, &rd, 0, 0, &t1);
      if (r>=0)
        break;
      printf("%s: select error: %s\n", argv[0], strerror(errno));
    }
    if (r==0) {
      printf("%s: awoke because of timeout\n", argv[0]);
      timeout = true;
    } else {
      printf("%s: awoke because of signal\n", argv[0]);
      struct sockaddr_un addr;
      socklen_t addrlen = sizeof(addr);
      int client = accept(sock, (struct sockaddr*) &addr, &addrlen);
      close(client);
    }
    
    // sync with the status file
    oldtail = tail;
    lockStatus();
    status->head = head;
    tail = status->tail;
    unlockStatus();
  }
  unmapStatus();
}

/**
 * handle a mail
 * returns 'true' when the mail was removed from the queue.
 *
 * we're also not keeping track of the number of retries. this information
 * could be placed at the head of the .env file.
 */
static bool
handleMail(unsigned long long id)
{
  int state = 0;
  int type;
  string user, user1, domain;
  vector<char *> args;
  FILE *envf, *out;
  int sock;
  const char *name = ::out;

  char datname[64];
  char envname[64];
  snprintf(datname, sizeof(datname), "%020llX.dat", id);
  snprintf(envname, sizeof(envname), "%020llX.env", id);

  int datfd = open(datname, O_RDONLY);
  envf = fopen(envname, "r");
  if (datfd<0 && envf==0) {
    printf("skip %020llX, already sent\n", id);
    return true;
  }
  if (datfd<0) {
    printf("failed to open data file '%s': %s\n", datname, strerror(errno));
    goto error;
  }
  if (envf==0) {
    printf("failed to open envelope file '%s': %s\n", envname, strerror(errno));
    goto error;
  }

  printf("transmit %020llX\n", id);

  fseek(envf, 8, SEEK_SET);

  // the code below is a bit oversized in the moment but it will be
  // used to implement the decision where a package will be delivered
  // to, thus the parsing of the addresses
  
  // open connection to mailgrave-remote
  sock = openUNIXSocket(name);
  if (sock<0) {
    perror("failed to connect to socket");
    return false;
  }
  out = fdopen(sock, "w");
  
  putc_unlocked(0, out); // empty hostname for now

  // parse envelope file
  while(state!=100) {
    int c = getc_unlocked(envf);
    switch(state) {
      case 0:
        user.clear();
        domain.clear();
        switch(c) {
          case EOF:
            state = 100;
            break;
          case 'T':
          case 'F':
            type = c;
            state = 1;
            break;
          default:
            fprintf(stderr, "unexpected character in envelope file '%s'\n", envname);
            goto error;
        }
        break;
      case 1:
        switch(c) {
          case EOF:
            fprintf(stderr, "unexpected end of envelope file '%s'\n", envname);
            goto error;
          case '@':
            if (!user.empty()) {
              user1 += user;
              user1 += '@';
            }
            user = domain;
            domain.clear();
            break;
          case '\0':
            user = user1 + user;
            if (user.empty()) {
              user = domain;
              domain = "localhost";
            }
            printf("  found '%c' '%s' @ '%s'\n",
                   type, user.c_str(), domain.c_str());
            state = 0;
            fwrite(user.c_str(), user.size(), 1, out);
            putc('@', out);
            fwrite(domain.c_str(), domain.size()+1, 1, out);
            break;
          default:
            domain += c;
        }
    }
  }
  
  putc(0, out); // end of envelope marker

  if (!copyfile(out, datfd)) {
    goto error;
  }
  fflush(out);
  if (shutdown(sock, SHUT_WR)==-1) {
    perror("mailgrave-send: shutdown");
    goto error;
  }
  
  char result;
  if (read(sock, &result, 1)!=1) {
    perror("mailgrave-send: unabled to read delivery process result\n");
    goto error;
  }
  if (!result) {
    printf("mailgrave-send: delivery process failed\n");
    goto error;
  }

  fclose(out);

  if (datfd>=0)
    close(datfd);
  if (envf!=0)
    fclose(envf);
  unlink(datname);
  unlink(envname);
  return true;
  
error:
  if (datfd>=0)
    close(datfd);
  if (envf!=0)
    fclose(envf);
  fclose(out);
  return false;
}

bool
copyfile(FILE *out, int in)
{
  char buffer[4096];
  while(true) {
    ssize_t l = read(in, buffer, sizeof(buffer));
    if (l==0)
      return true;
    if (l<0) {
      printf("mailgrave-send: copyfile: %s\n", strerror(errno));
      return false;
    }
    fwrite(buffer, l, 1, out);
  }
}
