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

typedef enum {
    INIT,
    READING,
    VALIDATING,
    DONE
} ProcessState;


// FTP Utility Functions
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
    const char *filename = path;
    for (const char *current = path; *current != '\0'; current++) {
        if (*current == '/') {
            filename = current + 1;  // Actualizamos el puntero después del último '/'
        }
    }
    return filename;
}

int parse_url(const char *url, char *user, char *password, char *host, char *path) {
    const char *prefix = "ftp://";
    if (strncmp(url, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    const char *current = url + strlen(prefix);
    const char *at = strstr(current, "@");
    const char *slash = strstr(current, "/");

    if (at && slash && at < slash) {
        // Extraer usuario y contraseña
        char credentials[100];
        strncpy(credentials, current, at - current);
        credentials[at - current] = '\0';

        char *colon = strstr(credentials, ":");
        if (colon) {
            strncpy(user, credentials, colon - credentials);
            user[colon - credentials] = '\0';
            strcpy(password, colon + 1);
        } else {
            strcpy(user, credentials);
            strcpy(password, "rcom");
        }

        // Extraer host
        current = at + 1;
        strncpy(host, current, slash - current);
        host[slash - current] = '\0';

        // Extraer path
        strcpy(path, slash + 1);
    } else if (slash) {
        // Extraer host
        strncpy(host, current, slash - current);
        host[slash - current] = '\0';
        strcpy(user, "rcom");
        strcpy(password, "rcom");

        // Extraer path
        strcpy(path, slash + 1);
    } else {
        // Caso sin path
        strcpy(host, current);
        strcpy(user, "rcom");
        strcpy(password, "rcom");
        path[0] = '\0';
    }

    return 0;
}


int validate_response_code(char resp_code) {
    return (resp_code >= '1' && resp_code <= '3');
}

int ftp_command(int sockfd, const char *command, char *response, size_t response_size) {
    char cmd_buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    char resp_code;
    ProcessState state = INIT;

    // Formatear el comando con terminador CRLF
    snprintf(cmd_buffer, BUFFER_SIZE, "%s\r\n", command);

    // Enviar el comando al servidor
    if (write(sockfd, cmd_buffer, strlen(cmd_buffer)) < 0) {
        perror("Error enviando el comando al servidor");
        return -1;
    }
    printf("[DEBUG] Comando enviado: %s\n", command);

    // Inicializar el buffer de respuesta
    memset(response, 0, response_size);

    // Ciclo para manejar el proceso
    while (state != DONE) {
        switch (state) {
            case INIT:
                printf("[DEBUG] Iniciando lectura...\n");
                state = READING;
                break;

            case READING:
                bytes_read = read(sockfd, response, response_size - 1);
                if (bytes_read <= 0) {
                    perror("Error leyendo la respuesta del servidor");
                    return -1;
                }
                response[bytes_read] = '\0';  // Finalizar la cadena
                printf("[DEBUG] Respuesta parcial recibida (%ld bytes): %s", bytes_read, response);
                state = VALIDATING;
                break;

            case VALIDATING:
                resp_code = response[0];
                if (validate_response_code(resp_code)) {
                    printf("[DEBUG] Código de respuesta válido detectado: %c\n", resp_code);

                    if (!strstr(command, "MODO_PASIVO")) {
                        printf("[DEBUG] Comando estándar, finalizando lectura.\n");
                        state = DONE;
                    } else if (strstr(command, "MODO_PASIVO") && strstr(response, "(") != NULL) {
                        printf("[DEBUG] Respuesta válida para modo pasivo. Finalizando.\n");
                        state = DONE;
                    } else {
                        printf("[DEBUG] Respuesta incompleta para modo pasivo. Continuando lectura.\n");
                        state = READING;
                    }
                } else {
                    printf("[DEBUG] Código de respuesta no válido: %c. Continuando lectura.\n", resp_code);
                    state = READING;
                }
                break;

            case DONE:
                break;

            default:
                fprintf(stderr, "Estado desconocido. Abortando.\n");
                return -1;
        }
    }

    printf("[DEBUG] Proceso completado exitosamente.\n");
    return 0;
}

int extract_ip_port(const char *start, int *h1, int *h2, int *h3, int *h4, int *p1, int *p2) {
    return sscanf(start, "(%d,%d,%d,%d,%d,%d)", h1, h2, h3, h4, p1, p2);
}

void construct_ip_and_port(char *data_ip, size_t buffer_size, int h1, int h2, int h3, int h4, int p1, int p2, int *data_port) {
    // Construir dirección IP
    snprintf(data_ip, buffer_size, "%d.%d.%d.%d", h1, h2, h3, h4);
    // Calcular el puerto
    *data_port = (p1 << 8) + p2;
}

int setup_passive_mode(int sockfd, char *data_ip, int *data_port) {
    char response[BUFFER_SIZE];
    char *start = NULL;
    char *end = NULL;
    int h1 = 0, h2 = 0, h3 = 0, h4 = 0, p1 = 0, p2 = 0;

    // Enviar comando PASV al servidor FTP
    if (ftp_command(sockfd, "PASV", response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Error ejecutando el comando PASV\n");
        return -1;
    }

    printf("Respuesta del comando PASV: %s\n", response);

    // Buscar los paréntesis manualmente
    for (char *ptr = response; *ptr != '\0'; ptr++) {
        if (*ptr == '(' && start == NULL) {
            start = ptr; // Marcar el inicio del paréntesis
        } else if (*ptr == ')' && start != NULL) {
            end = ptr; // Marcar el cierre del paréntesis
            break;
        }
    }

    // Validar si se encontraron ambos paréntesis y tienen el formato correcto
    if (start == NULL || end == NULL || start >= end) {
        fprintf(stderr, "Formato de respuesta PASV no válido\n");
        return -1;
    }

    // Llamar a la función auxiliar para extraer IP y puerto
    if (extract_ip_port(start, &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Error al analizar la respuesta PASV\n");
        fprintf(stderr, "Respuesta recibida: %s\n", response);
        return -1;
    }

    // Usar función para construir IP y calcular puerto
    construct_ip_and_port(data_ip, BUFFER_SIZE, h1, h2, h3, h4, p1, p2, data_port);

    printf("Modo pasivo - IP: %s, Puerto: %d\n", data_ip, *data_port);
    return 0;
}


// File Handling
// File Handling
FILE *open_file(const char *path) {
    const char *filename = get_filename(path);
    if (strlen(filename) > 255) {
        fprintf(stderr, "Filename too long: %s\n", filename);
        return NULL;
    }

    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Error opening file");
    }

    return file;
}

int transfer_data(int data_sockfd, FILE *file) {
    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    while ((bytes_read = read(data_sockfd, buffer, BUFFER_SIZE)) > 0) {
        if (fwrite(buffer, 1, bytes_read, file) != bytes_read) {
            perror("Error writing to file");
            return -1;
        }
    }

    if (bytes_read < 0) {
        perror("Error reading from data socket");
        return -1;
    }

    return 0;
}

int download_file(int data_sockfd, const char *path) {
    FILE *file = open_file(path);
    if (!file) {
        return -1;
    }

    printf("Starting file download: %s\n", get_filename(path));
    if (transfer_data(data_sockfd, file) < 0) {
        fclose(file);
        return -1;
    }

    fclose(file);
    printf("File downloaded successfully: %s\n", get_filename(path));
    return 0;
}

// Message Sending
int send_message(const char *server_ip, int server_port, const char *message) {
    int sockfd;
    struct sockaddr_in server_addr;
    size_t bytes;

    bzero((char *) &server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(server_port);

    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return -1;
    }

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

int handle_ftp_auth(int control_socket, const char *user, const char *password) {
    char response[BUFFER_SIZE] = {0};
    char command[BUFFER_SIZE] = {0};

    // Comando USER
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send USER command\n");
        return -1;
    }

    // Comando PASS
    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send PASS command\n");
        return -1;
    }

    printf("Authentication successful\n");
    return 0;
}

int handle_file_transfer(int control_socket, int data_socket, const char *path) {
    char response[BUFFER_SIZE] = {0};
    char command[BUFFER_SIZE] = {0};

    // Enviar comando RETR
    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send RETR command\n");
        return -1;
    }

    // Descargar archivo
    if (download_file(data_socket, path) < 0) {
        fprintf(stderr, "Failed to download file\n");
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[]) {
    const char *url = NULL;
    char user[100] = "", password[100] = "", host[100] = {0}, path[200] = {0}, ip_address[100] = {0};
    int control_socket = -1;
    int data_socket = -1;
    int data_port = 0;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    url = argv[1];

    // Parsear URL y extraer detalles
    if (parse_url(url, user, password, host, path) < 0) {
        fprintf(stderr, "Invalid FTP URL\n");
        return 1;
    }
    printf("User: %s, Password: %s, Host: %s, Path: %s\n", user, password, host, path);

    // Resolver hostname a IP
    if (get_ip_from_hostname(host, ip_address, sizeof(ip_address)) < 0) {
        fprintf(stderr, "Failed to resolve hostname: %s\n", host);
        return 1;
    }
    printf("Resolved IP: %s\n", ip_address);

    // Conectar al servidor FTP
    control_socket = connect_to_server(ip_address, 21);
    if (control_socket < 0) {
        if (control_socket >= 0) close(control_socket);
        return 1;
    }

    if (handle_ftp_auth(control_socket, user, password) < 0) {
        if (control_socket >= 0) close(control_socket);
        return 1;
    }

    char passive_ip[100] = {0};
    if (setup_passive_mode(control_socket, passive_ip, &data_port) < 0) {
        if (control_socket >= 0) close(control_socket);
        return 1;
    }

    // Conectar al socket de datos
    data_socket = connect_to_server(passive_ip, data_port);
    if (data_socket < 0) {
        if (control_socket >= 0) close(control_socket);
        if (data_socket >= 0) close(data_socket);
        return 1;
    }

    if (handle_file_transfer(control_socket, data_socket, path) < 0) {
        if (control_socket >= 0) close(control_socket);
        if (data_socket >= 0) close(data_socket);
        return 1;
    }

    if (data_socket >= 0) {
        close(data_socket);
        printf("Data socket closed successfully\n");
    }
    if (control_socket >= 0) {
        close(control_socket);
        printf("Control socket closed successfully\n");
    }

    return 0;
}

