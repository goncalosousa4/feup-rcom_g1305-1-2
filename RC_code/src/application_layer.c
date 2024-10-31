#include <stdio.h>     
#include <stdlib.h>    
#include <string.h>    
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include "application_layer.h"
#include "link_layer.h"
#include "serial_port.h"

// Declaración de funciones auxiliares utilizadas en la capa de aplicación
static int startTransmission(const char *filename);
static int startReception(const char *filename);
static FILE* openFile(const char *filename, const char *mode);
static long calculateFileSize(FILE *file);
static unsigned char* createControlPacket(unsigned char type, const char *filename, long fileSize);
static int sendControlPacket(unsigned char *packet, int packetSize);
static unsigned char* createDataPacket(unsigned char sequence, const unsigned char *data, int dataSize);
static unsigned char getNextSequence(unsigned char sequence);

////////////////////////////////////////////////
// APPLICATION LAYER - Gestor principal da camada de aplicação
////////////////////////////////////////////////
// Parâmetros:
//   serialPort: porta serial para comunicação
//   mode: "tx" para transmissão, "rx" para recepção
//   baudRate: taxa de transmissão
//   maxRetries: número de tentativas de retransmissão
//   waitTime: tempo limite de espera para retransmissão
//   filename: nome do arquivo a ser enviado ou recebido
void applicationLayer(const char *serialPort, const char *mode, int baudRate, int maxRetries, int waitTime, const char *filename) {
    // Configuração dos parâmetros para a camada de enlace
    LinkLayer config = {
        .baudRate = baudRate,
        .timeout = waitTime,
        .nRetransmissions = maxRetries,
        .role = strcmp(mode, "tx") == 0 ? LlTx : LlRx,
    };
    strcpy(config.serialPort, serialPort);

    // Abre a conexão serial usando llopen
    if (llopen(config) < 0) {
        perror("Erro ao abrir conexão\n");
        exit(-1);
    }

    // Marca o tempo de início para medir a duração da transmissão
    clock_t start = clock();

    // Escolha entre transmissão e recepção, conforme o papel da conexão
    if (config.role == LlTx) {
        if (startTransmission(filename) < 0) {
            perror("Erro durante a transmissão\n");
            exit(-1);
        }
    } else if (config.role == LlRx) {
        if (startReception(filename) < 0) {
            perror("Erro durante a recepção\n");
            exit(-1);
        }
    }

    // Calcula e exibe o tempo de transmissão total
    clock_t end = clock();
    printf("Tempo de transmissão: %.2f segundos\n", (double)(end - start) / CLOCKS_PER_SEC);

    // Fecha a conexão serial
    llclose(1);
}

// Inicia a transmissão de um arquivo
static int startTransmission(const char *filename) {
    // Abre o arquivo em modo de leitura
    FILE *file = openFile(filename, "rb");
    if (!file) return -1;

    // Calcula o tamanho do arquivo para incluir no pacote de controle
    long fileSize = calculateFileSize(file);

    // Envia o pacote de controle inicial com informações do arquivo
    unsigned char *controlPacket = createControlPacket(0x02, filename, fileSize);
    if (sendControlPacket(controlPacket, strlen((char *)controlPacket) + 1) < 0) {
        free(controlPacket);
        return -1;
    }
    free(controlPacket);

    // Variáveis para gerenciar sequência e buffer de dados
    unsigned char sequence = 0;
    unsigned char buffer[256];
    int bytesRead;

    // Lê e envia os dados do arquivo em pacotes de tamanho fixo até o final do arquivo
    while ((bytesRead = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        // Cria o pacote de dados com o número de sequência atual
        unsigned char *dataPacket = createDataPacket(sequence, buffer, bytesRead);
        // Envia o pacote e verifica erros
        if (llwrite(dataPacket, bytesRead + 4) < 0) {
            free(dataPacket);
            return -1;
        }
        sequence = getNextSequence(sequence);  // Atualiza a sequência
        free(dataPacket);
    }

    // Envia o pacote de controle final indicando o término da transmissão
    controlPacket = createControlPacket(0x03, filename, fileSize);
    if (sendControlPacket(controlPacket, strlen((char *)controlPacket) + 1) < 0) {
        free(controlPacket);
        return -1;
    }
    free(controlPacket);

    // Fecha o arquivo após a transmissão completa
    fclose(file);
    return 0;
}

// Inicia a recepção de um arquivo
static int startReception(const char *filename) {
    // Abre o arquivo em modo de escrita
    FILE *file = openFile(filename, "wb");
    if (!file) return -1;

    unsigned char buffer[512];
    int packetSize;

    // Recebe pacotes até o final do arquivo (pacote de controle final)
    while ((packetSize = llread(buffer)) > 0) {
        if (buffer[0] == 0x01) {  // Verifica se é um pacote de dados
            fwrite(buffer + 4, sizeof(unsigned char), packetSize - 4, file);
        } else if (buffer[0] == 0x03) {  // Pacote de controle final
            break;
        }
    }

    fclose(file);  // Fecha o arquivo após a recepção completa
    return packetSize < 0 ? -1 : 0;
}

// Abre um arquivo com o modo especificado
static FILE* openFile(const char *filename, const char *mode) {
    FILE *file = fopen(filename, mode);
    if (!file) {
        perror("Erro ao abrir o arquivo\n");
    }
    return file;
}

// Calcula o tamanho do arquivo
static long calculateFileSize(FILE *file) {
    // Move o ponteiro do arquivo para o final para obter o tamanho
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    // Retorna o ponteiro ao início
    fseek(file, 0, SEEK_SET);
    return size;
}

// Cria um pacote de controle para iniciar ou terminar a transmissão
static unsigned char* createControlPacket(unsigned char type, const char *filename, long fileSize) {
    int filenameSize = strlen(filename);
    unsigned char *packet = malloc(7 + filenameSize);

    // Define o tipo de controle e comprimento do tamanho do arquivo
    packet[0] = type;
    packet[1] = 0;
    packet[2] = sizeof(long);

    // Insere o tamanho do arquivo no pacote
    for (int i = sizeof(long) - 1; i >= 0; i--) {
        packet[3 + i] = (fileSize >> (8 * i)) & 0xFF;
    }

    // Insere o nome do arquivo no pacote
    packet[3 + sizeof(long)] = 1;
    packet[4 + sizeof(long)] = filenameSize;
    memcpy(packet + 5 + sizeof(long), filename, filenameSize);

    return packet;
}

// Cria um pacote de dados com número de sequência
static unsigned char* createDataPacket(unsigned char sequence, const unsigned char *data, int dataSize) {
    unsigned char *packet = malloc(dataSize + 4);

    // Estrutura do pacote de dados: flag, sequência e tamanho
    packet[0] = 0x01;
    packet[1] = sequence;
    packet[2] = (dataSize >> 8) & 0xFF;
    packet[3] = dataSize & 0xFF;
    memcpy(packet + 4, data, dataSize);  // Adiciona os dados

    return packet;
}

// Envia um pacote de controle e verifica se foi bem-sucedido
static int sendControlPacket(unsigned char *packet, int packetSize) {
    int result = llwrite(packet, packetSize);
    if (result < 0) {
        perror("Erro ao enviar o pacote de controle\n");
    }
    return result;
}

// Atualiza o número de sequência
static unsigned char getNextSequence(unsigned char sequence) {
    return (sequence + 1) % 256;  // Garante que o número de sequência é cíclico entre 0 e 255
}