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
    bzero((char *) &server_addr, sizeof(server_addr));
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
    const char *filename = strrchr(path, '/'); // Find the last '/'
    return (filename != NULL) ? filename + 1 : path; // Return the file name or the entire path if no '/'
}

int parse_url(const char *url, char *user, char *password, char *host, char *path) {
    if (sscanf(url, "ftp://%99[^:]:%99[^@]@%99[^/]/%199[^\n]", user, password, host, path) == 4) {
        return 0; // User, password, host, and path provided
    } else if (sscanf(url, "ftp://%99[^@]@%99[^/]/%199[^\n]", user, host, path) == 3) {
        strcpy(password, "anonymous"); // Default password
        return 0;
    } else if (sscanf(url, "ftp://%99[^/]/%199[^\n]", host, path) == 2) {
        strcpy(user, "anonymous");
        strcpy(password, "anonymous");
        return 0;
    }
    return -1; // Invalid URL
}

int ftp_command(int sockfd, const char *command, char *response, size_t response_size) {
    char buffer[BUFFER_SIZE];

    // Envia o comando para o servidor
    snprintf(buffer, BUFFER_SIZE, "%s\r\n", command);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("Error sending command");
        return -1;
    }

    // Lê a resposta do servidor até encontrar uma resposta relevante
    memset(response, 0, response_size);
    while (1) {
        ssize_t bytes_read = read(sockfd, response, response_size - 1);
        if (bytes_read <= 0) {
            perror("Error reading response");
            return -1;
        }

        response[bytes_read] = '\0'; // Garante a terminação da string

        // Imprime para depuração
        printf("Server Response: %s", response);

        // Verifica se a resposta é relevante:
        if (response[0] == '1' || response[0] == '2' || response[0] == '3') {
            // Resposta final para comandos comuns
            if (strstr(command, "PASV") == NULL) {
                break;
            }
            // Resposta válida para o comando PASV (verifica se contém parênteses)
            if (strstr(command, "PASV") != NULL && strstr(response, "(") != NULL) {
                break;
            }
        }

        // Mensagens intermediárias são ignoradas
        printf("Intermediate Response: %s", response);
    }

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
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <hostname> <message>\n", argv[0]);
        return -1;
    }

    const char *hostname = argv[1];
    const char *message = argv[2];
    char server_ip[BUFFER_SIZE];

    if (get_ip_from_hostname(hostname, server_ip, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", hostname);
        return -1;
    }

    printf("Resolved IP: %s\n", server_ip);

    if (send_message(server_ip, SERVER_PORT, message) < 0) {
        fprintf(stderr, "Failed to send message to server\n");
        return -1;
    }

    printf("Message sent successfully to %s (%s)\n", hostname, server_ip);
    return 0;
}
