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

#include <string>
#include <stdio.h>
#include <stdlib.h>

using std::string;

static const int TKN_ATOM = -2;
static const int TKN_QUOTED_STRING = -3;
static const int TKN_DOMAIN_LITERAL = -4;
static const int TKN_EOH = -5;
static string yytext;

static bool parse_address(FILE *in);
static int lex(FILE *in);

static bool eoh = false;

/*
 * For just fetching the email addresses from the header lines, the 
 * address definition in RFC 822 can be simplified to:
 *
 * mailbox := word *("." word) "@" domain
 *         |  1*word "<" [1#("@" domain) ":"] word *("." word) "@" domain ">"
 * group   := 1*word ":" [#mailbox] ";"
 *
 * domain := sub-domain *("." sub-domain)
 * sub-domain  :=  atom | domain-literal
 *
 */

static string address;
static char hchar = 'T';
static unsigned count; 
static bool zero = false;
static string *orig = 0;

/**
 * \param in
 * \param r
 *   string which takes the addresses in the form of
 *   (('T'|'F') address '\0')*
 * \param c
 *   'T' or 'F'
 * \param z
 *   append a zero ('\0') to each address copied to r
 * \param o
 */
unsigned
parseAddress(FILE *in, string *r, char c, bool z, string *o)
{
//printf("----------- start parse -------------\n");
  count = 0;
  zero = z;
  hchar = c;
  orig = o;
  address.clear();
  parse_address(in);
  *r += address;
  orig = 0;
//printf("------------- end parse --------------\n");
  return count;
}

static void
gotOne(const string &s)
{
  if (s.empty())
    return;
//printf("ADDRESS: '%s'\n", s.c_str());
  address += hchar;
  address += s;
  if (zero) {
    address += '\0';
  }
  ++count;
}

