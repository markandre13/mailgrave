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

/*
 * This program takes an eMail from stdin, makes some header adjustments
 * and calls mailgrave-queue to stuff the result into the mail queue.
 *
 * The worst part in this program is RFC 822 and RFC 2822 conform parsing
 * of the From:, To:, Cc: and Bcc: header entries into a minimal form.
 *
 */

#include "opensocket.hh"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <pwd.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <string>
using std::string;

extern unsigned parseAddress(FILE *in, string *r, char c, bool z, string *o);

static void
usage()
{
  printf(
    "mailgrave-inject\n"
    "Copyright (C) 2006, 2007 Mark-André Hopf <mhopf@mark13.org>\n"
    "Visit http://mark13.org/mailgrave/ for full details.\n"
    "\n"
    "Usage:\n"
    "  This command takes a mail from stdin and places it into the mail\n"
    "  queue in case nothing goes wrong.\n"
    "\n"
    "Options:\n"
    "  --file <filename>\n"
    "    Use the specified file instead of stdin.\n"
    "  --out <socket>\n"
    "    The socket of mailgrave-queue.\n"
    "  --dry-run\n"
    "    Print result instead of putting it into the queue.\n"
  );
}

int
main(int argc, char **argv)
{
  setvbuf(stdout, 0, _IONBF, 0);

  const char *filename = 0;
  bool dryrun = false;
  const char *name = "queue.ctrl";

  // parse argument list
  for(int i=1; i<argc; ++i) {
    if (strcmp(argv[i], "--file")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      filename = argv[++i];
    } else
    if (strcmp(argv[i], "--out")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      name = argv[++i];
    } else
    if (strcmp(argv[i], "--dry-run")==0) {
      dryrun = true;
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
  
  // preparations
  FILE *in = stdin;
  if (filename) {
    in = fopen(filename, "r");
    if (!in) {
      fprintf(stderr, "failed to open '%s': %s\n",
                      filename, strerror(errno));
      return EXIT_FAILURE;
    }
  }

  char host[MAXHOSTNAMELEN];
  if (gethostname(host, sizeof(host)) == -1) {
    perror("gethostname");
    return EXIT_FAILURE;
  }
  
  const char *user = "anonymous";
  struct passwd *pw;
  if ((pw = getpwuid(getuid())) != NULL) {
    if ((user = strdup(pw->pw_name)) == NULL) {
      perror("strdup");
      return EXIT_FAILURE;
    }
  }
  
  bool expand_lf2crlf = true;
  
  FILE *out = stdout;

  // open mailgrave-queue's socket to deliver mail
  int sock = -1;
  if (!dryrun) {
    sock = openUNIXSocket(name);
    if (sock<0) {
      exit(EXIT_FAILURE);
    }
    out = fdopen(sock, "w");
  }
  
  // rewrite header
  // - header name is case insensitive
  // - From:
  // - To:, Cc, Bcc
  // - Bcc: is dropped from the header
  // - Date: is added if missing
  // - Message-Id: is added if missing
  // - Received: qmail-queue is doing that for us
  // - Return-Path: removed
  // - Content-Length: removed
  
  char buffer[20];
  char lbuffer[20];
  size_t bp = 0;

  string toList;
  string fromList;

  bool has_date = false;
  bool has_message_id = false;

  enum {
    STATE_HEADER_FIELD_NAME,
    
    STATE_COPY,
    STATE_COPY_0,
    STATE_COPY_1,
    
    STATE_DROP,
    STATE_DROP_0,
    STATE_DROP_1,
    
    STATE_EMAIL,
    
    STATE_DATA,
    STATE_DATA_0,
    STATE_DATA_1
  } state = STATE_HEADER_FIELD_NAME;
  
  bool drop;
  bool from;
  string header;

  while(true) {
    int c = getc_unlocked(in);
    if (c==EOF)
      break;
//printf("main: %i '%c' '%i'\n", state, c, c);
    switch(state) {
      case STATE_HEADER_FIELD_NAME:
        if (expand_lf2crlf && c=='\n') {
          fwrite(fromList.data(), fromList.size(), 1, out);
          fwrite(toList.data(), toList.size(), 1, out);
          putc_unlocked(0, out);
          fwrite(header.data(), header.size(), 1, out);
          header.clear();
          state = STATE_DATA_0;
          break;
        }
        if (c=='\r') {
          fwrite(fromList.data(), fromList.size(), 1, out);
          fwrite(toList.data(), toList.size(), 1, out);
          putc_unlocked(0, out);
          fwrite(header.data(), header.size(), 1, out);
          header.clear();
          state = STATE_DATA;
          break;
        }
        if (c<=32 || c>=126) {
          fprintf(stderr, "Non-printable ASCII character in header field name\n");
          exit(EXIT_FAILURE);
        }
        if (c==':') {
          drop = false;
          from = false;
          lbuffer[bp] = 0;
          if (strcmp(lbuffer, "from")==0) {
            state = STATE_EMAIL;
            from = true;
          } else
          if (strcmp(lbuffer, "to")==0) {
            state = STATE_EMAIL;
          } else
          if (strcmp(lbuffer, "cc")==0) {
            state = STATE_EMAIL;
          } else
          if (strcmp(lbuffer, "bcc")==0) {
            state = STATE_EMAIL;
            drop = true;
          } else
          if (strcmp(lbuffer, "date")==0) {
            has_date = true;
            state = STATE_COPY;
          } else
          if (strcmp(lbuffer, "message-id")==0) {
            has_message_id = true;
            state = STATE_COPY;
          } else
          if (strcmp(lbuffer, "return-path")==0) {
            state = STATE_DROP;
          } else
          if (strcmp(lbuffer, "content-length")==0) {
            state = STATE_DROP;
          } else {
            state = STATE_COPY;
          }
          if (!drop) {
            buffer[bp] = ':';
            // fwrite(buffer, bp+1, 1, out);
            header.append(buffer, bp+1);
          }
          bp = 0;
          if (state == STATE_EMAIL) {
            string addresses;
            string o;
            unsigned n = parseAddress(in, &addresses, from ? 'F' : 'T', true, &o);
            if (!drop) {
              // fprintf(out, "%s", o.c_str());
              header.append(o);
            }
            if (from) {
              if (n>1 || !fromList.empty()) {
                fprintf(stderr, "only one From: entry allowed\n");
                exit(EXIT_FAILURE);
              }
              fromList += addresses;
            } else {
              toList += addresses;
            }
            state = STATE_HEADER_FIELD_NAME;
            continue;
          }
          break;
        }
        buffer[bp] = c;
        lbuffer[bp] = tolower(c);
        ++bp;
        if (bp==sizeof(buffer)-1) {
          state = STATE_COPY;
          bp = 0;
        }
        break;

      case STATE_COPY:
        if (expand_lf2crlf && c=='\n') {
          //putc_unlocked('\r', out);
          //putc_unlocked('\n', out);
          header += "\r\n";
          state = STATE_COPY_1;
          break;
        }
        // putc_unlocked(c, out);
        header += c;
        if (c=='\r')
          state = STATE_COPY_0;
        break;
      case STATE_COPY_0:
        // putc_unlocked(c, out);
        header += c;
        if (c=='\n')
          state = STATE_COPY_1;
        if (c!='\r')
          state = STATE_COPY;
        break;
      case STATE_COPY_1:
        if (c==' ' || c=='\t') {
          // putc_unlocked(c, out);
          header += c;
          state = STATE_COPY;
        } else {
          ungetc(c, in);
          state = STATE_HEADER_FIELD_NAME;
        }
        break;

      case STATE_DROP:
        if (expand_lf2crlf && c=='\n') {
          state = STATE_DROP_1;
          break;
        }
        if (c=='\r')
          state = STATE_DROP_0;
        break;
      case STATE_DROP_0:
        if (c=='\n')
          state = STATE_DROP_1;
        if (c!='\r')
          state = STATE_DROP;
        break;
      case STATE_DROP_1:
        if (c==' ' || c=='\t') {
          state = STATE_DROP;
        } else {
          ungetc(c, in);
          state = STATE_HEADER_FIELD_NAME;
        }
        break;

      case STATE_DATA:
        if (c!='\n') {
          fprintf(stderr, "Non-printable ASCII character in header field name\n");
          exit(1);
        }
        putc_unlocked('\n', out);
        state = STATE_DATA_0;
        break;
      case STATE_DATA_0:
        if (!has_message_id) {
          fprintf(out, "Message-Id: <%lu.%lu.mailgrave@%s>\r\n",
                       (u_long)time(NULL), (u_long)getpid(), host);
        }
        if (!has_date) {
          char str[256];
          time_t now = time(NULL);
          if (strftime(str, sizeof(str), "%a, %d %b %Y %T %z",
                       localtime(&now)) == 0)
          {
            perror("strftime");
          }
          fprintf(out, "Date: %s\r\n", str);
        }
        putc_unlocked('\r', out);
        putc_unlocked('\n', out);
        putc_unlocked(c, out);
        state = STATE_DATA_1;
        break;
      case STATE_DATA_1:
        putc_unlocked(c, out);
        break;
      default:
        fprintf(stderr, "unexpected parser state\n");
        exit(EXIT_FAILURE);
    }
  }
  
  if (!dryrun) {
    // shutdown write connection to the queue to signal end of data and
    // read the result code
    fflush(out);

    char result;
    if (shutdown(sock, SHUT_WR)==-1) {
      perror("mailgrave-inject: shutdown");
      exit(EXIT_FAILURE);
    } else
    if (read(sock, &result, 1)!=1) {
      perror("mailgrave-inject: unabled to read queue process result\n");
      exit(EXIT_FAILURE);
    } else
    if (!result) {
      fprintf(stderr, "mailgrave-inject: delivery to queue failed\n");
      exit(EXIT_FAILURE);
    };
    fclose(out);
  } else {
    for(string::const_iterator p = fromList.begin();
        p != fromList.end();
        ++p)
    {
      if (*p==0)
        printf("\\0");
      else
        printf("%c", *p);
    }
    printf("\n");
    for(string::const_iterator p = toList.begin();
        p != toList.end();
        ++p)
    {
      if (*p==0)
        printf("\\0");
      else
        printf("%c", *p);
    }
    printf("\n");
  }

  if (in!=stdin) {
    fclose(in);
  }

  return EXIT_SUCCESS;
}
