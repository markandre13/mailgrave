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

// RFC 1830: SMTP Service Extensions for Transmission of Large and Binary MIME Messages

#include "cug.hh"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/un.h>

#include <string>
using std::string;

static bool slow = false;

// RFC 2821: 4.5.3.1 Size limits and minimums
static const size_t local_part_maxsize = 64;
static const size_t domain_maxsize = 255;
static const size_t path_maxsize = 256;
static const size_t cmdline_maxsize = 512;
static const size_t replyline_maxsize = 512;
static const size_t textline_maxsize = 1000;

enum {
  CMD_HELO,
  CMD_EHLO,
  CMD_MAIL_FROM,
  CMD_RCPT_TO,
  CMD_DATA,
  CMD_QUIT
};

static void handleClient(int argc, char** argv, int client);
static bool queueMessage(int client, const string &fromToList);
static bool copyData(FILE *out, int client);

static const char* getline(int fd);
static bool getAddress(const char *line, char c, string *result);
static int createSocket(in_addr_t addr, int port);

static void
usage()
{
  printf(
    "mailgrave-smtpd\n"
    "Copyright (C) 2006, 2007 Mark-Andre Hopf <mhopf@mark13.org>\n"
    "Visit http://mark13.org/mailgrave/ for full details.\n"
    "\n"
    "Usage:\n"
    "  This command takes a mail via SMTPD and places it into the mail\n"
    "  queue in case nothing goes wrong.\n"
    "\n"
    "Options:\n"
    "  --bind <ip address>\n"
    "    IP address to bind to, default is any\n"
    "  --port <port>\n"
    "    TCP port to listen on, default is 25\n"
    "  --out <socket>\n"
    "    UNIX domain socket of mailgrave-queue. Defaults to 'queue.ctrl'.\n"
    "  --chroot <directory>\n"
    "    change root directory after setup\n"
    "  --user <user>[:<group>]\n"
    "    change user/group after setup\n"
    "  --help\n"
    "    print this message\n"
  );
}

static char hostname[MAXHOSTNAMELEN];
const char *out = "queue.ctrl";

int
main(int argc, char **argv)
{
  setvbuf(stdout, 0, _IONBF, 0);

  cug_t cug;
  int port = 25;
  in_addr_t addr = INADDR_ANY;
  
  // parse argument list
  for(int i=1; i<argc; ++i) {
    if (strcmp(argv[i], "--bind")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      ++i;
      in_addr ia;
      if (inet_aton(argv[i], &ia)!=0) {
        addr = ia.s_addr;
      } else {
        struct hostent *hostinfo;
        hostinfo = gethostbyname(argv[i]);
        if (hostinfo==0) {
          fprintf(stderr, "%s: could not resolve hostname '%s'\n", argv[0], argv[i]);
          return EXIT_FAILURE;
        }
        addr = (*(struct in_addr*)hostinfo->h_addr).s_addr;
      }
    } else
    if (strcmp(argv[i], "--port")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      port = atoi(argv[++i]);
    } else
    if (strcmp(argv[i], "--out")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      out = argv[++i];
    } else
    if (strcmp(argv[i], "--slow")==0) {
      slow = true;
    } else
    if (strcmp(argv[i], "--help")==0) {
      usage();
      return EXIT_SUCCESS;
    } else
    if (chrootOrUser(argc, argv, &i, &cug)) {
    } else {
      fprintf(stderr, "%s: unknown argument %s, please try --help\n",
                      argv[0], argv[i]);
      return EXIT_FAILURE;
    }
  }

  if (gethostname(hostname, sizeof(hostname)) == -1) {
    perror("gethostname");
    return EXIT_FAILURE;
  }

  int sock = createSocket(addr, port);

  // change root, uid, gid
  if (!setChrootUidGid(&cug))
    return EXIT_FAILURE;
  
  while(true) {
    sockaddr_in cname;
    socklen_t clen = sizeof(cname);
    int client = accept(sock, (sockaddr*)&cname, &clen);
    if (client < 0) {
      perror("accept");
      continue;
    }
    handleClient(argc, argv, client);
  }
  return EXIT_SUCCESS;
}

/**
 * Handle the communication with the client and invoke 'queueMessage' to copy
 * an email to mailgrave-queue.
 */