bool
parse_address(FILE *in)
{
  string address;
  eoh = false;
  bool ingroup, inroute;
  ingroup = inroute = false;

  int state = 0;
  while(true) {
    int c = lex(in);
//printf("top : %i '%c' (%i) -> '%s' '%s', ingroup=%i, inroute=%i\n", state, c, c, address.c_str(), yytext.c_str(),ingroup,inroute);
    switch(state) {
      case 0:
        switch(c) {
          case TKN_QUOTED_STRING:
          case TKN_ATOM:
            address += yytext;
            state = 1;
            break;
          case TKN_EOH:
            return true;
          case ',':
            address.clear();
            break;
          default:
            printf("unexpected character: '%c'\n", c);
            exit(1);
            break;
        }
        break;
      case 1:
        switch(c) {
          case '.':
            address += c;
            state = 2;
            break;
          case '@':
            address += c;
            state = 4;
            break;
          case '<':
            state = 7;
            break;
          case ':':
            address.clear();
            ingroup = true;
            state = 6;
            break;
          case TKN_QUOTED_STRING:
          case TKN_ATOM:
            address += yytext;
            // state = 6;
            break;
          case TKN_EOH:
            address += "@localhost";
            gotOne(address);
            return true;
        }
        break;
      case 2:
        switch(c) {
          case TKN_QUOTED_STRING:
          case TKN_ATOM:
            address += yytext;
            state = 3;
            break;
          case TKN_EOH:
          case EOF:
//            printf("end of header\n");
            return true;
          default:
            fprintf(stderr, "expected text after dot '.' or colon ':', got %i\n", c);
            exit(EXIT_FAILURE);
        }
        break;
      case 3:
        switch(c) {
          case '.':
            address += c;
            state = 2;
            break;
          case '@':
            address += c;
            state = 4;
            break;
          default:
            fprintf(stderr, "expected '@' or '.' after word\n");
            exit(EXIT_FAILURE);
        }
        break;
      case 4:
        switch(c) {
          case TKN_DOMAIN_LITERAL:
          case TKN_ATOM:
            address += yytext;
            state = 5;
            break;
          case ',':
            gotOne(address);
            address.clear();
            state = 0;
            break;
          default:
            fprintf(stderr, "expected atom/domain-literal after '@' or '.'\n");
            exit(EXIT_FAILURE);
        }
        break;
      case 5:
        switch(c) {
          case '.':
            address += c;
            state = 4;
            break;
          case ';':
            if (ingroup) {
              gotOne(address);
              address.clear();
              state = 0;
              ingroup = false;
              break;
            } else {
              fprintf(stderr, "unexpected end of group\n");
              exit(EXIT_FAILURE);
            }
            break;
          case '>':
            gotOne(address);
            address.clear();
            if (inroute) {
              inroute = false;
              if (ingroup)
                state = 6;
              else
                state = 0;
              break;
            }
            break;
          case ',':
            gotOne(address);
            address.clear();
            if (ingroup)
              state = 6;
            else
              state = 0;
            break;
            break;
          case TKN_EOH:
            if (ingroup || inroute) {
              fprintf(stderr, "unexpected end of header\n");
              exit(EXIT_FAILURE);
            }
            gotOne(address);
            return true;
          default:
            fprintf(stderr, "expected dot '.' after atom/domain-literal: %i\n", c);
            exit(EXIT_FAILURE);
        }
        break;
        
      case 6:
        switch(c) {
          case TKN_QUOTED_STRING:
          case TKN_ATOM:
            address += yytext;
            state = 1;
            break;
          case '<':
            address.clear();
            state = 7;
            break;
          case ':':
            if (ingroup) {
              fprintf(stderr, "group inside group\n");
              exit(EXIT_FAILURE);
            }
            address.clear();
            ingroup = true;
            break;
          case ';':
            if (!ingroup) {
              fprintf(stderr, "unexpected end of group\n");
              exit(EXIT_FAILURE);
            }
            ingroup = false;
            state = 0;
            break;
          case ',':
            if (!ingroup) {
              fprintf(stderr, "unexpected ,\n");
              exit(EXIT_FAILURE);
            }
            state = 0;
            break;
          default:
            printf("character    : '%c'\n", c);
            exit(1);
          break;
        }
        break;
        
      case 7:
        switch(c) {
          case '@':
            state = 8;
            break;
          case TKN_ATOM:
          case TKN_QUOTED_STRING:
            inroute = true;
            address = yytext;
            state = 3;
            break;
          default:
            fprintf(stderr, "malformed address\n");
            exit(EXIT_FAILURE);
        }
        break;
      case 8:
        if (c==':') {
          address.clear();
          inroute = true;
          state = 2;
        }
        break;
    }
  }
}

