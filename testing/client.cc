#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

ssize_t mygets(char*, size_t n, int fd);

ssize_t
mygets(char *b, size_t n, int fd)
{
  ssize_t r = 0;
  while(true) {
    char c;
    ssize_t l = read(fd, &c, 1);
    if (l<0) {
      perror("mygets");
      exit(1);
    }
    if (l==0)
      continue;
    if (c=='\r')
      continue;
    if (c=='\n')
      break;
    b[r++]=c;
  }
  b[r]=0;
  return r;
}

int
main(int argc, char **argv)
{
  int s=socket(AF_INET, SOCK_STREAM, 0);
  if (s<0) {
    perror("client: socket");
    exit(1);
  }
  sockaddr_in name;
  in_addr ia;
  inet_aton("127.0.0.1", &ia);
  name.sin_addr.s_addr = ia.s_addr;
  name.sin_family = AF_INET;
  name.sin_port   = htons(2525);
  if (connect(s, (sockaddr*) &name, sizeof(sockaddr_in)) < 0) {
    perror("client: connect");
    exit(1);
  }
  
  char buffer[4096];
  mygets(buffer, sizeof(buffer), s);
  printf("client received '%s'\n", buffer);

  for(int i=1; i<argc; ++i) {
    if (strcmp(argv[i], "helo")==0) {
      ++i;
      write(s, "HELO ", 5);
      write(s, argv[i], strlen(argv[i]));
      write(s, "\r\n", 2);
    } else
    if (strcmp(argv[i], "mailfrom")==0) {
      ++i;
      write(s, "MAIL FROM:", 10);
      write(s, argv[i], strlen(argv[i]));
      write(s, "\r\n", 2);
    } else
    if (strcmp(argv[i], "rcptto")==0) {
      ++i;
      write(s, "RCPT TO:", 8);
      write(s, argv[i], strlen(argv[i]));
      write(s, "\r\n", 2);
    } else
    if (strcmp(argv[i], "data")==0) {
      ++i;
      write(s, "DATA\r\n", 6);

      mygets(buffer, sizeof(buffer), s);
      printf("client received '%s'\n", buffer);
    
      write(s, argv[i], strlen(argv[i]));
      write(s, "\r\n.\r\n", 5);
    } else
    if (strcmp(argv[i], "quit")==0) {
      write(s, "QUIT\r\n", 6);
    } else {
      fprintf(stderr, "unknown option '%s'\n", argv[i]);
      exit(1);
    }

    mygets(buffer, sizeof(buffer), s);
    printf("client received '%s'\n", buffer);
  }
  
  close(s);
  return 0;
}
