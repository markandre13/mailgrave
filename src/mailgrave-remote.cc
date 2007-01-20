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

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/un.h>

#include <string>
#include <vector>

#include "createsocket.hh"
#include "cug.hh"

using std::string;
using std::vector;

// RFC 2821: 
// RFC 2554: SMTP Service Extension for Authentication
// RFC 2222: Simple Authentication and Security Layer (SASL)
// RFC 2245: ANONYMOUS
// RFC 4616: PLAIN
//           <authorization identity>\0<authentication identity>\0<password>
// RFC ????: LOGIN
// RFC 2831: DIGEST-MD5
// RFC 3207: SMTP Service Extension for Secure SMTP over Transport Layer Security

static bool sendMail(FILE *in);
static void sendData(FILE *out, FILE *in);
static bool parseResponse(FILE *server, unsigned *code, string *text, bool *more, unsigned timeout);
static bool parseResponseDoIt(FILE *server, unsigned *code, string *text, bool *more);
static int base64_encode(const char *in, char *out);

static int verbose = 1;

int sock;

static void
usage()
{
  printf(
    "mailgrave-remote [options] <host> <sender> <recipient> [<recipient>...]\n"
    "Copyright (C) 2006 Mark-Andre Hopf <mhopf@mark13.org>\n"
    "Visit http://mark13.org/mailgrave/ for full details.\n"
    "\n"
    "Usage:\n"
    "  Copy an email from stdin to a SMTP server.\n"
    "Options:\n"
    "  --file <file>\n"
    "    Use the specified file instead of stdin.\n"
    "  --relay <server>\n"
    "    don't deliver directly, use the specified relay server instead\n"
    "  --port <port>\n"
    "    SMTP port of the remote server\n"
    "  --login <login>\n"
    "    A login in case the server requires ESMTP auth\n"
    "  --password <password>\n"
    "    A password in case the server requires ESMTP auth.\n"
    "    You should use the environment variable SMTP_AUTH_PASSWORD instead\n"
    "    of this option as parameters on the command line may be visible to\n"
    "    other users on the same computer.\n"
    "  --verbose | -v\n"
    "    Print the dialog with the SMTP server\n"
    "  --help\n"
    "    show this help text\n"
  );
}

static void io_putc(FILE *f, int c);
static void io_put(FILE *f, const char *s);
static void io_flush(FILE *f);

static const char *login    = getenv("SMTP_AUTH_LOGIN");
static const char *password = getenv("SMTP_AUTH_PASSWORD");
static const char *relay = 0;
static int port = 25;

// various minimal timeouts as suggested by RFC 2821, 4.5.3.2 Timeouts
static unsigned timeout_initial = 5 * 60;
static unsigned timeout_helo = 5 * 60;
static unsigned timeout_auth = 5 * 60;
static unsigned timeout_mail = 5 * 60;
static unsigned timeout_rcpt = 5 * 60;
static unsigned timeout_data_init = 2 * 60;
static unsigned timeout_data_block = 3 * 60;
static unsigned timeout_data_term = 10 * 60;
static unsigned timeout_quit = 5 * 60;