// return single character, atom, quoted-string, domain-literal and
// end-of-headerline, skip comments and compress whitespace
int
lex(FILE *in)
{
//printf("begin lex/n");
  int comment = 0;
  int cstate;
  int state = 0;
  yytext.clear();
  while(true) {
    int c = fgetc(in); // don't compress blanks inside strings
    if (orig) {
      *orig += c;
    }
//printf("lex : %i '%c' (%i)\n", state, c<32?'?':c, c);
    switch(state) {
      case 0:
        switch(c) {
          case ' ':
          case '\t':
            break;
          case '\r':
            state = 8;
            break;
          case '\n':
            state = 9;
            break;
          case '\"':
            yytext += c;
            state = 1;
            break;
          case ')':
          case '<':
          case '>':
          case '@':
          case ',':
          case ';':
          case ':':
          case '\\':
          case '.':
          case ']':
            return c;
            break;
          case '[':
            yytext += c;
            state = 4;
            break;
          case '(':
            ++comment;
            cstate = state;
            state = 6;
            break;
          default:
            if (c<=32 || c>127)
              return c;
            yytext += c;
            state = 3;
            break;
        }
        break;
      
      // quoted string
      case 1:
        switch(c) {
          case '\\':
            state = 2;
            break;
          case '\"':
            yytext += c;
            return TKN_QUOTED_STRING;
          default:
            yytext += c;
            break;
        }
        break;
      case 2: // "..\?
        yytext += c;
        state = 1;
        break;

      // atom
      case 3:
        switch(c) {
          case '(':
            ++comment;
            cstate = state;
            state = 6;
            break;
          case TKN_EOH:
          case EOF:
            return TKN_EOH;
          case '\"':
          case ')':
          case '<':
          case '>':
          case '@':
          case ',':
          case ';':
          case ':':
          case '\\':
          case '.':
          case '[':
          case ']':
            ungetc(c, in);
            if (orig && !orig->empty()) {
              orig->erase(orig->size()-1);
            }
            return TKN_ATOM;
          default:
            if (c<=32 || c>127) {
              ungetc(c, in);
              if (orig && !orig->empty()) {
                orig->erase(orig->size()-1);
              }
              return TKN_ATOM;
            }
            yytext += c;
            break;
        }
        break;
      
      case 4:
        switch(c) {
          case '[':
            fprintf(stderr, "unexpected '['\n");
            exit(EXIT_FAILURE);
          case ']':
            yytext += c;
            return TKN_DOMAIN_LITERAL;
            break;
          case '\\':
            state = 5;
            break;
          default:
            yytext += c;
        }
        break;
      case 5:
        yytext += c;
        state = 4;
        break;
      
      // comment
      case 6:
        switch(c) {
          case '(':
            ++comment;
            break;
          case ')':
            --comment;
            if (comment==0)
              state = cstate;
            break;
          case '\\':
            state = 7;
            break;
          case EOF:
            fprintf(stderr, "unexpected end of header\n");
            exit(EXIT_FAILURE);
            break;
        }
        break;
      case 7:
        switch(c) {
          case EOF:
            fprintf(stderr, "unexpected end of header\n");
            exit(EXIT_FAILURE);
            break;
          default:
            state = 6;
        }
        break;
      
      // end of line
      case 8:
        switch(c) {
          case '\n':
            state = 9;
            break;
          default:
            ungetc(c, in);
            if (orig && !orig->empty()) {
              orig->erase(orig->size()-1);
            }
            return '\r';
        }
        break;
      case 9:
        switch(c) {
          case ' ':
          case '\t':
            state = 0;
            break;
          default:
            ungetc(c, in);
            if (orig && !orig->empty()) {
              orig->erase(orig->size()-1);
            }
            return TKN_EOH;
        }
    }
  }
}

#if 0
/*
<word>
/               or
(x y)           priority
*rule           0 to n times element
<l>*<m>element  l to m times element
<l>#<m>element  l to m times elements, separated by comma ','    
[optional]      0 to 1 times
;               comment

To: address



address := mailbox | group
  group := phrase ":" [#mailbox] ";"
    phrase := 1*word
      word := atom | quoted-string
        atom :=  1*<any CHAR except specials, SPACE and CTLs>
        quoted-string := <"> *(qtext|quoted-pair) <">
  mailbox := addr-spec | phrase route-addr
    route-addr := "<" [route] addr-spec ">"
      route :=  1#("@" domain) ":"
        domain := sub-domain *("." sub-domain)
          sub-domain  :=  domain-ref | domain-literal
            domain-ref :=  atom
            domain-literal :=  "[" *(dtext / quoted-pair) "]"
              dtext := <any CHAR excluding "[", "]", "\" & CR, & including linear-white-space>
              quoted-pair := "\" CHAR
      addr-spec := local-part "@" domain
        local-part := word *("." word)


address := mailbox | group
  group := phrase ":" [mailbox-list | CFWS] ";" [CFWS]

        
*/
// RFC 822: 6. ADDRESS SPECIFICATION; 6.1.  SYNTAX

#endif

#ifdef TEST

