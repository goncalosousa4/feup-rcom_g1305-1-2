#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

define BUFFER_SIZE 1024

typedef struct {
    char user[100];
    char password[100];
    char host[100];
    char path[200];
    char ip[100];
    int controlSocket;
    int dataSocket;
} FTPClient;

// Extract filename from path
const char *extractFilename(const char *path) {
    const char *filename = strrchr(path, '/');
    return (filename != NULL) ? filename + 1 : path;
}

// Parse FTP URL
int processFTPURL(const char *url, char *user, char *password, char *host, char *path) {
    if (sscanf(url, "ftp://%99[^:]:%99[^@]@%99[^/]/%199[^
]", user, password, host, path) == 4) {
        return 0;
    } else if (sscanf(url, "ftp://%99[^@]@%99[^/]/%199[^
]", user, host, path) == 3) {
        strcpy(password, "anonymous");
        return 0;
    } else if (sscanf(url, "ftp://%99[^/]/%199[^
]", host, path) == 2) {
        strcpy(user, "anonymous");
        strcpy(password, "anonymous");
        return 0;
    }
    return -1;
}

// Send and receive FTP command
int sendAndReceiveFTPCommand(int sockfd, const char *command, char *response, size_t response_size) {
    char buffer[BUFFER_SIZE];

    snprintf(buffer, BUFFER_SIZE, "%s\r\n", command);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("Error sending command");
        return -1;
    }

    memset(response, 0, response_size);
    while (1) {
        ssize_t bytes_read = read(sockfd, response, response_size - 1);
        if (bytes_read <= 0) {
            perror("Error reading response");
            return -1;
        }

        response[bytes_read] = '\0';
        if (response[0] == '1' || response[0] == '2' || response[0] == '3') {
            if (strstr(command, "PASV") == NULL || (strstr(command, "PASV") != NULL && strchr(response, '('))) {
                break;
            }
        }
    }

    return 0;
}

// Enable passive mode
int enablePassiveFTPMode(int sockfd, char *data_ip, int *data_port) {
    char response[BUFFER_SIZE];

    if (sendAndReceiveFTPCommand(sockfd, "PASV", response, BUFFER_SIZE) < 0) {
        return -1;
    }

    char *start = strchr(response, '(');
    char *end = strchr(response, ')');
    if (!start || !end || start >= end) {
        fprintf(stderr, "Invalid PASV response: %s\n", response);
        return -1;
    }

    char *token = strtok(start + 1, ",");
    int values[6], i = 0;
    while (token && i < 6) {
        values[i++] = atoi(token);
        token = strtok(NULL, ",");
    }

    if (i == 6) {
        snprintf(data_ip, BUFFER_SIZE, "%d.%d.%d.%d", values[0], values[1], values[2], values[3]);
        *data_port = values[4] * 256 + values[5];
        return 0;
    }

    fprintf(stderr, "Error parsing PASV response\n");
    return -1;
}

// Download a file
int initializeFileDownload(int data_sockfd, const char *path) {
    const char *filename = extractFilename(path);
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
        perror("Error reading from socket");
    }

    fclose(file);
    return (bytes_read < 0) ? -1 : 0;
}

// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    FTPClient client = {0};

    if (processFTPURL(argv[1], client.user, client.password, client.host, client.path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return 1;
    }

    struct hostent *h;
    if ((h = gethostbyname(client.host)) == NULL) {
        herror("gethostbyname");
        return 1;
    }
    strncpy(client.ip, inet_ntoa(*((struct in_addr *)h->h_addr)), sizeof(client.ip) - 1);

    printf("User: %s, Host: %s, Path: %s\n", client.user, client.host, client.path);

    client.controlSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.controlSocket < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(21);
    inet_pton(AF_INET, client.ip, &server_addr.sin_addr);

    if (connect(client.controlSocket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(client.controlSocket);
        return 1;
    }

    char response[BUFFER_SIZE];
    read(client.controlSocket, response, BUFFER_SIZE);
    printf("Connected to server: %s\n", response);

    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "USER %s", client.user);
    if (sendAndReceiveFTPCommand(client.controlSocket, command, response, BUFFER_SIZE) < 0) {
        close(client.controlSocket);
        return 1;
    }

    snprintf(command, BUFFER_SIZE, "PASS %s", client.password);
    if (sendAndReceiveFTPCommand(client.controlSocket, command, response, BUFFER_SIZE) < 0) {
        close(client.controlSocket);
        return 1;
    }

    char data_ip[BUFFER_SIZE];
    int data_port;
    if (enablePassiveFTPMode(client.controlSocket, data_ip, &data_port) < 0) {
        close(client.controlSocket);
        return 1;
    }

    printf("Passive mode - IP: %s, Port: %d\n", data_ip, data_port);

    client.dataSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (client.dataSocket < 0) {
        perror("socket");
        close(client.controlSocket);
        return 1;
    }

    struct sockaddr_in data_addr = {0};
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(data_port);
    inet_pton(AF_INET, data_ip, &data_addr.sin_addr);

    if (connect(client.dataSocket, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        perror("connect");
        close(client.controlSocket);
        close(client.dataSocket);
        return 1;
    }

    snprintf(command, BUFFER_SIZE, "RETR %s", client.path);
    if (sendAndReceiveFTPCommand(client.controlSocket, command, response, BUFFER_SIZE) < 0) {
        close(client.controlSocket);
        close(client.dataSocket);
        return 1;
    }

    if (initializeFileDownload(client.dataSocket, client.path) < 0) {
        close(client.controlSocket);
        close(client.dataSocket);
        return 1;
    }

    close(client.dataSocket);
    close(client.controlSocket);
    return 0;
}
