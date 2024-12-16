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
    INIT,            // Estado inicial
    READING,         // Estado de leitura
    VALIDATING,      // Estado de validação
    DONE             // Estado concluído
} ProcessState;


// Função: get_ip_from_hostname
// Objetivo: Resolver um hostname e obter o endereço IP correspondente.
// Parâmetros:
//   - hostname: Nome do host a ser resolvido.
//   - ip_buffer: Buffer para armazenar o endereço IP resolvido.
//   - buffer_size: Tamanho do buffer de saída.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 se a resolução falhar, exibindo uma mensagem de erro.
int get_ip_from_hostname(const char *hostname, char *ip_buffer, size_t buffer_size) {
    struct hostent *h;
    // Resolver o hostname para obter informações do host
    if ((h = gethostbyname(hostname)) == NULL) {
        //indicar o hostname problemático
        herror("gethostbyname()");
        return -1;  // Indicar falha na resolução
    }
    // Copiar o endereço IP obtido para o buffer de saída
    strncpy(ip_buffer, inet_ntoa(*((struct in_addr *) h->h_addr)), buffer_size - 1);
    ip_buffer[buffer_size - 1] = '\0'; // Ensure null-termination
    // Retornar sucesso
    return 0;
}

// Função: get_filename
// Objetivo: Extrair o nome do ficheiro a partir de um caminho fornecido.
// Parâmetros:
//   - path: Caminho completo para análise.
// Retorna:
//   - Ponteiro para o nome do ficheiro dentro da string original (ou NULL em caso de erro).
const char *get_filename(const char *path) {
    if (path == NULL || *path == '\0') {
        fprintf(stderr, "Erro: Caminho fornecido é inválido ou vazio.\n");
        return NULL; // Retornar NULL em caso de erro
    }
     // Percorrer o caminho para localizar a última barra '/'
    const char *filename = path;
    for (const char *current = path; *current != '\0'; current++) {
        if (*current == '/') {
            filename = current + 1;  // Atualizar o ponteiro após a última barra
        }
    }
    // Verificar se o nome do ficheiro está vazio após a última barra
    if (*filename == '\0') {
        fprintf(stderr, "Erro: Nenhum nome de ficheiro encontrado no caminho fornecido.\n");
        return NULL; // Retornar NULL se não houver nome de ficheiro
    }
    // Retornar o ponteiro para o nome do ficheiro
    return filename;
}

// Função: analizarUrl
// Objetivo: Analisar um URL FTP para extrair informações como utilizador, senha, host e caminho.
// Parâmetros:
//   - url: O URL completo para análise.
//   - user: Buffer para armazenar o nome do utilizador extraído.
//   - password: Buffer para armazenar a senha extraída.
//   - host: Buffer para armazenar o host extraído.
//   - path: Buffer para armazenar o caminho extraído.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 se o URL for inválido ou não puder ser analisado.
int analizarUrl(const char *url, char *user, char *password, char *host, char *path) {
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
// Função: validate_response_code
// Objetivo: Validar se o código de resposta fornecido está dentro do intervalo esperado ('1' a '3').
// Parâmetros:
//   - resp_code: Caractere representando o código de resposta do servidor.
// Retorna:
//   - 1 se o código for válido.
//   - 0 se o código for inválido, exibindo uma mensagem de erro.
int validate_response_code(char resp_code) {
    return (resp_code >= '1' && resp_code <= '3');
}

// Função: ftp_command
// Objetivo: Enviar um comando FTP ao servidor, processar a resposta recebida e validar o código de resposta.
// Parâmetros:
//   - sockfd: Descritor do socket conectado ao servidor FTP.
//   - command: Comando FTP a ser enviado.
//   - response: Buffer para armazenar a resposta do servidor.
//   - response_size: Tamanho máximo do buffer de resposta.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens apropriadas de depuração.
int ftp_command(int sockfd, const char *command, char *response, size_t response_size) {
    char cmd_buffer[BUFFER_SIZE];   // Buffer para armazenar o comando formatado
    ssize_t bytes_read;   // Número de bytes lidos do servidor
    char resp_code;   // Código de resposta do servidor
    ProcessState state = INIT;   // Estado inicial do processo
    int complete = 0;

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

    // Ciclo para manejar el proceso usando la máquina de estados
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
                response[bytes_read] = '\0';
                printf("[DEBUG] Respuesta parcial recibida (%ld bytes): %s", bytes_read, response);
                state = VALIDATING;
                break;

            case VALIDATING:
                resp_code = response[0];
                if (validate_response_code(resp_code)) {
                    printf("[DEBUG] Código de respuesta válido detectado: %c\n", resp_code);

                    if (!strstr(command, "PASV")) {
                        complete = 1;
                    } else if (strstr(command, "PASV") && strstr(response, "(") != NULL) {
                        complete = 1;
                    }

                    if (complete) {
                        printf("[DEBUG] Respuesta completa procesada, finalizando.\n");
                        state = DONE;
                    } else {
                        printf("[DEBUG] Respuesta incompleta para comando PASV, continuando lectura.\n");
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

// File Handling
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

// Main Function
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    const char *url = argv[1];
    char user[100] = "", password[100] = "", host[100], path[200], ip[100];
    int control_sockfd = -1, data_sockfd = -1, data_port;

    if (analizarUrl(url, user, password, host, path) < 0) {
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
