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

// Renombrada: Enviar mensaje a servidor
int transmit_message(const char *server_address, int port, const char *msg) {
    int connection_socket;
    struct sockaddr_in server_config;
    size_t sent_bytes;

    memset(&server_config, 0, sizeof(server_config));
    server_config.sin_family = AF_INET;
    server_config.sin_addr.s_addr = inet_addr(server_address);
    server_config.sin_port = htons(port);

    if ((connection_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Error creando socket");
        return -1;
    }

    if (connect(connection_socket, (struct sockaddr *)&server_config, sizeof(server_config)) < 0) {
        perror("Error conectando al servidor");
        close(connection_socket);
        return -1;
    }

    sent_bytes = write(connection_socket, msg, strlen(msg));
    if (sent_bytes <= 0) {
        perror("Error enviando mensaje");
        close(connection_socket);
        return -1;
    }

    printf("Bytes enviados: %ld\n", sent_bytes);

    if (close(connection_socket) < 0) {
        perror("Error cerrando conexión");
        return -1;
    }

    return 0;
}

// Renombrada: Obtener IP desde un nombre de dominio
int resolve_hostname_to_ip(const char *hostname, char *resolved_ip, size_t buffer_length) {
    struct hostent *host_entry;

    if ((host_entry = gethostbyname(hostname)) == NULL) {
        herror("Error resolviendo hostname");
        return -1;
    }

    strncpy(resolved_ip, inet_ntoa(*((struct in_addr *)host_entry->h_addr)), buffer_length - 1);
    resolved_ip[buffer_length - 1] = '\0';

    return 0;
}

// Renombrada: Extraer nombre de archivo de una ruta
const char *extract_filename(const char *filepath) {
    const char *filename = strrchr(filepath, '/');
    return (filename != NULL) ? filename + 1 : filepath;
}

// Renombrada: Análisis de la URL FTP
int decode_ftp_url(const char *ftp_url, char *username, char *passwd, char *server, char *filepath) {
    if (sscanf(ftp_url, "ftp://%99[^:]:%99[^@]@%99[^/]/%199[^\n]", username, passwd, server, filepath) == 4) {
        return 0;
    } else if (sscanf(ftp_url, "ftp://%99[^@]@%99[^/]/%199[^\n]", username, server, filepath) == 3) {
        strcpy(passwd, "rcom");
        return 0;
    } else if (sscanf(ftp_url, "ftp://%99[^/]/%199[^\n]", server, filepath) == 2) {
        strcpy(username, "rcom");
        strcpy(passwd, "rcom");
        return 0;
    }
    return -1;
}

// Renombrada: Ejecutar comando FTP
int execute_ftp_command(int connection_fd, const char *ftp_command, char *server_response, size_t response_limit) {
    char command_buffer[BUFFER_SIZE];

    snprintf(command_buffer, BUFFER_SIZE, "%s\r\n", ftp_command);
    if (write(connection_fd, command_buffer, strlen(command_buffer)) < 0) {
        perror("Error enviando comando FTP");
        return -1;
    }

    memset(server_response, 0, response_limit);
    while (1) {
        ssize_t read_bytes = read(connection_fd, server_response, response_limit - 1);
        if (read_bytes <= 0) {
            perror("Error leyendo respuesta FTP");
            return -1;
        }

        server_response[read_bytes] = '\0';
        printf("Respuesta del servidor: %s", server_response);

        if (server_response[0] == '1' || server_response[0] == '2' || server_response[0] == '3') {
            if (strstr(ftp_command, "PASV") == NULL) {
                break;
            }
            if (strstr(ftp_command, "PASV") != NULL && strstr(server_response, "(") != NULL) {
                break;
            }
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <url-ftp>\n", argv[0]);
        return 1;
    }

    const char *ftp_url = argv[1];
    char user[100] = "", password[100] = "", host[100], path[200], resolved_ip[100];

    if (decode_ftp_url(ftp_url, user, password, host, path) < 0) {
        fprintf(stderr, "Error: URL FTP no válida\n");
        return 1;
    }

    printf("Usuario: %s, Contraseña: %s, Servidor: %s, Ruta: %s\n", user, password, host, path);

    if (resolve_hostname_to_ip(host, resolved_ip, sizeof(resolved_ip)) < 0) {
        fprintf(stderr, "Error resolviendo hostname: %s\n", host);
        return 1;
    }

    printf("IP resuelta: %s\n", resolved_ip);

    return 0;
}
