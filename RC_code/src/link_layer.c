// Implementação do protocolo da camada de enlace
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include "link_layer.h"
#include "serial_port.h"


#define _POSIX_SOURCE 1
#define FALSE 0
#define TRUE 1
#define BUFFER_SIZE 256


// Definir os estados possíveis para a máquina de estados
#define START 0
#define FLAG_RECEIVED 1
#define A_RECEIVED 2
#define C_RECEIVED 3
#define BCC_VALID 4
#define STOP 5

LinkLayerRole globalRole;
int globalRetransmissions;

typedef struct {
    unsigned char FLAG;
    unsigned char A_TRANSMISSOR;
    unsigned char A_RECEPTOR;
    unsigned char CTRL_SET;
    unsigned char CTRL_UA;
    unsigned char CTRL_RR;
    unsigned char CTRL_REJ;
} Protocolo;
// Inicialização dos valores de controle do protocolo (tramas, bits de controle)
Protocolo protocolo = {0x7E, 0x03, 0x01, 0x03, 0x07, 0x05, 0x01};

// Variáveis globais para controle de comunicação e timeout
extern int fd;
int alarmEnabled = FALSE; // Estado do alarme
int alarmCount = 0; // Contador de alarmes
int globalTimeout;

// Manipulador do alarme para controlar timeouts. Incrementa alarmCount a cada timeout.
void alarmHandler(int signal) {
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarme #%d\n", alarmCount);
}

// Função para aplicar byte stuffing nos dados enviados, substituindo FLAG e 0x7D para evitar 
// confusão com o início e fim das tramas
static int applyByteStuffing(const unsigned char *input, int length, unsigned char *output) {
    int stuffedIndex = 0;
    for (int i = 0; i < length; i++) {
        if (input[i] == protocolo.FLAG) {
            output[stuffedIndex++] = 0x7D;
            output[stuffedIndex++] = 0x5E;
        } else if (input[i] == 0x7D) {
            output[stuffedIndex++] = 0x7D;
            output[stuffedIndex++] = 0x5D;
        } else {
            output[stuffedIndex++] = input[i];
        }
    }
    return stuffedIndex;
}

// Validação do campo BCC (utilizado para detecção de erros), calculando XOR nos dados recebidos
static int validateBCC(const unsigned char *data, int length, unsigned char BCC2) {
    unsigned char calculatedBCC = 0x00;
    for (int i = 0; i < length; i++) {
        calculatedBCC ^= data[i];
    }
    return calculatedBCC == BCC2;
}

// Função para gerenciar a resposta do receptor (RR, REJ, UA), trocando estados de acordo com 
// os bytes e o campo de controle esperados.
static int handleResponse(int expectedControlField) {
    unsigned char byte;
    int currentState = START;
    while (alarmEnabled && currentState != STOP) {
        int res = readByteSerialPort(&byte);
        if (res > 0) {
            switch (currentState) {
                case START:
                    if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    break;
                case FLAG_RECEIVED:
                    if (byte == protocolo.A_RECEPTOR) currentState = A_RECEIVED;
                    else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case A_RECEIVED:
                    if (byte == expectedControlField) currentState = C_RECEIVED;
                    else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case C_RECEIVED:
                    if (byte == (protocolo.A_RECEPTOR ^ expectedControlField)) currentState = BCC_VALID;
                    else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case BCC_VALID:
                    if (byte == protocolo.FLAG) currentState = STOP;
                    else currentState = START;
                    break;
                default:
                    currentState = START;
                    break;
            }
        }
    }
    return currentState == STOP ? 0 : -1;
}

// Função para aplicar o byte destuffing aos dados recebidos
// Retorna o índice final dos dados destuffed
static int applyByteDestuffing(const unsigned char *input, int length, unsigned char *output) {
    int destuffedIndex = 0;
    for (int i = 0; i < length; i++) {
        if (input[i] == 0x7D) {
            if (i + 1 < length) {
                if (input[i + 1] == 0x5E) {
                    output[destuffedIndex++] = 0x7E;
                    i++;
                } else if (input[i + 1] == 0x5D) {
                    output[destuffedIndex++] = 0x7D;
                    i++;
                } else {
                    output[destuffedIndex++] = input[i];
                }
            }
        } else {
            output[destuffedIndex++] = input[i];
        }
    }
    return destuffedIndex;
}

