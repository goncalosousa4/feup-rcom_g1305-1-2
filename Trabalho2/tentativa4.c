#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT_DEFAULT 21
#define MAX_BUF 1024

// Utility Functions
const char *extract_file_name(const char *file_path) {
    const char *last_slash = strrchr(file_path, '/');
    return last_slash ? last_slash + 1 : file_path;
}

int resolve_hostname(const char *hostname, char *resolved_ip, size_t ip_len) {
    struct hostent *host_entry;

    if ((host_entry = gethostbyname(hostname)) == NULL) {
        herror("Hostname resolution failed");
        return -1;
    }

    strncpy(resolved_ip, inet_ntoa(*((struct in_addr *) host_entry->h_addr)), ip_len - 1);
    resolved_ip[ip_len - 1] = '\0';
    return 0;
}

int parse_ftp_url(const char *ftp_url, char *username, char *passwd, char *hostname, char *file_path) {
    if (sscanf(ftp_url, "ftp://%99[^:]:%99[^@]@%99[^/]/%199[^\n]", username, passwd, hostname, file_path) == 4) {
        return 0;
    }
    if (sscanf(ftp_url, "ftp://%99[^@]@%99[^/]/%199[^\n]", username, hostname, file_path) == 3) {
        strcpy(passwd, "anonymous");
        return 0;
    }
    if (sscanf(ftp_url, "ftp://%99[^/]/%199[^\n]", hostname, file_path) == 2) {
        strcpy(username, "anonymous");
        strcpy(passwd, "anonymous");
        return 0;
    }
    return -1;
}

// FTP Command Execution
int send_ftp_command(int socket_fd, const char *cmd, char *response, size_t response_limit) {
    char cmd_buf[MAX_BUF];
    snprintf(cmd_buf, MAX_BUF, "%s\r\n", cmd);

    if (write(socket_fd, cmd_buf, strlen(cmd_buf)) < 0) {
        perror("Command transmission error");
        return -1;
    }

    memset(response, 0, response_limit);
    ssize_t bytes_read;
    while ((bytes_read = read(socket_fd, response, response_limit - 1)) > 0) {
        response[bytes_read] = '\0';
        if (response[0] == '2' || response[0] == '3') break;
    }

    return (bytes_read > 0) ? 0 : -1;
}

int configure_passive_mode(int socket_fd, char *data_ip, int *data_port) {
    char server_response[MAX_BUF];

    if (send_ftp_command(socket_fd, "PASV", server_response, MAX_BUF) < 0) {
        return -1;
    }

    char *begin = strchr(server_response, '(');
    char *end = strchr(server_response, ')');
    if (!begin || !end || begin >= end) {
        fprintf(stderr, "Malformed PASV response: %s\n", server_response);
        return -1;
    }

    int ip_parts[4], port_parts[2];
    if (sscanf(begin, "(%d,%d,%d,%d,%d,%d)", &ip_parts[0], &ip_parts[1], &ip_parts[2], &ip_parts[3], &port_parts[0], &port_parts[1]) != 6) {
        fprintf(stderr, "Error parsing PASV response: %s\n", server_response);
        return -1;
    }

    snprintf(data_ip, MAX_BUF, "%d.%d.%d.%d", ip_parts[0], ip_parts[1], ip_parts[2], ip_parts[3]);
    *data_port = (port_parts[0] << 8) | port_parts[1];
    return 0;
}

// File Transfer
int retrieve_file(int data_fd, const char *file_path) {
    const char *file_name = extract_file_name(file_path);

    FILE *file = fopen(file_name, "wb");
    if (!file) {
        perror("File open error");
        return -1;
    }

    char buffer[MAX_BUF];
    ssize_t read_bytes;
    while ((read_bytes = read(data_fd, buffer, MAX_BUF)) > 0) {
        fwrite(buffer, 1, read_bytes, file);
    }

    fclose(file);
    return (read_bytes < 0) ? -1 : 0;
}

// Main
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char user[100], pass[100], host[100], path[200], server_ip[100];
    int ctrl_socket = -1, data_socket = -1, data_port;

    if (parse_ftp_url(argv[1], user, pass, host, path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return EXIT_FAILURE;
    }

    if (resolve_hostname(host, server_ip, sizeof(server_ip)) < 0) {
        return EXIT_FAILURE;
    }

    ctrl_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (ctrl_socket < 0) {
        perror("Control socket creation failed");
        return EXIT_FAILURE;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT_DEFAULT);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    if (connect(ctrl_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to FTP server failed");
        close(ctrl_socket);
        return EXIT_FAILURE;
    }

    char server_resp[MAX_BUF];
    read(ctrl_socket, server_resp, MAX_BUF);

    char cmd_buf[MAX_BUF];
    snprintf(cmd_buf, MAX_BUF, "USER %s", user);
    if (send_ftp_command(ctrl_socket, cmd_buf, server_resp, MAX_BUF) < 0) goto cleanup;

    snprintf(cmd_buf, MAX_BUF, "PASS %s", pass);
    if (send_ftp_command(ctrl_socket, cmd_buf, server_resp, MAX_BUF) < 0) goto cleanup;

    char passive_ip[100];
    if (configure_passive_mode(ctrl_socket, passive_ip, &data_port) < 0) goto cleanup;

    data_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (data_socket < 0) {
        perror("Data socket creation failed");
        goto cleanup;
    }

    struct sockaddr_in data_addr = {0};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, passive_ip, &data_addr.sin_addr);

    if (connect(data_socket, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        perror("Data connection failed");
        goto cleanup;
    }

    snprintf(cmd_buf, MAX_BUF, "RETR %s", path);
    if (send_ftp_command(ctrl_socket, cmd_buf, server_resp, MAX_BUF) < 0) goto cleanup;

    if (retrieve_file(data_socket, path) < 0) goto cleanup;

cleanup:
    if (data_socket >= 0) close(data_socket);
    if (ctrl_socket >= 0) close(ctrl_socket);
    return EXIT_SUCCESS;
}
