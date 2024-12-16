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

// Função: extract_ip_port
// Objetivo: Extrair os componentes da IP e da porta a partir de uma string formatada no estilo (h1,h2,h3,h4,p1,p2).
// Parâmetros:
//   - start: Ponteiro para o início da string contendo os dados de IP e porta.
//   - h1, h2, h3, h4: Ponteiros para armazenar os quatro octetos da IP.
//   - p1, p2: Ponteiros para armazenar os dois componentes do número da porta.
// Retorna:
//   - O número de valores extraídos com sucesso (deve ser 6 para sucesso completo).
int extract_ip_port(const char *start, int *h1, int *h2, int *h3, int *h4, int *p1, int *p2) {
    return sscanf(start, "(%d,%d,%d,%d,%d,%d)", h1, h2, h3, h4, p1, p2);
}

// Função: construct_ip_and_port
// Objetivo: Construir uma string de endereço IP e calcular o número da porta com base nos valores fornecidos.
// Parâmetros:
//   - data_ip: Buffer para armazenar o endereço IP formatado.
//   - buffer_size: Tamanho máximo do buffer de IP.
//   - h1, h2, h3, h4: Os quatro octetos do endereço IP.
//   - p1, p2: Os dois componentes do número da porta.
//   - data_port: Ponteiro para armazenar o número calculado da porta.
void construct_ip_and_port(char *data_ip, size_t buffer_size, int h1, int h2, int h3, int h4, int p1, int p2, int *data_port) {
    // Construir dirección IP
    snprintf(data_ip, buffer_size, "%d.%d.%d.%d", h1, h2, h3, h4);
    // Calcular el puerto
    *data_port = (p1 << 8) + p2;
}

// Função: setup_passive_mode
// Objetivo: Configurar o modo passivo em uma conexão FTP, extraindo o endereço IP e o número da porta
//           a partir da resposta do servidor ao comando PASV.
// Parâmetros:
//   - sockfd: Descritor do socket conectado ao servidor FTP.
//   - data_ip: Buffer para armazenar o endereço IP do modo passivo.
//   - data_port: Ponteiro para armazenar o número da porta do modo passivo.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens de depuração apropriadas.
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

// Função: open_file
// Objetivo: Abrir um ficheiro para escrita binária, verificando o tamanho do nome do ficheiro e tratando erros.
// Parâmetros:
//   - path: Caminho completo do ficheiro a ser aberto.
// Retorna:
//   - Ponteiro para o ficheiro aberto em caso de sucesso.
//   - NULL em caso de erro, exibindo mensagens de erro apropriadas.
FILE *open_file(const char *path) {
    // Obter o nome do ficheiro a partir do caminho completo
    const char *filename = get_filename(path);
    // Verificar se o nome do ficheiro excede o tamanho máximo permitido
    if (strlen(filename) > 255) {
        fprintf(stderr, "Filename too long: %s\n", filename);
        return NULL;
    }
    // Tentar abrir o ficheiro para escrita binária
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Error opening file");
    }
    // Retornar o ponteiro para o ficheiro aberto ou NULL
    return file;
}

// Função: transfer_data
// Objetivo: Transferir dados de um socket para um ficheiro, garantindo a escrita completa e tratando erros.
// Parâmetros:
//   - data_sockfd: Descritor do socket de onde os dados serão lidos.
//   - file: Ponteiro para o ficheiro onde os dados serão escritos.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens de erro apropriadas.
int transfer_data(int data_sockfd, FILE *file) {
    char buffer[BUFFER_SIZE];  // Buffer para armazenar os dados lidos do socket
    ssize_t bytes_read;  // Quantidade de bytes lidos do socket
    // Ler dados do socket e escrever no ficheiro
    while ((bytes_read = read(data_sockfd, buffer, BUFFER_SIZE)) > 0) {
        if (fwrite(buffer, 1, bytes_read, file) != bytes_read) {
            perror("Error writing to file");
            return -1; // Indicar erro na escrita
        }
    }
    // Verificar se ocorreu um erro na leitura do socket
    if (bytes_read < 0) {
        perror("Error reading from data socket");
        return -1; // Indicar erro na leitura
    }

    return 0; // Sucesso
}

// Função: download_file
// Objetivo: Gerir o processo de download de um ficheiro a partir de um socket de dados para o sistema de ficheiros.
// Parâmetros:
//   - data_sockfd: Descritor do socket de onde os dados serão lidos.
//   - path: Caminho completo para o ficheiro a ser criado no sistema local.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens de erro apropriadas.
int download_file(int data_sockfd, const char *path) {
    FILE *file = open_file(path);
    if (!file) {
        return -1; // Indicar erro
    }

    printf("Starting file download: %s\n", get_filename(path));

    // Transferir os dados do socket para o ficheiro
    if (transfer_data(data_sockfd, file) < 0) {
        fclose(file);
        return -1; // Indicar erro
    }

    fclose(file);  // Fechar o ficheiro após o download
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
