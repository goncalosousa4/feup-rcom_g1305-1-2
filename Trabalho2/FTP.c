/**      (C)2000-2021 FEUP
 *       tidy up some includes and parameters
 * */

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>

#include <string.h>
#include <netdb.h>

// Porta FTP padrão
#define FTP_PORT 21
// Tamanho do buffer para as comunicações
#define BUFFER_SIZE 1024

/**
 * Envia um comando para o servidor FTP.
 * @param sockfd Descritor do socket para a conexão de controlo.
 * @param command Comando FTP (por exemplo, "USER", "PASS").
 * @param args Argumentos do comando (por exemplo, nome de utilizador ou palavra-passe).
 */
void sendCommand(int sockfd, const char *command, const char *args) {
    char buffer[BUFFER_SIZE];
    snprintf(buffer, sizeof(buffer), "%s %s\r\n", command, args);
    write(sockfd, buffer, strlen(buffer)); // Enviar comando para o servidor
    printf(">> %s", buffer); // Mostrar o comando enviado no ecrã
}

/**
 * Lê e exibe a resposta do servidor FTP.
 * @param sockfd Descritor do socket para a conexão de controlo.
 */
void readResponse(int sockfd) {
    char buffer[BUFFER_SIZE];
    int bytesRead = read(sockfd, buffer, sizeof(buffer) - 1); // Ler dados do servidor
    if (bytesRead <= 0) {
        perror("Erro ao ler resposta do servidor");
        exit(EXIT_FAILURE);
    }
    buffer[bytesRead] = '\0'; // Terminar a string com '\0'
    printf("<< %s", buffer); // Mostrar a resposta do servidor no ecrã
}

/**
 * Estabelece uma conexão TCP com o servidor.
 * @param host Endereço ou nome do host para conectar.
 * @return Descritor do socket para a conexão estabelecida.
 */
int connectToServer(const char *host) {
    // Resolver o nome do host para um endereço IP
    struct hostent *server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "Erro: não foi possível converter o nome do host '%s' para um endereço IP\n", host);
        exit(EXIT_FAILURE);
    }

    // Criar um socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Erro ao criar o socket");
        exit(EXIT_FAILURE);
    }

    // Configurar o endereço do servidor
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET; //AF_INET é uma constante que indica que o socket vai usar o protocolo IPv4(Internet Protocol version 4)
    serverAddr.sin_port = htons(FTP_PORT); // Porta em ordem de bytes de rede
    memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length); // Copiar o IP do host

    // Tentar conectar ao servidor
    if (connect(sockfd, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) < 0) {
        perror("Erro ao conectar ao servidor");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Conectado a %s:%d\n", host, FTP_PORT);
    return sockfd;
}

/**
 * Ativa o modo passivo no servidor FTP.
 * @param sockfd Descritor do socket de controlo.
 * @param ip Endereço IP obtido no modo passivo.
 * @param port Porta obtida no modo passivo.
 */
void enterPassiveMode(int sockfd, char *ip, int *port) {
    // Enviar o comando PASV para o servidor
    sendCommand(sockfd, "PASV", "");
    char buffer[BUFFER_SIZE];
    readResponse(sockfd);

    // Analisar a resposta para obter IP e porta
    int ip1, ip2, ip3, ip4, p1, p2;
    sscanf(buffer, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)", &ip1, &ip2, &ip3, &ip4, &p1, &p2);
    snprintf(ip, BUFFER_SIZE, "%d.%d.%d.%d", ip1, ip2, ip3, ip4); // Construir o endereço IP
    *port = p1 * 256 + p2; // Calcular a porta a partir dos valores de p1 e p2
    printf("Modo passivo: IP %s, Porta %d\n", ip, *port);
}

/**
 * Faz o download de um ficheiro do servidor FTP.
 * @param dataSockfd Descritor do socket de dados.
 * @param filename Nome do ficheiro a ser guardado localmente.
 */
void downloadFile(int dataSockfd, const char *filename) {
    FILE *file = fopen(filename, "wb"); // Criar um ficheiro para guardar os dados
    if (file == NULL) {
        perror("Erro ao criar o ficheiro local");
        exit(EXIT_FAILURE);
    }

    char buffer[BUFFER_SIZE];
    int bytesRead;
    // Ler dados do socket de dados e escrever no ficheiro
    while ((bytesRead = read(dataSockfd, buffer, sizeof(buffer))) > 0) {
        fwrite(buffer, 1, bytesRead, file);
    }

    printf("Ficheiro %s descarregado com sucesso.\n", filename);
    fclose(file); // Fechar o ficheiro local
}

/**
 * Função principal do cliente FTP.
 * @param argc Número de argumentos.
 * @param argv Argumentos (deve incluir o URL FTP).
 * @return Código de saída.
 */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <ftp://host/file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Extrair o host e o caminho do ficheiro do URL fornecido
    char host[BUFFER_SIZE], path[BUFFER_SIZE];
    sscanf(argv[1], "ftp://%[^/]/%s", host, path);

    // Conectar ao servidor de controlo
    int controlSockfd = connectToServer(host);

    // Ler a resposta inicial do servidor FTP
    readResponse(controlSockfd);

    // Enviar as credenciais de utilizador e palavra-passe
    sendCommand(controlSockfd, "USER", "rcom");
    readResponse(controlSockfd);
    sendCommand(controlSockfd, "PASS", "rcom");
    readResponse(controlSockfd);

    // Ativar o modo passivo
    char ip[BUFFER_SIZE];
    int port;
    enterPassiveMode(controlSockfd, ip, &port);

    // Conectar ao servidor de dados
    int dataSockfd = connectToServer(ip);
    sendCommand(controlSockfd, "RETR", path); // Solicitar o ficheiro ao servidor
    readResponse(controlSockfd);

    // Fazer o download do ficheiro
    downloadFile(dataSockfd, path);

    // Fechar as conexões
    close(dataSockfd);
    sendCommand(controlSockfd, "QUIT", "");
    readResponse(controlSockfd);
    close(controlSockfd);

    return EXIT_SUCCESS;
}