void
handleClient(int argc, char** argv, int client)
{
  bool forked = false;
//  pid_t pid = fork();
  if (false/*pid==0*/) {
    close(client);
    return;
  }

  // fcntl(client, F_SETFL, O_NONBLOCK);
  //close(STDIN_FILENO);
  //close(STDOUT_FILENO);
  //close(STDERR_FILENO);
  string fromToList;
  const char *line = 0;
  unsigned state = 0;
  int cmd = 0;
  while(true) {
    switch(state) {
      case 0: {
        if (slow) {
          fprintf(stderr, "****** wait 6min before sending greeting\n");
          sleep(6*60);
        }
        char buffer[4096];
        snprintf(buffer, sizeof(buffer), "220 %s ESMTP MailGrave\r\n", hostname);
        if (write(client, buffer, strlen(buffer))==-1) {
          perror("write");
          close(client);
          return;
        }
        state = 1;
      } break;
      case 1:
        if (cmd==CMD_HELO || cmd==CMD_EHLO) {
          write(client, "250 welcome\r\n", 13);
          state = 2;
        } else {
          write(client, "503 bad sequence of commands\r\n", 30);
        }
        break;
      case 2:
        if (cmd==CMD_MAIL_FROM) {
          if (getAddress(line+10, 'F', &fromToList)) {
            write(client, "250 ok\r\n", 8);
            state = 3;
          } else {
            write(client, "501 missing or malformed local part\r\n", 37);
          }
        } else {
          write(client, "503 bad sequence of commands\r\n", 30);
        }
        break;
      case 3:
        if (cmd==CMD_RCPT_TO) {
          if (getAddress(line+8, 'T', &fromToList)) {
            write(client, "250 ok\r\n", 8);
            state = 4;
          } else {
            write(client, "501 missing or malformed local part\r\n", 37);
          }
        } else {
          write(client, "503 bad sequence of commands\r\n", 30);
        }
        break;
      case 4:
        if (cmd==CMD_RCPT_TO) {
          if (getAddress(line+8, 'T', &fromToList)) {
            write(client, "250 ok\r\n", 8);
          } else {
            write(client, "501 missing or malformed local part\r\n", 37);
          }
        } else
        if (cmd==CMD_DATA) {
//fprintf(stderr, "%s:%d\n", __FILE__, __LINE__);
          write(client, "354 Start mail input; end with <CRLF>.<CRLF>\r\n", 46);
          if (queueMessage(client, fromToList)) {
//fprintf(stderr, "%s:%d\n", __FILE__, __LINE__);
            write(client, "250 queued\r\n", 12);
          } else {
//fprintf(stderr, "%s:%d\n", __FILE__, __LINE__);
          }
          state = 2;
        } else {
          write(client, "503 bad sequence of commands\r\n", 30);
        }
        break;
    }
    while(true) {
//fprintf(stderr, "%s: wait for client\n", argv[0]);
      line = getline(client);
//fprintf(stderr, "%s: got from client '%s'\n", argv[0], line);
      if (!line)
        break;
      if (strncmp(line, "HELO ", 5)==0)
        cmd = CMD_HELO;
      else if (strncmp(line, "EHLO ", 5)==0)
        cmd = CMD_EHLO;
      else if (strncmp(line, "MAIL FROM:", 10)==0)
        cmd = CMD_MAIL_FROM;
      else if (strncmp(line, "RCPT TO:", 8)==0)
        cmd = CMD_RCPT_TO;
      else if (strcmp(line, "DATA")==0)
        cmd = CMD_DATA;
      else if (strncmp(line, "QUIT", 4)==0) {
        write(client, "221 Bye\r\n", 9);
        close(client);
        if (forked)
          exit(EXIT_SUCCESS);
        else {
          return;
        }
      } else {
        write(client, "500 unknown command\r\n", 21);
        printf("received unknown command: ");
        const char *p = line;
        while(*p) {
          if (*p>=32)
            printf("%c", *p);
          else
            printf("\\x%02x", *p);
          ++p;
        }
        printf("\n");
        continue;
      }
      break;
    }
    if (!line)
      break;
  }
  close(client);
}

/**
 * \param client
 *   TCP socket connection with the client were the 'DATA' was given.
 * \param fromToList
 *   Envelope data collected from the client to be send to mailgrave-queue.
 */
bool
queueMessage(int client, const string &fromToList)
{
  struct sockaddr_un control;
  control.sun_family = AF_UNIX;
  if (strlen(out) >= sizeof(control.sun_path)) {
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    fprintf(stderr, "path name for control socket is too long.\n");
    return false;
  }
  strcpy(control.sun_path, out);
  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock<0) {
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    perror ("failed to create unix domain socket");
    return false;
  }
  if (connect(sock, (struct sockaddr *) &control,
              offsetof(struct sockaddr_un, sun_path)+
              strlen(control.sun_path)) < 0)
  {
    close(sock);
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    perror("failed to connect to socket");
    return false;
  }

  FILE *out = fdopen(sock, "w");
  if (!out) {
    close(sock);
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    perror("fdopen failed");
    return false;
  }
  
  // 1st: stuff the envelope data into the socket
  fwrite(fromToList.data(), 1, fromToList.size(), out);
  putc_unlocked(0, out);
  
  // 2nd: copy data to the queue
  bool r = copyData(out, client);
  
  // 3rd: shutdown write connection to the queue to signal end of data and
  //      read the result code
  fflush(out);

  char result;
  if (shutdown(sock, SHUT_WR)==-1) {
    perror("mailgrave-smtpd: shutdown");
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    r = false;
  } else
  if (read(sock, &result, 1)!=1) {
    perror("mailgrave-smtpd: unabled to read queue process result\n");
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    r = false;
  } else
  if (!result) {
    printf("mailgrave-send: delivery to queue failed\n");
    write(client, "451 Requested action aborted: local error in processing\r\n", 58);
    r = false;
  }

  fclose(out);

  return r;
}

