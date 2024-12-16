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

// Función: Establecer conexión y retornar el socket abierto
int connect_to_server(const char *ip, int port) {
    int sockfd;
    struct sockaddr_in server_addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket()");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &server_addr.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return -1;
    }

    return sockfd;
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
    char buffer[BUFFER_SIZE];

    snprintf(buffer, BUFFER_SIZE, "%s\r\n", command);
    if (write(sockfd, buffer, strlen(buffer)) < 0) {
        perror("Error enviando comando FTP");
        return -1;
    }

    ssize_t bytes_read = read(sockfd, response, response_size - 1);
    if (bytes_read <= 0) {
        perror("Error leyendo respuesta FTP");
        return -1;
    }

    response[bytes_read] = '\0';
    printf("Respuesta: %s", response);
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

    if (ftp_command(control_socket, "PASV", response, BUFFER_SIZE) < 0) {
        return -1;
    }

    int h1, h2, h3, h4, p1, p2;
    if (sscanf(strchr(response, '('), "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        fprintf(stderr, "Error parsing PASV response\n");
        return -1;
    }

    snprintf(data_ip, BUFFER_SIZE, "%d.%d.%d.%d", h1, h2, h3, h4);
    *data_port = p1 * 256 + p2;
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

// Função: send_message
// Objetivo: Enviar uma mensagem para um servidor especificado via socket TCP.
// Parâmetros:
//   - server_ip: Endereço IP do servidor para o qual a mensagem será enviada.
//   - server_port: Porta do servidor para a conexão.
//   - message: Mensagem a ser enviada.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens de erro apropriadas.
int send_message(const char *server_ip, int server_port, const char *message) {
    int sockfd;
    struct sockaddr_in server_addr;
    ssize_t bytes;

    // Crear un socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket()");
        return -1;
    }

    // Configurar la dirección del servidor
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);

    // Conectar al servidor
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect()");
        close(sockfd);
        return -1;
    }

    // Enviar el mensaje si está definido
    if (message && strlen(message) > 0) {
        bytes = write(sockfd, message, strlen(message));
        if (bytes <= 0) {
            perror("write()");
            close(sockfd);
            return -1;
        }
        printf("Mensaje enviado con éxito. Bytes escritos: %ld\n", bytes);
    }

    // Retornar el descriptor del socket abierto para su uso posterior
    return sockfd;
}

// Função: handle_ftp_auth
// Objetivo: Gerir o processo de autenticação no servidor FTP, enviando os comandos USER e PASS.
// Parâmetros:
//   - control_socket: Descritor do socket de controle conectado ao servidor FTP.
//   - user: Nome do utilizador para autenticação.
//   - password: Palavra-passe para autenticação.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens de erro apropriadas.
int handle_ftp_auth(int control_socket, const char *user, const char *password) {
    char response[BUFFER_SIZE] = {0};// Buffer para armazenar a resposta do servidor
    char command[BUFFER_SIZE] = {0}; // Buffer para armazenar os comandos USER e PASS

    // Enviar comando USER para o servidor FTP
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send USER command\n");
        return -1; // Indicar erro
    }

    // Enviar comando PASS para o servidor FTP
    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send PASS command\n");
        return -1;
    }

    printf("Authentication successful\n");
    return 0; // Sucesso
}

// Função: handle_file_transfer
// Objetivo: Gerir o processo de transferência de ficheiros no servidor FTP, enviando o comando RETR e iniciando o download.
// Parâmetros:
//   - control_socket: Descritor do socket de controle conectado ao servidor FTP.
//   - data_socket: Descritor do socket de dados utilizado para a transferência.
//   - path: Caminho do ficheiro no servidor FTP a ser transferido.
// Retorna:
//   - 0 em caso de sucesso.
//   - -1 em caso de erro, exibindo mensagens apropriadas.
int handle_file_transfer(int control_socket, int data_socket, const char *path) {
    char response[BUFFER_SIZE] = {0};// Buffer para armazenar a resposta do servidor
    char command[BUFFER_SIZE] = {0}; // Buffer para armazenar o comando RETR

    // Enviar comando RETR para iniciar a transferência do ficheiro
    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Failed to send RETR command\n");
        return -1; // Indicar erro
    }

    // Iniciar o download do ficheiro usando o socket de dados
    if (download_file(data_socket, path) < 0) {
        fprintf(stderr, "Failed to download file\n");
        return -1;  // Indicar erro
    }

    return 0; // Sucesso
}