////////////////////////////////////////////////
// LLOPEN - Abre a conexão serial
////////////////////////////////////////////////
// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: o descritor da porta serial se bem-sucedido, -1 caso contrário
int llopen(LinkLayer connectionParameters) {
    globalRole = connectionParameters.role;
    globalRetransmissions = connectionParameters.nRetransmissions;
    fd = openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate);
    
    if (fd < 0) {
        perror("[Erro] ao abrir a porta serial");
        return -1;
    }
    printf("Porta serial aberta com sucesso: %s\n", connectionParameters.serialPort);

    (void) signal(SIGALRM, alarmHandler);

    if (connectionParameters.role == LlTx) {
        // Construir e enviar a trama SET
        unsigned char setFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_SET, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET, protocolo.FLAG};
        
        for (int i = 0; i < connectionParameters.nRetransmissions; i++) {
            alarmEnabled = FALSE;
            writeBytesSerialPort(setFrame, sizeof(setFrame));
            printf("Trama SET enviada (Tentativa %d)\n", i + 1);

            alarmEnabled = TRUE;
            alarm(connectionParameters.timeout);
             // Retransmissão com base no número de tentativas
            if (handleResponse(protocolo.CTRL_UA) == 0) {
                printf("Trama UA recebida corretamente!\n");
                alarm(0);
                alarmEnabled = FALSE;
                return fd;
            } else {
                printf("Não foi recebida a trama UA, a retransmitir...\n");
                alarmEnabled = FALSE;
            }
        }
        printf("Erro: não foi possível estabelecer a conexão após várias tentativas\n");
        return -1;
    } else if (connectionParameters.role == LlRx) {
        alarmEnabled = TRUE;
        alarm(connectionParameters.timeout);
        int result = handleResponse(protocolo.CTRL_SET);

        alarm(0);
        alarmEnabled = FALSE;
        
        if (result == 0) {
            printf("Trama SET recebida corretamente, enviando UA.\n");
            unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, protocolo.CTRL_UA, protocolo.A_RECEPTOR ^ protocolo.CTRL_UA, protocolo.FLAG};
            writeBytesSerialPort(uaFrame, sizeof(uaFrame));
            return fd;
        } else {
            printf("Erro: não foi possível estabelecer a conexão.\n");
            return -1;
        }
    }

    return -1;
}
////////////////////////////////////////////////
// LLWRITE - Envia uma trama de dados
////////////////////////////////////////////////
// Parâmetros:
//   buf: ponteiro para o buffer que contém os dados a serem enviados
//   bufSize: tamanho do buffer de dados
// Retorna:
//   0 se a trama for enviada com sucesso e confirmada, -1 em caso de erro
int llwrite(const unsigned char *buf, int bufSize) {
    unsigned char frame[BUFFER_SIZE];
    unsigned char stuffedFrame[BUFFER_SIZE];
    int retries = 0;
     // Construir a trama de dados com FLAG, endereço, campo de controle e BCC1
    frame[0] = protocolo.FLAG;
    frame[1] = protocolo.A_TRANSMISSOR;
    frame[2] = 0x00;
    frame[3] = frame[1] ^ frame[2];
    memcpy(frame + 4, buf, bufSize);
    // Aplicar o byte stuffing aos dados da trama
    int stuffedIndex = applyByteStuffing(frame, bufSize + 4, stuffedFrame);
    stuffedFrame[stuffedIndex++] = protocolo.FLAG;
    // Loop de retransmissão para enviar a trama até atingir o limite de tentativas
    while (retries < globalTimeout) {
        int bytes_written = writeBytesSerialPort(stuffedFrame, stuffedIndex);
        if (bytes_written < 0) {
            printf("Erro ao enviar a trama de dados\n");
            return -1;
        }
        printf("Trama enviada (tentativa %d)\n", retries + 1);
        // Configurar alarme e aguardar resposta (RR ou REJ)
        alarmEnabled = FALSE;
        signal(SIGALRM, alarmHandler);
        alarm(globalTimeout);
        // Processar resposta usando handleResponse para verificar RR
        if (handleResponse(protocolo.CTRL_RR) == 0) {
            alarm(0);
            return 0;
        }
        retries++;
        printf("Tentativa %d de %d falhou, retransmitindo...\n", retries, globalTimeout);
    }

    printf("Erro: número máximo de tentativas excedido.\n");
    return -1;
}