bool
copyData(FILE *out, int client)
{
  // 2nd: copy the mail data
  unsigned state = 0;
  while(true) {
    char buffer[4096];
    ssize_t l = read(client, buffer, sizeof(buffer));
    if (l<0) {
      if (errno==EINTR)
        continue;
      write(client, "451 Requested action aborted: local error in processing\r\n", 58);
      perror("while reading from client");
      // TODO: append a single byte as an error code to 'out'
      return false;
    }
    for(const char *p = buffer; p != buffer + l; ++p) {
      int c = *p;
      switch(state) {
        case 0:
          if (c=='\r') {
            state = 1;
          } else {
            putc(c, out);
          }
          break;
        case 1: // \r?
          switch(c) {
            case '\n':
              state = 2;
              break;
            case '\r':
              putc('\r', out);
              break;
            default:
              putc('\r', out);
              putc(c, out);
              state = 0;
          }
          break;
        case 2: // \r\n?
          switch(*p) {
            case '.':
              state = 3;
              break;
            case '\r':
              putc('\r', out);
              putc('\n', out);
              state = 1;
              break;
            default:
              putc('\r', out);
              putc('\n', out);
              putc(c, out);
              state = 0;
          }
          break;
        case 3: // \r\n.?
          switch(*p) {
            case '\r':
              state = 4;
              break;
            default:
              putc('\r', out);
              putc('\n', out);
              // skip '.' at beginning of line which does not mark end of data
              putc(c, out);
              state = 0;
          }
          break;
        case 4: // \r\n.\r?
          switch(*p) {
            case '\n':
              if (p+1 != buffer+l) {
                write(client, "554 trailing data after data\r\n", 30);
                return false;
              }
              return true;
              break;
            case '\r':
              putc('\r', out);
              putc('\n', out);
              putc('.', out);
              putc('\r', out);
              state = 1;
              break;
            default:
              putc('\r', out);
              putc('\n', out);
              putc('.', out);
              putc(c, out);
              state = 0;
          }
          break;
      }
    }
  }
  write(client, "451 Requested action aborted: local error in processing\r\n", 58);
  return false;
}


const char*
getline(int fd)
{
  static char buffer[cmdline_maxsize+1];
  char *p = buffer;
  ssize_t n = cmdline_maxsize+1;
  while(true) {
    ssize_t l = read(fd, p, n);
    if (l<0) {
      if (errno==EINTR)
        continue;
      perror("while reading from client");
      write(fd, "554 Transaction failed\r\n", 24);
      return 0;
    }
    if (l==0) {
      fprintf(stderr, "lost connection to client\n");
      return 0;
    }
    if (l==n) {
      write(fd, "500 Line too long.\r\n", 20);
      return 0;
    }
    if (p[l-2]=='\r' && p[l-1]=='\n') {
      p[l-2] = 0;
      return buffer;
    }
    n -= l;
    p += l;
  }
  *p = 0;
  return 0;
}

/**
 * RFC 2821 states, that the address may contain source routes,
 * which the algorithm below can't detect.
 *
 * \param line
 *   The part after 'MAIL FROM:' and 'RCPT TO:'.
 * \param c
 *   'F' or 'T'
 * \param result
 *   The result.
 */
bool
getAddress(const char *line, char c, string *result)
{
  const char *p0, *p1;
  for(p0 = line; *p0 != '<' && *p0 !=0 ; ++p0);
  if (*p0==0)
    return false;
  ++p0;
  for(p1 = p0; *p1 != '>' && *p1 !=0; ++p1);
  if (*p1==0)
    return false;
    
  result->append(1, c);
  result->append(p0, p1-p0);
  result->append(1, (char)0);
  return true;
}

  

/**
 * Create TCP Server Socket
 */
int
createSocket(in_addr_t addr, int port)
{
  int sock;

  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock==-1) {
    perror("while creating tcp socket");
    exit(EXIT_FAILURE);
  }
   
  int yes = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))<0) {
    perror("failed to set SO_REUSEADDR");
  }
   
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(int))<0) {
    perror("failed to set TCP_NODELAY");
  }
   
  sockaddr_in name;
  name.sin_family = AF_INET;
  name.sin_addr.s_addr = addr;
  name.sin_port   = htons(port);
  if (bind(sock, (sockaddr*) &name, sizeof(sockaddr_in)) < 0) {
    perror("while binding to tcp socket");
    exit(EXIT_FAILURE);
  }
   
  if (listen(sock, 20)==-1) {
    perror("while starting to listen on tcp socket");
    exit(EXIT_FAILURE);
  }
   
  return sock;
}
