// Application layer protocol implementation

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include "application_layer.h"
#include "link_layer.h"

////////////////////////////////////////////////
// APPLICATION LAYER - Gerencia a camada de aplicação
////////////////////////////////////////////////
// Parâmetros:
//   serialPort: porta serial para a comunicação
//   mode: papel na conexão ("tx" para transmissor, "rx" para receptor)
//   baudRate: taxa de transmissão da conexão
//   maxRetries: número máximo de tentativas de retransmissão
//   waitTime: tempo de espera para retransmissão em segundos
//   filename: nome do ficheiro a ser transmitido
// Retorna:
//   Void. Gerencia toda a transmissão e recepção de dados.
void applicationLayer(const char *serialPort, const char *mode, int baudRate, int maxRetries, int waitTime, const char *filename) {
    LinkLayer config;
    strcpy(config.serialPort, serialPort);
    config.role = strcmp(mode, "tx") == 0 ? LlTx : LlRx;
    config.baudRate = baudRate;
    config.nRetransmissions = maxRetries;
    config.timeout = waitTime;

    // Abre a conexão usando as configurações definidas
    if (llopen(config) < 0) {
        perror("Erro ao abrir a conexão\n");
        llclose(0);
        exit(-1);
    }

    // Inicia a contagem do tempo de transmissão para estatísticas
    clock_t start = clock();

    if (config.role == LlTx) {
        if (iniciarTransmissao(filename) < 0) {
            perror("Erro durante a transmissão\n");
            exit(-1);
        }
    } else if (config.role == LlRx) {
        if (iniciarRecepcao(filename) < 0) {
            perror("Erro durante a recepção\n");
            exit(-1);
        }
    }

    // Calcular o tempo decorrido e imprimir estatísticas
    clock_t end = clock();
    float elapsed = (float)(end - start) / CLOCKS_PER_SEC;
    printf("Tempo de transmissão: %.2f segundos\n", elapsed);

    llclose(1);
}

// Função para iniciar a transmissão de um ficheiro
static int iniciarTransmissao(const char *filename) {
    // Abrir o ficheiro em modo de leitura binária
    FILE *file = abrirArquivo(filename, "rb");
    if (file == NULL) return -1;

    long fileSize = calcularTamanhoArquivo(file);
    enviarPacoteControle(0x02, filename, fileSize);

    unsigned char sequenceNumber = 0;
    unsigned char buffer[256];
    int bytesRead;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        unsigned char *dataPacket = criarPacoteDados(sequenceNumber, buffer, bytesRead);
        if (verificarEnvioPacote(dataPacket, bytesRead + 4) < 0) {
            free(dataPacket);
            return -1;
        }
        sequenceNumber = sequenceHandler(sequenceNumber);
        free(dataPacket);
    }

    enviarPacoteControle(0x03, filename, fileSize);
    fclose(file);
    return 0;
}

// Função para iniciar a recepção de um ficheiro
static int iniciarRecepcao(const char *filename) {
    FILE *file = abrirArquivo(filename, "wb");
    if (file == NULL) return -1;

    unsigned char buffer[512];
    int packetSize;
    int recebendo = 1;

    while (recebendo && (packetSize = llread(buffer)) > 0) {
        switch (buffer[0]) {
            case 0x01: 
                fwrite(buffer + 4, sizeof(unsigned char), packetSize - 4, file);
                break;
            case 0x03:  
                recebendo = 0;
                break;
            default:
                break;
        }
    }

    fclose(file);
    return 0;
}

// Função para abrir um ficheiro
static FILE* abrirArquivo(const char *filename, const char *mode) {
    FILE *file = fopen(filename, mode);
    if (file == NULL) {
        perror("Erro ao abrir o ficheiro\n");
    }
    return file;
}

// Função para calcular o tamanho do ficheiro
static long calcularTamanhoArquivo(FILE *file) {
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    return size;
}

// Função para enviar um pacote de controlo de início ou fim
static int enviarPacoteControle(unsigned char controlFlag, const char *filename, long fileSize) {
    unsigned char *packet = buildControlPacket(controlFlag, filename, fileSize);
    int result = verificarEnvioPacote(packet, strlen((char *)packet) + 1);
    free(packet);
    return result;
}

// Função para criar um pacote de controle (inicio/fim)
static unsigned char *buildControlPacket(unsigned char controlType, const char *filename, long fileSize) {
    unsigned char *packet = malloc(512);
    int index = 0;

    packet[index++] = controlType;
    packet[index++] = 0x00;
    packet[index++] = sizeof(long);

    for (int i = sizeof(long) - 1; i >= 0; i--) {
        packet[index++] = (fileSize >> (i * 8)) & 0xFF;
    }

    packet[index++] = 0x01;
    packet[index++] = strlen(filename);

    for (int i = 0; i < strlen(filename); i++) {
        packet[index++] = filename[i];
    }
    return packet;
}

// Função para criar um pacote de dados
static unsigned char *criarPacoteDados(unsigned char sequenceNumber, const unsigned char *data, int dataSize) {
    unsigned char *packet = malloc(dataSize + 4);

    packet[0] = 0x01;
    packet[1] = sequenceNumber;
    packet[2] = (dataSize >> 8) & 0xFF;
    packet[3] = dataSize & 0xFF;

    memcpy(packet + 4, data, dataSize);
    return packet;
}

// Função auxiliar para verificar a transmissão de pacotes
static int verificarEnvioPacote(unsigned char *packet, int packetSize) {
    if (llwrite(packet, packetSize) < 0) {
        perror("Erro ao enviar o pacote\n");
        return -1;
    }
    return 0;
}

// Função para verificar e atualizar o número de sequência
static unsigned char sequenceHandler(unsigned char sequenceNumber) {
    return (sequenceNumber + 1) % 256;
}