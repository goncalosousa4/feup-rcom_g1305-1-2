#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

// Função para analisar o URL e extrair informações essenciais
void analisarURL(const char *url, char *utilizador, char *senha, char *host, char *recurso) {
    if (strncmp(url, "ftp://", 6) != 0) {
        fprintf(stderr, "URL inválido. Deve começar com ftp://\n");
        exit(EXIT_FAILURE);
    }

    const char *inicio = url + 6;
    const char *at = strchr(inicio, '@');
    const char *barra = strchr(at ? at + 1 : inicio, '/');

    if (at) {
        sscanf(inicio, "%[^:]:%[^@]@%[^/]/%s", utilizador, senha, host, recurso);
    } else {
        strcpy(utilizador, "anonymous");
        strcpy(senha, "anonymous");
        sscanf(inicio, "%[^/]/%s", host, recurso);
    }
}

// Função para conectar-se ao servidor FTP
int conectarServidor(const char *host, int porta) {
    struct hostent *servidor;
    struct sockaddr_in endereco;

    int socketFD = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFD < 0) {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    servidor = gethostbyname(host);
    if (!servidor) {
        fprintf(stderr, "Host inválido\n");
        exit(EXIT_FAILURE);
    }

    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    memcpy(&endereco.sin_addr.s_addr, servidor->h_addr, servidor->h_length);
    endereco.sin_port = htons(porta);

    if (connect(socketFD, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        perror("Erro ao conectar");
        exit(EXIT_FAILURE);
    }

    return socketFD;
}

// Função para enviar comandos FTP e ler respostas
void enviarComando(int socketFD, const char *comando, char *resposta) {
    write(socketFD, comando, strlen(comando));
    write(socketFD, "\r\n", 2);
    memset(resposta, 0, BUFFER_SIZE);
    read(socketFD, resposta, BUFFER_SIZE);
    printf("Resposta: %s", resposta);
}

// Função para entrar no modo passivo e extrair informações de conexão
void modoPassivo(int socketCtrl, char *ip, int *porta) {
    char resposta[BUFFER_SIZE];
    enviarComando(socketCtrl, "PASV", resposta);

    int ip1, ip2, ip3, ip4, p1, p2;
    sscanf(resposta, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)", &ip1, &ip2, &ip3, &ip4, &p1, &p2);

    sprintf(ip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
    *porta = p1 * 256 + p2;
}

// Função para transferir ficheiro do servidor
void transferirFicheiro(int socketCtrl, const char *recurso) {
    char resposta[BUFFER_SIZE], ip[BUFFER_SIZE];
    int porta;

    modoPassivo(socketCtrl, ip, &porta);

    int socketDados = conectarServidor(ip, porta);
    char comando[BUFFER_SIZE];
    sprintf(comando, "RETR %s", recurso);
    enviarComando(socketCtrl, comando, resposta);

    FILE *ficheiro = fopen("ficheiro_descarregado", "wb");
    if (!ficheiro) {
        perror("Erro ao abrir ficheiro");
        close(socketDados);
        return;
    }

    char buffer[BUFFER_SIZE];
    int lidos;
    while ((lidos = read(socketDados, buffer, BUFFER_SIZE)) > 0) {
        fwrite(buffer, 1, lidos, ficheiro);
    }

    fclose(ficheiro);
    close(socketDados);
    printf("Transferência concluída.\n");
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s ftp://[<utilizador>:<senha>@]<host>/<recurso>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char utilizador[BUFFER_SIZE], senha[BUFFER_SIZE], host[BUFFER_SIZE], recurso[BUFFER_SIZE];
    analisarURL(argv[1], utilizador, senha, host, recurso);

    printf("Conectando ao servidor FTP: %s\n", host);
    int socketCtrl = conectarServidor(host, 21);

    char resposta[BUFFER_SIZE];
    read(socketCtrl, resposta, BUFFER_SIZE);
    printf("Bem-vindo: %s\n", resposta);

    char comando[BUFFER_SIZE];
    sprintf(comando, "USER %s", utilizador);
    enviarComando(socketCtrl, comando, resposta);

    sprintf(comando, "PASS %s", senha);
    enviarComando(socketCtrl, comando, resposta);

    transferirFicheiro(socketCtrl, recurso);

    enviarComando(socketCtrl, "QUIT", resposta);
    close(socketCtrl);

    return 0;
}
