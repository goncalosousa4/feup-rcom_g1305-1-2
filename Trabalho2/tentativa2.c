#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define SERVER_PORT 6000
#define BUFFER_SIZE 1024

int send_message(const char *server_ip, int server_port, const char *message) {
    int sockfd;
    struct sockaddr_in server_addr;
    size_t bytes;

    /* Server address handling */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(server_port);

    /* Open a TCP socket */
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }

    /* Connect to the server */
    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return -1;
    }

    /* Send a string to the server */
    bytes = write(sockfd, message, strlen(message));
    if (bytes <= 0) {
        perror("write()");
        close(sockfd);
        return -1;
    }

    printf("Bytes escritos: %ld\n", bytes);

    if (close(sockfd) < 0) {
        perror("close()");
        return -1;
    }

    return 0;
}

int get_ip_from_hostname(const char *hostname, char *ip_buffer, size_t buffer_size) {
    struct hostent *h;

    if ((h = gethostbyname(hostname)) == NULL) {
        herror("gethostbyname()");
        return -1;
    }

    strncpy(ip_buffer, inet_ntoa(*((struct in_addr *) h->h_addr)), buffer_size - 1);
    ip_buffer[buffer_size - 1] = '\0'; // Ensure null-termination

    return 0;
}

const char *get_filename(const char *path) {
    const char *filename = strrchr(path, '/');
    return (filename != NULL) ? filename + 1 : path;
}

int parse_url(const char *ftp_url, char *username, char *passwd, char *server, char *filepath) {
    // Verifica que la URL comience con "ftp://"
    if (strncmp(ftp_url, "ftp://", 6) != 0) {
        return -1;
    }

    const char *cursor = ftp_url + 6; // Saltar "ftp://"
    const char *at = strchr(cursor, '@');
    const char *slash = strchr(cursor, '/');

    if (!slash) {
        return -1; // Formato inválido, debe contener una ruta
    }

    if (at && at < slash) {
        // Hay credenciales incluidas
        const char *colon = strchr(cursor, ':');
        if (colon && colon < at) {
            // Formato: ftp://username:password@server/filepath
            strncpy(username, cursor, colon - cursor);
            username[colon - cursor] = '\0';
            strncpy(passwd, colon + 1, at - colon - 1);
            passwd[at - colon - 1] = '\0';
        } else {
            // Formato: ftp://username@server/filepath (sin password)
            strncpy(username, cursor, at - cursor);
            username[at - cursor] = '\0';
            strcpy(passwd, "rcom");
        }
        cursor = at + 1; // Mover cursor después de '@'
    } else {
        // Sin credenciales: ftp://server/filepath
        strcpy(username, "rcom");
        strcpy(passwd, "rcom");
    }

    // Extraer servidor y ruta
    strncpy(server, cursor, slash - cursor);
    server[slash - cursor] = '\0';
    strcpy(filepath, slash + 1);

    return 0;
}

int ftp_command(int sockfd, const char *command, char *response, size_t response_size) {
    char buffer[BUFFER_SIZE];

    snprintf(buffer, BUFFER_SIZE, "%s\r\n", command);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("Error sending command");
        return -1;
    }

    memset(response, 0, response_size);
    ssize_t bytes_read = read(sockfd, response, response_size - 1);
    if (bytes_read <= 0) {
        perror("Error reading response");
        return -1;
    }

    response[bytes_read] = '\0';
    printf("Server Response: %s", response);

    return 0;
}

int setup_passive_mode(int sockfd, char *data_ip, int *data_port) {
    char response[BUFFER_SIZE];

    if (ftp_command(sockfd, "PASV", response, BUFFER_SIZE) < 0) {
        return -1;
    }

    char *start = strchr(response, '(');
    char *end = strchr(response, ')');
    if (!start || !end || start >= end) {
        fprintf(stderr, "Invalid PASV response format\n");
        fprintf(stderr, "Response received: %s\n", response);
        return -1;
    }

    int h1, h2, h3, h4, p1, p2;
    if (sscanf(start, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Error parsing PASV response\n");
        fprintf(stderr, "Response received: %s\n", response);
        return -1;
    }

    snprintf(data_ip, BUFFER_SIZE, "%d.%d.%d.%d", h1, h2, h3, h4);
    *data_port = p1 * 256 + p2;

    printf("Passive mode - IP: %s, Port: %d\n", data_ip, *data_port);
    return 0;
}

int download_file(int data_sockfd, const char *path) {
    const char *filename = get_filename(path);
    if (strlen(filename) > 255) {
        fprintf(stderr, "Filename too long\n");
        return -1;
    }

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Error opening file");
        return -1;
    }

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(data_sockfd, buffer, BUFFER_SIZE)) > 0) {
        fwrite(buffer, 1, bytes_read, file);
    }

    if (bytes_read < 0) {
        perror("Error reading from data socket");
        fclose(file);
        return -1;
    }

    fclose(file);
    printf("File downloaded successfully: %s\n", filename);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    const char *url = argv[1];
    char user[100] = "", password[100] = "", host[100], path[200], ip[100];
    int control_sockfd = -1, data_sockfd = -1, data_port;

    if (parse_url(url, user, password, host, path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return 1;
    }

    printf("User: %s, Password: %s, Host: %s, Path: %s\n", user, password, host, path);

    if (get_ip_from_hostname(host, ip, sizeof(ip)) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", host);
        return 1;
    }
    printf("Resolved IP: %s\n", ip);

    if ((control_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(21);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(control_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(control_sockfd);
        return 1;
    }

    char response[BUFFER_SIZE];
    read(control_sockfd, response, BUFFER_SIZE);
    printf("Connected to FTP server: %s\n", response);

    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    char data_ip[100];
    if (setup_passive_mode(control_sockfd, data_ip, &data_port) < 0) goto cleanup;

    if ((data_sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        goto cleanup;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, data_ip, &server_addr.sin_addr);

    if (connect(data_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        goto cleanup;
    }

    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    if (download_file(data_sockfd, path) < 0) goto cleanup;

cleanup:
    if (data_sockfd >= 0) close(data_sockfd);
    if (control_sockfd >= 0) close(control_sockfd);

    return 0;
}
