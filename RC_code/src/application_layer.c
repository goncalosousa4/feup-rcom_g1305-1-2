// Implementação do protocolo da camada de aplicação

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include "application_layer.h"
#include "link_layer.h"
#include "serial_port.h"

// Funções auxiliares
static int iniciarTransmissao(const char *filename);
static int iniciarRecepcao(const char *filename);
static FILE* abrirArquivo(const char *filename, const char *mode);
static long calcularTamanhoArquivo(FILE *file);
static int enviarPacoteControle(unsigned char controlFlag, const char *filename, long fileSize);
static unsigned char *criarPacoteControle(unsigned char controlType, const char *filename, long fileSize);
static unsigned char *criarPacoteDados(unsigned char sequenceNumber, const unsigned char *data, int dataSize);
static int verificarEnvioPacote(unsigned char *packet, int packetSize);
static unsigned char proximoNumeroSequencia(unsigned char sequenceNumber);

// Gerencia a camada de aplicação
void applicationLayer(const char *serialPort, const char *mode, int baudRate, int maxRetries, int waitTime, const char *filename) {
    LinkLayer config;
    strcpy(config.serialPort, serialPort);
    config.role = strcmp(mode, "tx") == 0 ? LlTx : LlRx;
    config.baudRate = baudRate;
    config.nRetransmissions = maxRetries;
    config.timeout = waitTime;

    if (llopen(config) < 0) {
        perror("Erro ao abrir a conexão\n");
        llclose(0);
        exit(-1);
    }

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

    clock_t end = clock();
    float elapsed = (float)(end - start) / CLOCKS_PER_SEC;
    printf("Tempo de transmissão: %.2f segundos\n", elapsed);

    llclose(1);
}

// Função para iniciar a transmissão de um ficheiro
static int iniciarTransmissao(const char *filename) {
    FILE *file = abrirArquivo(filename, "rb");
    if (file == NULL) return -1;

    long fileSize = calcularTamanhoArquivo(file);
    if (enviarPacoteControle(0x02, filename, fileSize) < 0) return -1;

    unsigned char sequenceNumber = 0;
    unsigned char buffer[256];
    int bytesRead;

    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        unsigned char *dataPacket = criarPacoteDados(sequenceNumber, buffer, bytesRead);
        if (verificarEnvioPacote(dataPacket, bytesRead + 4) < 0) {
            free(dataPacket);
            fclose(file);
            return -1;
        }
        sequenceNumber = proximoNumeroSequencia(sequenceNumber);
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
    unsigned char *packet = criarPacoteControle(controlFlag, filename, fileSize);
    int result = verificarEnvioPacote(packet, strlen((char *)packet) + 1);
    free(packet);
    return result;
}

// Função para criar um pacote de controle (inicio/fim)
static unsigned char *criarPacoteControle(unsigned char controlType, const char *filename, long fileSize) {
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
static unsigned char proximoNumeroSequencia(unsigned char sequenceNumber) {
    return (sequenceNumber + 1) % 256;
}