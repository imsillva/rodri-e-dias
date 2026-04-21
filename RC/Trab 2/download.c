// download.c - Simple FTP download client with CWD support
//
// Usage:
// ./download ftp://[user:pass@]host/path/to/file
//
// Features:
// - URL parsing
// - Anonymous or authenticated login
// - Passive mode (PASV)
// - CWD traversal of each directory in path
// - RETR last component
// - Saves file in current directory
//


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUF_SIZE 4096
#define FTP_PORT 21

// -----------------------------------------
// Simple error handlers
// -----------------------------------------

// ---- Forward declarations (fix implicit declaration warnings) ----
void ftp_fail(const char *where, const char code[4], const char *reply);
void ftp_read_log(int sock, char code[4], char *reply, size_t rsz, const char *where);
void ftp_expect_code(const char *where, const char code[4], const char *reply,
                     const char *expected);


void error(const char *msg) {
perror(msg);
exit(-1);
}

void error_msg(const char *msg) {
fprintf(stderr, "%s\n", msg);
exit(-1);
}

// -----------------------------------------
// URL parsing: ftp://[user:pass@]host/path
// -----------------------------------------

int parse_url(const char *url,
char *user, size_t usz,
char *pass, size_t psz,
char *host, size_t hsz,
char *path, size_t psz2)
{
const char *p, *at, *slash, *colon;
size_t ulen, plen, hlen;

if (strncmp(url, "ftp://", 6) != 0)
return -1;

p = url + 6; // after ftp://
at = strchr(p, '@');
slash = strchr(p, '/');

if (slash == NULL)
return -1;

if (at != NULL && at < slash) {
// user:pass@
colon = strchr(p, ':');
if (colon == NULL || colon > at) return -1;

ulen = colon - p;
plen = at - colon - 1;

if (ulen >= usz || plen >= psz) return -1;

memcpy(user, p, ulen);
user[ulen] = '\0';

memcpy(pass, colon + 1, plen);
pass[plen] = '\0';

p = at + 1; // now p points to host
}
else {
// anonymous login
snprintf(user, usz, "anonymous");
snprintf(pass, psz, "anonymous@example.com");
}

hlen = slash - p;
if (hlen == 0 || hlen >= hsz) return -1;

memcpy(host, p, hlen);
host[hlen] = '\0';

if (strlen(slash + 1) >= psz2) return -1;
strcpy(path, slash + 1);

return 0;
}

// -----------------------------------------
// TCP helpers
// -----------------------------------------

int connect_tcp(const char *hostname, int port) {
int sockfd;
struct hostent *h;
struct sockaddr_in server_addr;

if ((h = gethostbyname(hostname)) == NULL) {
herror("gethostbyname()");
return -1;
}

printf("Host name : %s\n", h->h_name);
printf("IP Address : %s\n",
inet_ntoa(*((struct in_addr *) h->h_addr)));

bzero((char *) &server_addr, sizeof(server_addr));
server_addr.sin_family = AF_INET;
server_addr.sin_port = htons(port);
server_addr.sin_addr.s_addr =
((struct in_addr *) h->h_addr)->s_addr; // first IP

if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
perror("socket()");
return -1;
}

if (connect(sockfd, (struct sockaddr *) &server_addr,
sizeof(server_addr)) < 0) {
perror("connect()");
close(sockfd);
return -1;
}

return sockfd;
}

int connect_ip_port(const char *ip, int port) {
int sockfd;
struct sockaddr_in addr;

if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
perror("socket()");
return -1;
}

bzero((char *) &addr, sizeof(addr));
addr.sin_family = AF_INET;
addr.sin_port = htons(port);

if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
perror("inet_pton()");
close(sockfd);
return -1;
}

if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
perror("connect()");
close(sockfd);
return -1;
}

return sockfd;
}

// -----------------------------------------
// FTP reply reader
// -----------------------------------------

ssize_t read_line(int sock, char *buf, size_t max) {
size_t i = 0;
char c;
ssize_t n;

while (i < max - 1) {
n = recv(sock, &c, 1, 0);
if (n <= 0)
break;
buf[i++] = c;
if (c == '\n')
break;
}

buf[i] = '\0';
return i;
}