int
main(int argc, char **argv)
{
  setvbuf(stdout, 0, _IONBF, 0);

  cug_t cug;
  const char *name = "remote.ctrl";
  int firstarg = argc;
  FILE *in  = stdin;

  // parse argument list
  for(int i=1; i<argc; ++i) {
    if (strcmp(argv[i], "--in")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      name = argv[++i];
    } else
    if (chrootOrUser(argc, argv, &i, &cug)) {
    } else
    if (strcmp(argv[i], "--file")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      in = fopen(argv[++i], "r");
      if (!in) {
        perror("failed to open data file");
        return EXIT_FAILURE;
      }
    } else
    if (strcmp(argv[i], "--relay")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      relay = argv[++i];
    } else
    if (strcmp(argv[i], "--login")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      login = argv[++i];
    } else
    if (strcmp(argv[i], "--password")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      password = argv[++i];
    } else
    if (strcmp(argv[i], "--verbose")==0 || strcmp(argv[i], "-v")==0) {
      ++verbose;
    } else
    if (strcmp(argv[i], "--port")==0) {
      if (i+1 >= argc) {
        fprintf(stderr, "%s: not enough arguments for %s\n", argv[0], argv[i]);
        return EXIT_FAILURE;
      }
      port = atoi(argv[++i]);
    } else
    if (strcmp(argv[i], "--help")==0) {
      usage();
      return EXIT_SUCCESS;
    } else {
      firstarg = i;
      break;
    }
  }

  // create socket
  int sock = createUNIXSocket(name);
  if (sock<0) {
    perror ("failed to create unix domain socket");
    return EXIT_FAILURE;
  }

  if (listen(sock, 5) < 0) {
    perror("control listen");
    close(sock);
    return EXIT_FAILURE;
  }

  if (chown(name, cug.uid, cug.gid)!=0) {
    perror("chown");
    close(sock);
    return EXIT_FAILURE;
  }

  // change root, uid, gid
  if (!setChrootUidGid(&cug))
    return EXIT_FAILURE;
  
  // message loop  
  while(true) {
    struct sockaddr_un addr;
    socklen_t addrlen = sizeof(addr);
    int client = accept(sock, (struct sockaddr*) &addr, &addrlen);
    printf("%s: got job\n", argv[0]);
    // we should fork here...
    FILE *in = fdopen(client, "r");
    if (sendMail(in)) {
      char x = 1;
      write(client, &x, 1);
    } else {
      char x = 0;
      write(client, &x, 1);
    }
    fclose(in);
  }
  return EXIT_SUCCESS;
}

