#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <ctype.h>

#define FTP_PORT 21

struct URL {
    char user[128];
    char password[128];
    char host[128];
    char path[256];
};

int parse_url(char* input, struct URL* url) {
    if (strncmp(input, "ftp://", 6) != 0) {
        printf("Error: URL must start with ftp://\n");
        return -1;
    }
    char *user_pass_start = input + 6;
    char *slash_pos = strchr(user_pass_start, '/');
    char *at_pos = strchr(user_pass_start, '@');

    if (slash_pos) {
        strcpy(url->path, slash_pos);
        *slash_pos = '\0'; 
    } else {
        strcpy(url->path, "");
    }

    if (at_pos && at_pos < slash_pos) { 
        *at_pos = '\0';
        char *colon_pos = strchr(user_pass_start, ':');
        strcpy(url->host, at_pos + 1);
        if (colon_pos) {
            *colon_pos = '\0';
            strcpy(url->user, user_pass_start);
            strcpy(url->password, colon_pos + 1);
        } else {
            strcpy(url->user, user_pass_start);
            strcpy(url->password, ""); 
        }
    } else { 
        strcpy(url->host, user_pass_start);
        strcpy(url->user, "anonymous");
        strcpy(url->password, "anonymous");
    }
    
    if (slash_pos) *slash_pos = '/'; 
    return 0;
}

int connect_socket(char *ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }
    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        return -1;
    }
    return sockfd;
}

char* get_ip(char *hostname) {
    struct hostent *h;
    if ((h = gethostbyname(hostname)) == NULL) {
        herror("gethostbyname");
        return NULL;
    }
    return inet_ntoa(*((struct in_addr *) h->h_addr_list[0]));
}

int read_response(int sockfd, char *buffer_out, int max_size) {
    char c;
    int ptr = 0;
    
    // If user provided NULL, use a local dummy buffer
    char local_buffer[4096]; 
    char *buf = buffer_out ? buffer_out : local_buffer;
    int limit = buffer_out ? max_size : 4096;

    memset(buf, 0, limit);

    while (read(sockfd, &c, 1) > 0) {
        if (ptr < limit - 1) {
            buf[ptr++] = c;
        }

        if (c == '\n') {
            // Calculate start of the current line
            int line_start = ptr - 1; 
            while (line_start > 0 && buf[line_start-1] != '\n') {
                line_start--;
            }

            // FTP Protocol End Condition:
            // The line must start with 3 digits and a SPACE (e.g., "220 ")
            // Continuation lines (e.g., "220-") or text lines do NOT stop the loop.
            if ((ptr - line_start) >= 4) {
                 if (isdigit(buf[line_start]) && 
                     isdigit(buf[line_start+1]) && 
                     isdigit(buf[line_start+2]) && 
                     buf[line_start+3] == ' ') {
                     break;
                 }
            }
        }
    }
    
    buf[ptr] = '\0';
    printf("%s", buf); // Print server response to console
    return ptr;
}

int send_command(int sockfd, char *cmd, char *arg) {
    char buffer[1024];
    if (arg != NULL) sprintf(buffer, "%s %s\r\n", cmd, arg);
    else sprintf(buffer, "%s\r\n", cmd);
    
    printf("Log: Sending %s", buffer); 
    
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("write");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("Usage: ./download ftp://[user:pass@]host/path\n");
        return 1;
    }

    // A. PARSE
    struct URL url;
    if (parse_url(argv[1], &url) < 0) return 1;
    printf("Debug: User: %s, Pass: %s, Host: %s, Path: %s\n", url.user, url.password, url.host, url.path);

    // B. DNS
    char *ip_str = get_ip(url.host);
    if (!ip_str) return 1;
    printf("Debug: IP Address is %s\n", ip_str);

    // C. CONNECT
    printf("Connecting to Control Socket...\n");
    int control_socket = connect_socket(ip_str, FTP_PORT);
    if (control_socket < 0) return 1;

    read_response(control_socket, NULL, 0); // Greeting

    // D. LOGIN
    printf("\n--- LOGGING IN ---\n");
    send_command(control_socket, "USER", url.user);
    read_response(control_socket, NULL, 0); 

    send_command(control_socket, "PASS", url.password);
    read_response(control_socket, NULL, 0); 

    // E. PASSIVE MODE
    printf("\n--- ENTERING PASSIVE MODE ---\n");
    send_command(control_socket, "PASV", NULL);

    char buffer[4096]; // Increased buffer size just in case
    read_response(control_socket, buffer, 4096); 

    int ip1, ip2, ip3, ip4, port1, port2;
    char *start = strchr(buffer, '(');
    if (start && sscanf(start, "(%d,%d,%d,%d,%d,%d)", &ip1, &ip2, &ip3, &ip4, &port1, &port2) == 6) {
        
        int data_port = port1 * 256 + port2;
        char data_ip[20];
        sprintf(data_ip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
        printf("Debug: DATA IP: %s | PORT: %d\n", data_ip, data_port);

        // F. DOWNLOAD SEQUENCE
        printf("\n--- STARTING DOWNLOAD ---\n");
        
        // 1. Connect to Data Socket
        int data_socket = connect_socket(data_ip, data_port);
        if (data_socket < 0) return 1;

        // 2. Request File
        send_command(control_socket, "RETR", url.path);
        
        // Check for errors (e.g. 550 File Not Found)
        char retr_buf[1024];
        read_response(control_socket, retr_buf, 1024);
        if (strncmp(retr_buf, "550", 3) == 0) {
            printf("Error: File not found on server.\n");
            close(data_socket);
            close(control_socket);
            return 1;
        }

        // 3. Save File
        char *filename = strrchr(url.path, '/');
        filename = filename ? filename + 1 : "downloaded_file";
        FILE *f = fopen(filename, "wb");
        
        if (f) {
            char data_buf[1024];
            int bytes;
            while ((bytes = read(data_socket, data_buf, 1024)) > 0) {
                fwrite(data_buf, 1, bytes, f);
            }
            fclose(f);
            printf("\nSUCCESS: File saved as '%s'\n", filename);
        } else {
            perror("File Error");
        }
        
        close(data_socket);
        
        // 4. Final Confirmation
        read_response(control_socket, NULL, 0); 

    } else {
        printf("Error parsing PASV response.\n");
    }

    close(control_socket);
    return 0;
}
