#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 512

void errMsg(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

/* read one line (ending with '\n') from socket */
static int recv_line(int fd, char *buf, size_t bufsz) {
    size_t i = 0;

    while (i + 1 < bufsz) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) return 0;        /* connection closed */
        if (r < 0) return -1;        /* error */
        buf[i++] = c;
        if (c == '\n') break;
    }

    buf[i] = '\0';
    return 1;
}

/* read server response until END\n */
static void read_until_end(int sockfd) {
    char buf[BUFFER_SIZE];

    while (1) {
        int rc = recv_line(sockfd, buf, sizeof(buf));
        if (rc <= 0)
            errMsg("read");

        fputs(buf, stdout);

        if (strcmp(buf, "END\n") == 0)
            break;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        errMsg("socket");

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0)
        errMsg("inet_pton");

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1) {
        fprintf(stderr, "Error: Unable to connect to the server.\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    /* welcome block */
    read_until_end(sockfd);

    char line[BUFFER_SIZE];
    char srv[BUFFER_SIZE];

    while (1) {
        /* read READY> */
        int rc = recv_line(sockfd, srv, sizeof(srv));
        if (rc == 0) break;
        if (rc < 0) errMsg("read");
        fputs(srv, stdout);

        /* user input */
        if (!fgets(line, sizeof(line), stdin))
            break;

        trim_newline(line);

        /* send line + newline */
        char sendbuf[BUFFER_SIZE];
        int n = snprintf(sendbuf, sizeof(sendbuf), "%s\n", line);
        if (n < 0 || n >= (int)sizeof(sendbuf)) {
            fprintf(stderr, "Input too long.\n");
            continue;
        }

        if (write(sockfd, sendbuf, (size_t)n) != n)
            errMsg("write");

        /* read response until END */
        read_until_end(sockfd);

        if (strncmp(line, "QUIT", 4) == 0)
            break;
    }

    close(sockfd);
    return 0;
}