bool
sendMail(FILE *in)
{
  bool result = false;

  bool auth = false;
  bool has_tls = false;
  bool has_plain = false;
  bool has_login = false;

  // fetch host, sender and receipients from the socket:
  // <host>\0
  // <sender>\0
  // <recipient>\0...\0
  // <data>
  
  string host, sender, tmp;
  vector<string> receipients;
  
  unsigned state=0;
  while(state!=255) {
    int c = fgetc(in);
    if (c==EOF) {
      return result;
    }
    switch(state) {
      case 0: // collect 'host'
        if (c==0) {
          state = 1;
          if (verbose)
            printf("mailgrave-remote: got host '%s'\n", host.c_str());
          continue;
        }
        host += c;
        break;
      case 1: // collect 'sender'
        if (c==0) {
          state = 2;
          if (verbose)
            printf("mailgrave-remote: got sender '%s'\n", sender.c_str());
          continue;
        }
        sender += c;
        break;
      case 2: // collect receipients
        if (c==0) {
          if (verbose)
            printf("mailgrave-remote: got %lu recipients\n", (unsigned long)receipients.size());
          state = 255;
          continue;
        }
        tmp += c;
        state = 3;
        break;
      case 3: // collect single receipient
        if (c==0) {
          state = 2;
          receipients.push_back(tmp);
          if (verbose)
            printf("mailgrave-remote: got receipient '%s'\n", tmp.c_str());
          tmp.clear();
          continue;
        }
        tmp += c;
        break;
    }
  }

  // connect to the server
  sockaddr_in name;
  in_addr ia;
  if (inet_aton(relay, &ia)!=0) {
    name.sin_addr.s_addr = ia.s_addr;
  } else {
    struct hostent *hostinfo;
    hostinfo = gethostbyname(relay);
    if (hostinfo==0) {
      fprintf(stderr, "Failed to resolve hostname '%s'\n", relay);
      return result;
    }
    name.sin_addr = *(struct in_addr *) hostinfo->h_addr;
  }
  name.sin_family = AF_INET;
  name.sin_port   = htons(port);

  sock = socket (AF_INET, SOCK_STREAM, 0);
  if (sock==-1) {
    perror("Failed to create socket");
    return result;
  }
  
  if (connect(sock, (sockaddr*) &name, sizeof(sockaddr_in)) < 0) {
    printf("mailgrave-remote: failed to connect to '%s:%d': %s\n",
           relay, port, strerror(errno));
    return result;
  }

  char myhost[MAXHOSTNAMELEN];
  if (gethostname(myhost, sizeof(myhost)) == -1) {
    perror("gethostname failed");
    return result;
  }

  FILE *server = fdopen(sock, "r+");
  if (!server) {
    perror("fdopen failed");
    return result;
  }

  // communicate with the server
  unsigned code;
  string text;
  bool more;

  // read server name
  parseResponse(server, &code, &text, 0, timeout_initial);
  if (code!=220) {
    printf("Server send error %03u %s\n", code, text.c_str());
    goto error1;
  }
  
  // send EHLO
  io_put(server, "EHLO ");
  io_put(server, myhost);
  io_put(server, "\r\n");
  io_flush(server);
  
  // parse HELO/EHLO response (may be multiple lines, thus the loop)
  do {
    parseResponse(server, &code, &text, &more, timeout_helo);
    if (code!=250) {
      printf("Server send error %03u %s\n", code, text.c_str());
      goto error1;
    }
    if (text=="STARTTLS") {
      has_tls = true;
    } else
    if (text.compare(0, 5, "AUTH ", 5)==0) {
      auth = true;
      string::size_type i0 = 5, i1;
//printf("split '%s\n", text.c_str());
      while(true) {
        i1 = text.find(' ', i0);
        string name = text.substr(i0, i1-i0);
//printf("  i0=%d, i1=%d, '%s'\n", i0, i1, name.c_str());
        if (name=="PLAIN") {
          has_plain = true;
        } else
        if (name=="LOGIN") {
          has_login = true;
        }
        if (i1==string::npos)
          break;
        i0 = i1 + 1;
      }
    }
  } while(more);
  
  if (!password || !login) {
    auth = false;
  }
  
  // STARTTLS
  if (has_tls) {
  }

  // ANONYMOUS, PLAIN, LOGIN, CRAM-MD5, DIGEST-MD5, GSSAPI, NTLM

  if (auth && has_login) {
    char b64[(strlen(login)+strlen(password))*2];

    io_put(server, "AUTH LOGIN\r\n");
    io_flush(server);
    
    parseResponse(server, &code, &text, 0, timeout_auth);
    if (code != 334) {
      io_put(server, "QUIT\r\n");
      io_flush(server);
      printf("Connected to '%s' but AUTH LOGIN was rejected.\n", host.c_str());
      goto error1;
    }
    
    base64_encode(login, b64);
    io_put(server, b64);
    io_put(server, "\r\n");
    io_flush(server);

    parseResponse(server, &code, &text, 0, timeout_auth);
    if (code != 334) {
      io_put(server, "QUIT\r\n");
      io_flush(server);
      printf("Connected to '%s' but AUTH LOGIN's username was rejected.\n", host.c_str());
      goto error1;
    }

    base64_encode(password, b64);
    io_put(server, b64);
    io_put(server, "\r\n");
    io_flush(server);

    parseResponse(server, &code, &text, 0, timeout_auth);
    if (code != 235) {
      io_put(server, "QUIT\r\n");
      io_flush(server);
      printf("Connected to '%s' but AUTH LOGIN' password was rejected.\n", host.c_str());
      goto error1;
    }
  }

  io_put(server, "MAIL FROM:<");
  io_put(server, sender.c_str());
  io_put(server, ">\r\n");
  io_flush(server);

  parseResponse(server, &code, &text, 0, timeout_mail);
  if (code != 250) {
    io_put(server, "QUIT\r\n");
    io_flush(server);
    printf("Connected to '%s' but MAIL FROM was rejected.\n", host.c_str());
    goto error1;
  }
  
  for(vector<string>::const_iterator p = receipients.begin();
      p != receipients.end();
      ++p)
  {
    io_put(server, "RCPT TO:<");
    io_put(server, p->c_str());
    io_put(server, ">\r\n");
    io_flush(server);

    parseResponse(server, &code, &text, 0, timeout_rcpt);
    if (code != 250) {
      printf("Connected to '%s' but RCPT TO was rejected.\n", host.c_str());
      io_put(server, "QUIT\r\n");
      io_flush(server);
      parseResponse(server, &code, &text, 0, timeout_quit);
      if (code != 221) {
        printf("QUIT was rejected. (ignored)\n");
      }
      goto error1;
    }
  }
  
  io_put(server, "DATA\r\n");
  io_flush(server);
  
  parseResponse(server, &code, &text, 0, timeout_data_init);
  if (code != 354) {
    printf("Connected to '%s' but DATA was rejected.\n", host.c_str());
    goto error1;
  }
  
  sendData(server, in);
  
  parseResponse(server, &code, &text, 0, timeout_data_term);
  if (code != 250) {
    printf("Connected to '%s' DATA was rejected.\n", host.c_str());
    goto error1;
  }
  
  io_put(server, "QUIT\r\n");
  io_flush(server);
  
  parseResponse(server, &code, &text, 0, timeout_quit);
  if (code != 221) {
    printf("Mail send but QUIT was rejected. (ignored)\n");
  }
  result = true;
error1:  
//  if (in != stdin)
//    fclose(in);
  fclose(server);
  return result;
}