void
test(unsigned t, const char *in, const char *out)
{
  FILE *f = fopen("test.tmp", "w+");
  if (!f) {
    perror("fopen");
    exit(EXIT_FAILURE);
  }
  fwrite(in, strlen(in), 1, f);
  fclose(f);
  
  f = fopen("test.tmp", "r");
  address.clear();
  parse_address(f);
  fclose(f);
  
  if (address != out) {
    printf("test %u failed:\n"
           "expected '%s'\n"
           "but got  '%s'\n", t, out, address.c_str());
    exit(EXIT_FAILURE);
  }
  printf("test %u okay!\n", t);
}

int
main()
{
  test( 0, "root\r\n", "Troot@localhost");
  test( 1, "a@d\r\n", "Ta@d"); 
  test( 2, "a@\r\n d\r\n", "Ta@d"); 
  test( 3, "a.x@\r\n d\r\n", "Ta.x@d"); 
  test( 4, "a@\r\n d . x\r\n", "Ta@d.x"); 
  test( 5, "walter <a@\r\n d . x>\r\n", "Ta@d.x"); 
  test( 6, "walter ppk <a@\r\n d . x>\r\n", "Ta@d.x"); 
  test( 7, "\"walter ppk\" <a@\r\n d . x>\r\n", "Ta@d.x"); 
  test( 8, "Important folk:\r\n"
           "     Tom Softwood <Balsa@Tree.Root>,\r\n"
           "      \"Sam Irving\"@Other-Host;,\r\n"
           "\tStandard Distribution:\r\n"
           "     /main/davis/people/standard@Other-Host,\r\n"
           "     \"<Jones>standard.dist.3\"@Tops-20-Host>;\r\n",
           "TBalsa@Tree.Root"
           "T\"Sam Irving\"@Other-Host"
           "T/main/davis/people/standard@Other-Host"
           "T\"<Jones>standard.dist.3\"@Tops-20-Host");
  test( 9, "George Jones<Group@Host>\r\n", "TGroup@Host");
  test(10, "\"Al Neuman\"@Mad-Host,\r\n"
           "  Sam.Irving@Other-Host\r\n",
           "T\"Al Neuman\"@Mad-HostTSam.Irving@Other-Host");
  test(11, "\r\n", "");
  test(12, "   \r\n", "");
  test(13, "Jones@Host, Smith@Other-Host, Doe@Somewhere-Else\r\n",
           "TJones@HostTSmith@Other-HostTDoe@Somewhere-Else");
  test(14, "George Jones <Group@Host>\r\n", "TGroup@Host");
  test(15, "The Committee: Jones@Host.Net,\r\n"
           " Smith@Other.Org,\r\n"
           " Doe@Somewhere-Else;\r\n",
           "TJones@Host.NetTSmith@Other.OrgTDoe@Somewhere-Else");
  test(16, " Gourmets: Pompous Person <WhoZiWhatZit@Cordon-Bleu>,\r\n"
           "          Childs@WGBH.Boston, Galloping Gourmet@\r\n"
           "          ANT.Down-Under (Australian National Television),\r\n"
           "          Cheapie@Discount-Liquors;,\r\n"
           " Cruisers:  Port@Portugal, Jones@SEA;,\r\n"
           " Another@Somewhere.SomeOrg\r\n",
           "TWhoZiWhatZit@Cordon-Bleu"
           "TChilds@WGBH.Boston"
           "TGallopingGourmet@ANT.Down-Under"
           "TCheapie@Discount-Liquors"
           "TPort@Portugal"
           "TJones@SEA"
           "TAnother@Somewhere.SomeOrg");
  test(17, " Wilt . (the  Stilt) Chamberlain@NBA.US\r\n",
           "TWilt.Chamberlain@NBA.US");
  test(18, " Wilt . (the(da)  Stilt) Chamberlain@NBA.US\r\n",
           "TWilt.Chamberlain@NBA.US");
  test(19, "getrud@arschkrampen.de, \"gürgen\" <oliver@kalkofe.de>\r\n",
           "Tgetrud@arschkrampen.deToliver@kalkofe.de");
}

#endif
