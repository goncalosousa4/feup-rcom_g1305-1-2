// Link layer protocol implementation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include "link_layer.h"
#include "serial_port.h"

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source
// Definir os estados possíveis usando macros
#define START 0
#define FLAG_RECEIVED 1
#define A_RECEIVED 2
#define C_RECEIVED 3
#define BCC_VALID 4
#define STOP 5


// Estrutura para armazenar os valores do protocolo
typedef struct {
    unsigned char FLAG;
    unsigned char A_TRANSMISSOR;
    unsigned char A_RECEPTOR;
    unsigned char CTRL_SET;
    unsigned char CTRL_UA;
    unsigned char CTRL_RR;  // RR para ACK
    unsigned char CTRL_REJ; // REJ para NACK
} Protocolo;

// Inicialização dos valores do protocolo
Protocolo protocolo = {0x7E, 0x03, 0x01, 0x03, 0x07, 0x05, 0x01};

int fd;
int alarmEnabled = FALSE;
int alarmCount = 0;
int globalTimeout;


// Manipulador do alarme para controlar timeouts
void alarmHandler(int signal) {
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarme #%d\n", alarmCount);
}

// Função auxiliar para aplicar byte stuffing aos dados enviados
// Esta função substitui caracteres FLAG e 0x7D nos dados para evitar
// confusão com o fim e início de trama
int applyByteStuffing(const unsigned char *input, int length, unsigned char *output) {
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

// Função auxiliar para validar o campo de verificação BCC2 dos dados recebidos
// Realiza o cálculo XOR para verificar se os dados recebidos estão corretos
int validateBCC(const unsigned char *data, int length, unsigned char BCC2) {
    unsigned char calculatedBCC = 0x00;
    for (int i = 0; i < length; i++) {
        calculatedBCC ^= data[i];
    }
    return calculatedBCC == BCC2;
}

// Função auxiliar para tratar a resposta do receptor (RR, REJ ou UA)
// Esta função processa a resposta do receptor, mudando o estado
// conforme os bytes recebidos e o campo de controle esperado
int handleResponse(int expectedControlField) {
    unsigned char byte;
    int currentState = START;
    while (alarmEnabled && currentState != STOP) {
        int res = readByteSerialPort(fd, &byte);
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


////////////////////////////////////////////////
// LLOPEN - Abre a conexão serial
////////////////////////////////////////////////
// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: o descritor da porta serial se bem-sucedido, -1 caso contrário
int llopen(LinkLayer connectionParameters) {
    unsigned char buffer[BUFFER_SIZE];
    fd = openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate);
    if (fd < 0) {
        perror("[Erro] ao abrir a porta serial");
        return -1;
    }
   
    // Configuração do alarme
    (void) signal(SIGALRM, alarmHandler);

    if (connectionParameters.role == LlTx) {
        // Construir e enviar a trama SET
        unsigned char setFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_SET, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET, protocolo.FLAG};
        int currentState = START;
        int res;

        // Retransmissão com base no número de tentativas
        for (int i = 0; i < connectionParameters.nRetransmissions; i++) {
            // Reiniciar o estado e enviar a trama SET
            alarmEnabled = FALSE;
            writeBytesSerialPort(fd, setFrame, sizeof(setFrame));
            printf("Trama SET enviada (Tentativa %d)\n", i + 1);

            // Ativar o alarme para esperar pela resposta UA
            alarmEnabled = TRUE;
            alarm(connectionParameters.timeout);
            globalTimeout = connectionParameters.timeout;

            // Processar a resposta UA usando handleResponse
            if (handleResponse(protocolo.CTRL_UA) == 0) {
                printf("Trama UA recebida corretamente!\n");
                alarm(0);  // Desativar o alarme
                alarmEnabled = FALSE;  // Reiniciar alarmEnabled
                return fd;
            } else {
                printf("Não foi recebida a trama UA, a retransmitir...\n");
                currentState = START;
                alarmEnabled = FALSE;
            }
        }

        printf("Erro: não foi possível estabelecer a conexão após várias tentativas\n");
        return -1;
    }
    else if (connectionParameters.role == LlRx) {
        // Usa handleResponse para verificar a resposta SET esperada
        return handleResponse(protocolo.CTRL_SET) == 0 ? fd : -1;
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
    int ack_received = 0;
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
        // Enviar a trama com stuffing aplicado
        int bytes_written = write(fd, stuffedFrame, stuffedIndex);
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

    while (!received_packet) {
        // Ler um byte da porta serial
        int bytes_read = readByteSerialPort(fd, &frame[index], 1);
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
                        // Realizar o byte destuffing e verificar BCC2
                        int destuffedIndex = applyByteDestuffing(frame, index, destuffedFrame);
                        if (validateBCC(destuffedFrame, destuffedIndex - 1, BCC2)) {
                            memcpy(packet, destuffedFrame, destuffedIndex - 1);
                            received_packet = 1;
                            return destuffedIndex - 1;
                        } else {
                            printf("Erro: BCC2 incorreto, enviando REJ.\n");
                            unsigned char rejFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, protocolo.CTRL_REJ, protocolo.A_RECEPTOR ^ protocolo.CTRL_REJ, protocolo.FLAG};
                            writeBytesSerialPort(fd, rejFrame, sizeof(rejFrame));
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
    unsigned char byte;

    // Se o role for de transmissor (TRANSMITTER)
    if (connectionP.role == LlTx) {
        for (int retransmitions = connectionP.nRetransmissions; retransmitions > 0 && state != STOP; retransmitions--) {
            unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, 0x0B, protocolo.A_TRANSMISSOR ^ 0x0B, protocolo.FLAG};
            writeBytesSerialPort(fd, discFrame, sizeof(discFrame));
            alarm(connectionP.timeout);
            state = handleResponse(0x0B) == 0 ? STOP : START;
        }
        unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_UA, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_UA, protocolo.FLAG};
        writeBytesSerialPort(fd, uaFrame, sizeof(uaFrame));
    } else if (connectionP.role == LlRx) {
        handleResponse(0x0B);
        unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, 0x0B, protocolo.A_RECEPTOR ^ 0x0B, protocolo.FLAG};
        writeBytesSerialPort(fd, discFrame, sizeof(discFrame));
        handleResponse(protocolo.CTRL_UA);
    }

    // Mostrar estatísticas se showStatistics for verdadeiro
    if (showStatistics) {
        printf("\n---ESTATÍSTICAS---\n\n Número de timeouts: %d\n", alarmCount);
    }

    return closeSerialPort(fd);
}