/**
 * Copy email data to the SMTP server.
 *
 * \li convert '\r\n.' to '\r\n..' so a dot at the beginning of a line
 *     isn't confused with end of data
 * \li convert single '\n' to '\r\n'
 */
void
sendData(FILE *out, FILE *in)
{  
  if (verbose>0)
    printf("BEGIN OF DATA\n");
  unsigned state = 0;
  while(state!=100) {
    int c = getc_unlocked(in);
    switch(state) {
      case 0: // BOL
        switch(c) {
          case EOF:
            state = 100;
            break;
          case '\n':
            io_putc(out, '\r');
            io_putc(out, c);
            break;
          case '\r':
            io_putc(out, c);
            state = 2;
            break;
          case '.':
            io_putc(out, '.');
            io_putc(out, c);
            state = 1;
            break;
          default:
            io_putc(out, c);
            state = 1;
        }
        break;

      case 1: // behind BOL
        switch(c) {
          case EOF:
            io_putc(out, '\r');
            io_putc(out, '\n');
            state = 100;
            break;
          case '\n':
            io_putc(out, '\r');
            io_putc(out, c);
            state = 0;
            break;
          case '\r':
            io_putc(out, c);
            state = 2;
            break;
          default:
            io_putc(out, c);
        }
        break;

      case 2: // behind '\r'
        io_putc(out, c);
        switch(c) {
          case EOF:
            io_putc(out, '\n');
            state = 100;
            break;
          case '\n':
            state = 0;
            break;
          case '\r':
            break;
          default:
            state = 1;
        }
        break;
    }
  }
  if (verbose>0) {
    printf("END OF DATA\n");
  }
  io_put(out, ".\r\n");
  io_flush(out);
}

/**
 * Parse a single server response line.
 * In case of an error, an message is printed and exit(0) invoked.
 *
 * \param server
 *   stream with the server socket
 * \param code
 *   out: the SMTP result code
 * \param text
 *   out: additional text after the result code
 * \param: more
 *   out: returns true, when this response line is followed by another one;
 *   it is an error if another line will follows and 'more' is NULL.
 */
static bool timeout_flag;

void alarm_cb(int) {
  printf("alarm_cb\n");
  timeout_flag = true;
}

bool
parseResponse(FILE *server, unsigned *aCode, string *text, bool *more, unsigned timeout)
{
  *aCode = 0;
  timeout_flag = false;
  alarm(timeout);
//alarm(5);
  struct sigaction sig;
  sig.sa_handler = &alarm_cb;
  sig.sa_flags   = 0;
  sigemptyset(&sig.sa_mask);
  sigaction(SIGALRM, &sig, 0);
  bool r = parseResponseDoIt(server, aCode, text, more);
  alarm(0);
  sig.sa_handler = SIG_DFL;
  sigaction(SIGALRM, &sig, 0);
  if (timeout_flag) {
    printf("parseResponse timed out\n");
    r = false;
  }
  if (verbose)
    fflush(stdout);
  return r;
}