int ftp_read_reply(int sock, char code_out[4], char *last_line, size_t sz) {
char line[BUF_SIZE];
char first_code[4];
int multiline = 0;
ssize_t n;
int i;

first_code[0] = first_code[1] = first_code[2] = first_code[3] = 0;

for (;;) {
n = read_line(sock, line, sizeof(line));
if (n <= 0)
return -1;

// trim CRLF
for (i = (int)n - 1; i >= 0; i--) {
if (line[i] == '\r' || line[i] == '\n')
line[i] = 0;
else
break;
}

if (strlen(line) < 3)
continue;

if (!multiline) {
if (!(isdigit((unsigned char) line[0]) &&
isdigit((unsigned char) line[1]) &&
isdigit((unsigned char) line[2])))
continue;

strncpy(first_code, line, 3);
first_code[3] = 0;

if (line[3] == '-') {
multiline = 1;
}
else {
strncpy(code_out, first_code, 3);
code_out[3] = 0;
strncpy(last_line, line, sz);
return 0;
}
}
else {
if (strncmp(line, first_code, 3) == 0 && line[3] == ' ') {
strncpy(code_out, first_code, 3);
code_out[3] = 0;
strncpy(last_line, line, sz);
return 0;
}
}
}
}

int ftp_cmd(int sock, const char *fmt, ...) {
char buf[BUF_SIZE];
va_list ap;

va_start(ap, fmt);
vsnprintf(buf, sizeof(buf), fmt, ap);
va_end(ap);

strcat(buf, "\r\n");

if (send(sock, buf, strlen(buf), 0) < 0)
return -1;

return 0;
}

// ---------------------------------------------------
// Parse PASV reply
// ---------------------------------------------------

int parse_pasv(const char *line, char *ip, size_t ip_sz, int *port) {
const char *p, *q;
int h1,h2,h3,h4,p1,p2;

p = strchr(line, '(');
q = strchr(line, ')');
if (p == NULL || q == NULL)
return -1;

if (sscanf(p + 1, "%d,%d,%d,%d,%d,%d",
&h1,&h2,&h3,&h4,&p1,&p2) != 6)
return -1;

snprintf(ip, ip_sz, "%d.%d.%d.%d", h1,h2,h3,h4);
*port = p1 * 256 + p2;

return 0;
}

// ---------------------------------------------------
// CWD traversal for path
// ---------------------------------------------------
int ftp_cwd_path(int ctrl, const char *path) {
    char tmp[1024];
    char code[4];
    char reply[BUF_SIZE];
    char *tok, *next;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    tok = strtok(tmp, "/");
    while (tok != NULL) {
        next = strtok(NULL, "/");
        if (next == NULL)
            break; // last token is filename

        if (ftp_cmd(ctrl, "CWD %s", tok) < 0) {
            fprintf(stderr, "[FTP ERROR] CWD '%s': failed sending command\n", tok);
            return -1;
        }

        ftp_read_log(ctrl, code, reply, sizeof(reply), "CWD");
        if (strcmp(code, "250") != 0) {
            fprintf(stderr, "[FTP ERROR] CWD '%s' failed. code=%s reply='%s'\n",
                    tok, code, reply);   // <-- shows 550 etc
            return -1;
        }

        tok = next;
    }

    return 0;
}


// -----------------------------------------
// FTP error helper (prints FTP code + line)
// -----------------------------------------
void ftp_fail(const char *where, const char code[4], const char *reply) {
    fprintf(stderr, "[FTP ERROR] %s failed. Reply code=%s, reply='%s'\n",
            where,
            (code && code[0]) ? code : "???",
            reply ? reply : "(null)");
    exit(-1);
}

// Reads a reply, prints it, and optionally checks expected code
void ftp_read_log(int sock, char code[4], char *reply, size_t rsz, const char *where) {
    if (ftp_read_reply(sock, code, reply, rsz) < 0) {
        fprintf(stderr, "[FTP ERROR] %s: failed reading reply from server\n", where);
        exit(-1);
    }
    printf("<-- %s\n", reply);
}

void ftp_expect_code(const char *where, const char code[4], const char *reply,
                     const char *expected) {
    if (strcmp(code, expected) != 0) {
        ftp_fail(where, code, reply);   // includes 530/550/etc
    }
}