////////////////////////////////////////////////
// LLREAD - Lê uma trama de dados 
////////////////////////////////////////////////
// Parâmetros:
//   packet: ponteiro para o buffer onde os dados recebidos serão armazenados
// Retorna:
//   O tamanho do pacote de dados (sem FLAG, A, C, e BCC1) se for recebido corretamente,
//   -1 em caso de erro.
int llread(unsigned char *packet) {
    unsigned char frame[BUFFER_SIZE];
    unsigned char destuffedFrame[BUFFER_SIZE];
    unsigned char BCC2 = 0x00;
    int index = 0;
    int received_packet = 0;
    int currentState = START;

    while (!received_packet) {
        // Ler um byte da porta serial
        int bytes_read = readByteSerialPort(&frame[index]);
        if (bytes_read < 0) {
            perror("Erro ao ler a trama");
            return -1;
        }

        if (bytes_read > 0) {
            // Máquina de estados para processar cada byte recebido
            switch (currentState) {
                case START:
                    if (frame[index] == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    break;
                case FLAG_RECEIVED:
                    if (frame[index] == protocolo.A_TRANSMISSOR) currentState = A_RECEIVED;
                    else if (frame[index] == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case A_RECEIVED:
                    if (frame[index] == 0x00) currentState = C_RECEIVED;
                    else if (frame[index] == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case C_RECEIVED:
                    if (frame[index] == (protocolo.A_TRANSMISSOR ^ 0x00)) currentState = BCC_VALID;
                    else if (frame[index] == protocolo.FLAG) currentState = FLAG_RECEIVED;
                    else currentState = START;
                    break;
                case BCC_VALID:
                    if (frame[index] == protocolo.FLAG) {
                        int destuffedIndex = applyByteDestuffing(frame, index, destuffedFrame);
                        if (validateBCC(destuffedFrame, destuffedIndex - 1, BCC2)) {
                            memcpy(packet, destuffedFrame, destuffedIndex - 1);
                            received_packet = 1;
                            return destuffedIndex - 1;
                        } else {
                            printf("Erro: BCC2 incorreto, enviando REJ.\n");
                            unsigned char rejFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, protocolo.CTRL_REJ, protocolo.A_RECEPTOR ^ protocolo.CTRL_REJ, protocolo.FLAG};
                            writeBytesSerialPort(rejFrame, sizeof(rejFrame));
                            currentState = START;
                        }
                    } else {
                        frame[index] = frame[index];
                        index++;
                    }
                    break;
                default:
                    currentState = START;
                    break;
            }
        }
    }

    return -1;
}
////////////////////////////////////////////////
// LLCLOSE - Fecha a conexão
////////////////////////////////////////////////
// Parâmetros:
//   showStatistics: indica se as estatísticas devem ser mostradas após o fechamento da conexão
// Retorna:
//   0 se a conexão for fechada corretamente, -1 em caso de erro.
int llclose(int showStatistics) {
    // Configurar o manipulador de alarme
    signal(SIGALRM, alarmHandler);
    int state = START;
     // Se o role for de transmissor (TRANSMITTER)
    if (globalRole == LlTx) {
        for (int retransmitions = globalRetransmissions; retransmitions > 0 && state != STOP; retransmitions--) {
            unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, 0x0B, protocolo.A_TRANSMISSOR ^ 0x0B, protocolo.FLAG};
            writeBytesSerialPort(discFrame, sizeof(discFrame));
            alarm(globalTimeout);
            state = handleResponse(0x0B) == 0 ? STOP : START;
        }
        unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_UA, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_UA, protocolo.FLAG};
        writeBytesSerialPort(uaFrame, sizeof(uaFrame));
    } else if (globalRole == LlRx) {
        handleResponse(0x0B);
        unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, 0x0B, protocolo.A_RECEPTOR ^ 0x0B, protocolo.FLAG};
        writeBytesSerialPort(discFrame, sizeof(discFrame));
        handleResponse(protocolo.CTRL_UA);
    }
    // Mostrar estatísticas se showStatistics for verdadeiro
    if (showStatistics) {
        printf("\n---ESTATÍSTICAS---\n\n Número de timeouts: %d\n", alarmCount);
    }

    return closeSerialPort(fd);
}