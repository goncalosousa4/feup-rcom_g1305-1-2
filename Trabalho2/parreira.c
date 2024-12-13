#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "clientTCP.c"
#include "getip.c"

#define BUFFER_SIZE 1024

// Function to extract the file name from a path
const char *get_filename(const char *path) {
    const char *filename = strrchr(path, '/'); // Find the last '/'
    return (filename != NULL) ? filename + 1 : path; // Return the file name or the entire path if no '/'
}

// Function to parse the FTP URL
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

// Function to send an FTP command and receive a response
int ftp_command(int sockfd, const char *command, char *response, size_t response_size) {
    char buffer[BUFFER_SIZE];

    // Envia o comando para o servidor
    snprintf(buffer, BUFFER_SIZE, "%s\r\n", command);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("Error sending command");
        return -1;
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


// Function to enable passive mode
int setup_passive_mode(int sockfd, char *data_ip, int *data_port) {
    char response[BUFFER_SIZE];
    
    // Envia o comando PASV e captura a resposta
    if (ftp_command(sockfd, "PASV", response, BUFFER_SIZE) < 0) {
        return -1;
    }

    // Localiza o trecho com parênteses na resposta
    char *start = strchr(response, '(');
    char *end = strchr(response, ')');
    if (!start || !end || start >= end) {
        fprintf(stderr, "Invalid PASV response format\n");
        fprintf(stderr, "Response received: %s\n", response); // Para diagnóstico
        return -1;
    }

    // Extrai os números do formato "(h1,h2,h3,h4,p1,p2)"
    int h1, h2, h3, h4, p1, p2;
    if (sscanf(start, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Error parsing PASV response\n");
        fprintf(stderr, "Response received: %s\n", response); // Para diagnóstico
        return -1;
    }

    // Constrói o endereço IP e calcula a porta
    snprintf(data_ip, BUFFER_SIZE, "%d.%d.%d.%d", h1, h2, h3, h4);
    *data_port = p1 * 256 + p2;

    printf("Passive mode - IP: %s, Port: %d\n", data_ip, *data_port);
    return 0;
}


// Function to download a file
int download_file(int data_sockfd, const char *path) {
    const char *filename = get_filename(path); // Extract file name
    if (strlen(filename) > 255) {
        fprintf(stderr, "Filename too long\n");
        return -1;
    }

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Error opening file");
        return -1;
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

// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    const char *url = argv[1];
    char user[100] = "", password[100] = "", host[100], path[200], ip[100];
    int control_sockfd = -1, data_sockfd = -1, data_port;

    // Parse the FTP URL
    if (parse_url(url, user, password, host, path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return 1;
    }

    printf("User: %s, Password: %s, Host: %s, Path: %s\n", user, password, host, path);

    // Resolve the hostname to an IP address
    if (get_ip(host, ip) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", host);
        return 1;
    }
    printf("Resolved IP: %s\n", ip);

    // Create a control connection
    if ((control_sockfd = create_connection(ip, 21)) < 0) {
        return 1;
    }

    char response[BUFFER_SIZE];
    read(control_sockfd, response, BUFFER_SIZE);
    printf("Connected to FTP server: %s\n", response);

    // Login to the FTP server
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    // Enter passive mode
    char data_ip[100];
    if (setup_passive_mode(control_sockfd, data_ip, &data_port) < 0) goto cleanup;

    printf("Passive mode - IP: %s, Port: %d\n", data_ip, data_port);

    // Create a data connection
    if ((data_sockfd = create_connection(data_ip, data_port)) < 0) goto cleanup;

    // Request file transfer
    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    // Download the file
    if (download_file(data_sockfd, path) < 0) goto cleanup;

cleanup:
    if (data_sockfd >= 0) close(data_sockfd);
    if (control_sockfd >= 0) close(control_sockfd);

    return 0;
}

    printf("File downloaded successfully: %s\n", filename);
    return 0;
}

// Main function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    const char *url = argv[1];
    char user[100] = "", password[100] = "", host[100], path[200], ip[100];
    int control_sockfd = -1, data_sockfd = -1, data_port;

    // Parse the FTP URL
    if (parse_url(url, user, password, host, path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return 1;
    }

    printf("User: %s, Password: %s, Host: %s, Path: %s\n", user, password, host, path);

    // Resolve the hostname to an IP address
    if (get_ip(host, ip) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", host);
        return 1;
    }
    printf("Resolved IP: %s\n", ip);

    // Create a control connection
    if ((control_sockfd = create_connection(ip, 21)) < 0) {
        return 1;
    }

    char response[BUFFER_SIZE];
    read(control_sockfd, response, BUFFER_SIZE);
    printf("Connected to FTP server: %s\n", response);

    // Login to the FTP server
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    // Enter passive mode
    char data_ip[100];
    if (setup_passive_mode(control_sockfd, data_ip, &data_port) < 0) goto cleanup;

    printf("Passive mode - IP: %s, Port: %d\n", data_ip, data_port);

    // Create a data connection
    if ((data_sockfd = create_connection(data_ip, data_port)) < 0) goto cleanup;

    // Request file transfer
    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_sockfd, command, response, BUFFER_SIZE) < 0) goto cleanup;

    // Download the file
    if (download_file(data_sockfd, path) < 0) goto cleanup;

cleanup:
    if (data_sockfd >= 0) close(data_sockfd);
    if (control_sockfd >= 0) close(control_sockfd);

    return 0;
}