// ---------------------------------------------------
// Main
// ---------------------------------------------------
int main(int argc, char **argv) {
    int ctrl, data;
    char user[128], pass[128], host[256], path[512];
    char code[4] = {0}, reply[BUF_SIZE] = {0};
    char ip[64] = {0};
    int port = 0;
    char *filename;
    FILE *f;
    char buf[BUF_SIZE];
    ssize_t n;

    if (argc != 2) {
        fprintf(stderr,
                "Usage: %s ftp://[user:pass@]host/path/to/file\n", argv[0]);
        exit(-1);
    }

    if (parse_url(argv[1],
                  user, sizeof(user),
                  pass, sizeof(pass),
                  host, sizeof(host),
                  path, sizeof(path)) < 0) {
        error_msg("Invalid FTP URL");
    }

    printf("URL parsed:\n");
    printf(" user: %s\n", user);
    printf(" pass: %s\n", pass);
    printf(" host: %s\n", host);
    printf(" path: %s\n", path);

    // Connect control socket
    ctrl = connect_tcp(host, FTP_PORT);
    if (ctrl < 0)
        error_msg("Could not connect to FTP server");

    // Greeting
    ftp_read_log(ctrl, code, reply, sizeof(reply), "greeting");
    // Usually 220, but some servers may behave differently
    if (strcmp(code, "220") != 0) {
        ftp_fail("greeting", code, reply);
    }

    // USER
    if (ftp_cmd(ctrl, "USER %s", user) < 0)
        error_msg("Failed to send USER");
    ftp_read_log(ctrl, code, reply, sizeof(reply), "USER");

    // USER reply: 331 (need PASS) or 230 (already logged in) are OK
    if (strcmp(code, "331") != 0 && strcmp(code, "230") != 0) {
        // e.g. 530
        ftp_fail("USER", code, reply);
    }

    // PASS if needed
    if (strcmp(code, "331") == 0) {
        if (ftp_cmd(ctrl, "PASS %s", pass) < 0)
            error_msg("Failed to send PASS");
        ftp_read_log(ctrl, code, reply, sizeof(reply), "PASS");

        // PASS should succeed with 230
        if (strcmp(code, "230") != 0) {
            // e.g. 530 Login incorrect
            ftp_fail("PASS", code, reply);
        }
    }

    // Binary mode
    if (ftp_cmd(ctrl, "TYPE I") < 0)
        error_msg("Failed to send TYPE I");
    ftp_read_log(ctrl, code, reply, sizeof(reply), "TYPE I");
    ftp_expect_code("TYPE I", code, reply, "200");

    // Passive mode
    if (ftp_cmd(ctrl, "PASV") < 0)
        error_msg("Failed to send PASV");
    ftp_read_log(ctrl, code, reply, sizeof(reply), "PASV");
    ftp_expect_code("PASV", code, reply, "227");

    if (parse_pasv(reply, ip, sizeof(ip), &port) < 0) {
        fprintf(stderr, "[FTP ERROR] PASV parse failed. reply='%s'\n", reply);
        exit(-1);
    }
    printf("[INFO] PASV data endpoint: %s:%d\n", ip, port);

    // CWD through directories (logs 550 etc inside if you used the updated version)
    if (ftp_cwd_path(ctrl, path) < 0) {
        // If ftp_cwd_path() already printed code/reply, you can keep it simple:
        error_msg("CWD path failed");
    }

    // Last token is filename
    filename = strrchr(path, '/');
    if (filename != NULL) filename++;
    else filename = path;

    printf("Final file: %s\n", filename);

    // Open data connection
    data = connect_ip_port(ip, port);
    if (data < 0) {
        fprintf(stderr,
                "[NET ERROR] Could not open data socket to %s:%d (PASV).\n"
                "This often means the passive data port is blocked / bench not configured.\n",
                ip, port);
        close(ctrl);
        exit(-1);
    }

    // RETR
    if (ftp_cmd(ctrl, "RETR %s", filename) < 0)
        error_msg("Failed to send RETR");
    ftp_read_log(ctrl, code, reply, sizeof(reply), "RETR(initial)");

    // Some servers answer 150 or 125 to start transfer; failures like 550 appear here
    if (strcmp(code, "150") != 0 && strcmp(code, "125") != 0) {
        ftp_fail("RETR(initial)", code, reply); // <-- shows 550 etc
    }

    // Open local file
    f = fopen(filename, "wb");
    if (f == NULL) {
        close(data);
        close(ctrl);
        error("fopen");
    }

    // Read data and write to file
    while ((n = recv(data, buf, sizeof(buf), 0)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            close(data);
            close(ctrl);
            error_msg("fwrite failed");
        }
    }

    fclose(f);
    close(data);

    // Final reply after transfer (usually 226)
    ftp_read_log(ctrl, code, reply, sizeof(reply), "RETR(final)");
    if (strcmp(code, "226") != 0 && strcmp(code, "250") != 0) {
        ftp_fail("RETR(final)", code, reply);
    }

    // QUIT
    ftp_cmd(ctrl, "QUIT");
    ftp_read_log(ctrl, code, reply, sizeof(reply), "QUIT");

    close(ctrl);
    return 0;
}