// Função: main
// Objetivo: Gerir a conexão FTP, realizar a autenticação, configurar o modo passivo e transferir um ficheiro do servidor.
// Parâmetros:
//   - argc: Contagem de argumentos recebidos.
//   - argv: Vetor de argumentos recebidos da linha de comandos.
// Retorna:
//   - 0 em caso de sucesso.
//   - 1 em caso de erro, exibindo mensagens apropriadas.
int main(int argc, char *argv[]) {
    int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <ftp-url>\n", argv[0]);
        return 1;
    }

    // Variables para la URL y conexión
    char user[100] = "", password[100] = "", host[100], path[200], ip[100];
    int control_socket = -1, data_socket = -1, data_port;

    // 1. Parsear la URL
    if (analizarUrl(argv[1], user, password, host, path) < 0) {
        fprintf(stderr, "URL FTP inválida\n");
        return 1;
    }

    printf("User: %s, Password: %s, Host: %s, Path: %s\n", user, password, host, path);

    // 2. Resolver el hostname a IP
    if (get_ip_from_hostname(host, ip, sizeof(ip)) < 0) {
        fprintf(stderr, "No se pudo resolver el hostname\n");
        return 1;
    }
    printf("Resolved IP: %s\n", ip);

    // 3. Establecer conexión al socket de control
    control_socket = send_message(ip, 21, ""); // Se conecta pero no envía datos
    if (control_socket < 0) {
        fprintf(stderr, "Error al conectar al servidor FTP\n");
        return 1;
    }

    // Leer el mensaje de bienvenida del servidor FTP
    char response[BUFFER_SIZE];
    if (read(control_socket, response, BUFFER_SIZE) <= 0) {
        perror("Error leyendo la respuesta inicial del servidor FTP");
        close(control_socket);
        return 1;
    }
    printf("Servidor FTP: %s\n", response);

    // 4. Enviar comandos USER y PASS para autenticación
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "USER %s", user);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Error enviando comando USER\n");
        close(control_socket);
        return 1;
    }

    snprintf(command, BUFFER_SIZE, "PASS %s", password);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Error enviando comando PASS\n");
        close(control_socket);
        return 1;
    }

    // 5. Configurar modo pasivo y obtener IP/puerto del socket de datos
    char data_ip[100];
    if (setup_passive_mode(control_socket, data_ip, &data_port) < 0) {
        fprintf(stderr, "Error configurando el modo pasivo\n");
        close(control_socket);
        return 1;
    }
    printf("Modo pasivo configurado - IP: %s, Puerto: %d\n", data_ip, data_port);

    // 6. Establecer conexión al socket de datos
    data_socket = send_message(data_ip, data_port, ""); // Se conecta pero no envía datos
    if (data_socket < 0) {
        fprintf(stderr, "Error al conectar al socket de datos\n");
        close(control_socket);
        return 1;
    }

    // 7. Enviar comando RETR para descargar el archivo
    snprintf(command, BUFFER_SIZE, "RETR %s", path);
    if (ftp_command(control_socket, command, response, BUFFER_SIZE) < 0) {
        fprintf(stderr, "Error enviando comando RETR\n");
        close(data_socket);
        close(control_socket);
        return 1;
    }

    // 8. Descargar el archivo
    if (download_file(data_socket, path) < 0) {
        fprintf(stderr, "Error descargando el archivo\n");
        close(data_socket);
        close(control_socket);
        return 1;
    }

    // 9. Cerrar sockets y finalizar
    close(data_socket);
    close(control_socket);
    printf("Descarga completada y conexiones cerradas con éxito\n");
    return 0;

}
