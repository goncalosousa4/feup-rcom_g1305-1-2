#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#define BUFFER_SIZE 1024

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
        strcpy(senha, "password");
        sscanf(inicio, "%[^/]/%s", host, recurso);
    }

    if (!barra || strlen(host) == 0 || strlen(recurso) == 0) {
        fprintf(stderr, "Erro ao interpretar URL. Verifique o formato.\n");
        exit(EXIT_FAILURE);
    }
}

void resolverHost(const char *host, char *ip_resolvido) {
    struct hostent *servidor = gethostbyname(host);
    if (!servidor) {
        fprintf(stderr, "Erro: Não foi possível resolver o host %s\n", host);
        exit(EXIT_FAILURE);
    }
    strcpy(ip_resolvido, inet_ntoa(*((struct in_addr *)servidor->h_addr)));
    printf("Host resolvido: %s -> %s\n", host, ip_resolvido);
}

int conectarServidor(const char *ip, int porta) {
    struct sockaddr_in endereco;

    int socketFD = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFD < 0) {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    memset(&endereco, 0, sizeof(endereco));
    endereco.sin_family = AF_INET;
    endereco.sin_addr.s_addr = inet_addr(ip);
    endereco.sin_port = htons(porta);

    if (connect(socketFD, (struct sockaddr *)&endereco, sizeof(endereco)) < 0) {
        perror("Erro ao conectar");
        exit(EXIT_FAILURE);
    }

    return socketFD;
}

void enviarComando(int socketFD, const char *comando, char *resposta) {
    write(socketFD, comando, strlen(comando));
    write(socketFD, "\r\n", 2);
    memset(resposta, 0, BUFFER_SIZE);
    read(socketFD, resposta, BUFFER_SIZE);
    printf("Comando enviado: %s\nResposta: %s", comando, resposta);
}

int authConn(int socket, const char *user, const char *pass) {
    char comando[BUFFER_SIZE];
    char resposta[BUFFER_SIZE];

    sprintf(comando, "USER %s", user);
    enviarComando(socket, comando, resposta);

    if (strncmp(resposta, "331", 3) == 0) { // Contraseña requerida
        sprintf(comando, "PASS %s", pass);
        enviarComando(socket, comando, resposta);

        if (strncmp(resposta, "230", 3) != 0) {
            fprintf(stderr, "Erro na autenticação. Resposta: %s\n", resposta);
            return -1;
        }
    } else if (strncmp(resposta, "230", 3) != 0) {
        fprintf(stderr, "Erro inesperado na autenticação. Resposta: %s\n", resposta);
        return -1;
    }

    printf("Autenticação bem-sucedida como '%s'\n", user);
    return 0;
}

void modoPassivo(int socketCtrl, char *ip, int *porta) {
    char resposta[BUFFER_SIZE];
    enviarComando(socketCtrl, "PASV", resposta);

    int ip1, ip2, ip3, ip4, p1, p2;
    if (sscanf(resposta, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d)", &ip1, &ip2, &ip3, &ip4, &p1, &p2) != 6) {
        fprintf(stderr, "Erro ao interpretar a resposta do modo passivo. Resposta: %s\n", resposta);
        exit(EXIT_FAILURE);
    }

    sprintf(ip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);
    *porta = p1 * 256 + p2;

    printf("Modo passivo: IP %s, Porta %d\n", ip, *porta);
}

void transferirFicheiro(int socketCtrl, const char *recurso) {
    char resposta[BUFFER_SIZE], ip[BUFFER_SIZE];
    int porta;

    modoPassivo(socketCtrl, ip, &porta);

    int socketDados = conectarServidor(ip, porta);
    char comando[BUFFER_SIZE];
    sprintf(comando, "RETR %s", recurso);
    enviarComando(socketCtrl, comando, resposta);

    if (strncmp(resposta, "150", 3) != 0) {
        fprintf(stderr, "Erro ao iniciar transferência do ficheiro. Resposta: %s\n", resposta);
        close(socketDados);
        return;
    }

    FILE *ficheiro = fopen("pipe.txt", "wb");
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

    enviarComando(socketCtrl, "QUIT", resposta);
    if (strncmp(resposta, "226", 3) != 0) {
        fprintf(stderr, "Erro ao finalizar transferência. Resposta: %s\n", resposta);
    } else {
        printf("Transferência concluída com sucesso.\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s ftp://[<utilizador>:<senha>@]<host>/<recurso>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char utilizador[BUFFER_SIZE], senha[BUFFER_SIZE], host[BUFFER_SIZE], recurso[BUFFER_SIZE], ip[BUFFER_SIZE];
    analisarURL(argv[1], utilizador, senha, host, recurso);
    resolverHost(host, ip);

    printf("Conectando ao servidor FTP: %s (%s)\n", host, ip);
    int socketCtrl = conectarServidor(ip, 21);

    char resposta[BUFFER_SIZE];
    read(socketCtrl, resposta, BUFFER_SIZE);
    printf("Bem-vindo: %s\n", resposta);

    if (authConn(socketCtrl, utilizador, senha) != 0) {
        close(socketCtrl);
        return EXIT_FAILURE;
    }

    transferirFicheiro(socketCtrl, recurso);
    close(socketCtrl);

    return 0;
}