bool
parseResponseDoIt(FILE *server, unsigned *aCode, string *text, bool *more)
{
printf("reading server response\n");
  unsigned state = 0;
  unsigned code = 0;
  if (more)
    *more = false;
  text->clear();
  while(true) {
    int c = getc_unlocked(server);
    if (c<0) {
      if (errno==EINTR) {
        if (!timeout_flag)
          continue;
        else
          return false;
      }
      perror("getc failed");
      return false;
    }
//    int c = fgetc(server);
//    char c;
//    read(sock, &c, 1);
    if (verbose>0) {
      switch(c) {
        case '\r':
          printf("\\r");
          break;
        case '\n':
          printf("\\n\n");
          break;
        case '\0':
          printf("\\0");
          break;
        default:
	  putc_unlocked(c, stdout);
      }
    }
    switch(state) {
      case 0: // parse three digit number
        if (!isdigit(c)) {
          printf("Unexpected response: expected 1st digit\n");
          return false;
        }
        code += (c - '0') * 100;
        state = 1;
        break;
      case 1:
        if (!isdigit(c)) {
          printf("Unexpected response: expected 2nd digit\n");
          return false;
        }
        code += (c - '0') * 10;
        state = 2;
        break;
      case 2:
        if (!isdigit(c)) {
          printf("Unexpected response: expected 3rd digit\n");
          return false;
        }
        code += (c - '0');
        state = 3;
        break;
        
      case 3: // parse character after three digit number
        switch(c) {
          case '-':
            if (more) {
              *more = true;
            } else {
              printf("Unexpected response: expected single line response\n");
              return false;
            }
          case ' ':
            state = 4;
            break;
          case '\r':
            state = 5;
            break;
          case '\n':
            printf("Unexpected '\\n' (3)\n");
            return false;
          default:
            printf("Unexpected response: expected ' ' or '\\r'\n");
            return false;
        }
        break;
        
      case 4: // parse text
        switch(c) {
          case '\r':
            state = 5;
            break;
          case '\n':
            printf("Unexpected '\\n' (4)\n");
            return false;
          default:
            *text += c;
        }
        break;
      case 5:
        switch(c) {
          case '\n':
            *aCode = code;
            return true;
          case '\r':
            *text += '\r';
            break;
          default:
            *text += '\r';
            *text += c;
            state = 4;
        }
        break;
    }
  }
  return false;
}

/*
 * methods for writing data to the server which also dump their output
 * on stdout when verbose mode is selected
 */
void
io_putc(FILE *f, int c)
{
  if (verbose>0) {
    if (c=='\r')
      fwrite("\\r", 1, 2, stdout);
    else if (c=='\n')
      fwrite("\\n\n", 1, 3, stdout);
    else
      putc_unlocked(c, stdout);
  }
  putc_unlocked(c, f);
}

void
io_put(FILE *f, const char *s)
{
  if (verbose>0) {
    int l = strlen(s);
    for(int i=0; i<l; ++i)
      io_putc(f, s[i]);
    return;
  }
  fwrite(s, 1, strlen(s), f);
}

void
io_flush(FILE *f)
{
  if (verbose>0)
    fflush(stdout);
  fflush(f);
}

/*
 * BASE64 encoding
 */

static int
base64_encode6(int v)
{
  if (v<26)
    return v+'A';
  if (v<52)
    return v-26+'a';
  if (v<62)
    return v-52+'0';
  if (v==62)
    return '+';
  return '/';
}

static void
base64_encode24(char **out, int d, int n)
{
  int m = 0;
  if (n==0)
    return;
  
  d<<=(24-n);
  n=24;
  
  while(n>0) {
    if (n>18)
      **out = base64_encode6((d>>18)&63);
    else if (n>12)
      **out = base64_encode6((d>>12)&63);
    else if (n>6)
      **out = base64_encode6((d>> 6)&63);
    else
      **out = base64_encode6((d    )&63);
    ++*out;
    ++m;
    n-=6;
  }
  while(m<4) {
    **out = '=';
    ++*out;
    ++m;
  }
}

int
base64_encode(const char *in, char *out)
{
  int i, d, n;
  while(1) {
    d = 0;
    n = 0;
    for(i=0; i<3; ++i) {
      if (!*in)
        break;
      d<<=8;
      d|=*in;
      ++in;
      n+=8;
    }
    base64_encode24(&out, d, n);
    if (i<3)
      break;
  }
  *out = 0;
  return 0;
}
