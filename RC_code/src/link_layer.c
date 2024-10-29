#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include "serial_port.h"
#include "link_layer.h"

#define FLAG 0x7E
#define A_TX 0x03
#define A_RX 0x01
#define C_SET 0x03
#define C_UA 0x07
#define C_DISC 0x0B
#define C_DATA 0x01

#define ESCAPE 0x7D
#define C_RR 0x05
#define C_REJ 0x01
unsigned char frame[MAX_FRAME_SIZE];

extern int fd;
LinkLayerRole currentRole;

// Estrutura para armazenar as estatísticas de conexão
typedef struct {
    int tramasEnviadas;
    int tramasRecebidas;
    int tramasRejeitadas;
    int tramasAceitas;
} EstatisticasConexao;

// Instância global para as estatísticas
EstatisticasConexao estatisticas = {0, 0, 0, 0};

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC1_OK,
    DATA,
    STOP_R
} LinkLayerState;

int alarmEnabled = 0;
int alarmCount = 0;

void alarmHandler(int signal) {
    alarmEnabled = 1;
    alarmCount++;
}

void sendSupervisionFrame(int fd, unsigned char address, unsigned char control) {
    unsigned char frame[5] = {FLAG, address, control, address ^ control, FLAG};
    writeBytesSerialPort(frame, 5);
    printf("DEBUG (sendSupervisionFrame): A enviar frame de controlo: 0x%X\n", control);
    estatisticas.tramasEnviadas++;  // Atualizar contagem de tramas enviadas
}

void actualizarEstadisticasEnvio(int aceito) {
    if (aceito) {
        estatisticas.tramasAceitas++;
    } else {
        estatisticas.tramasRejeitadas++;
    }
}

void actualizarEstadisticasRecepcao() {
    estatisticas.tramasRecebidas++;
}

void mostrarEstatisticas() {
    printf("=== Estatísticas da Conexão ===\n");
    printf("Tramas Enviadas: %d\n", estatisticas.tramasEnviadas);
    printf("Tramas Recebidas: %d\n", estatisticas.tramasRecebidas);
    printf("Tramas Rejeitadas: %d\n", estatisticas.tramasRejeitadas);
    printf("Tramas Aceitas: %d\n", estatisticas.tramasAceitas);
    printf("===============================\n");
}

unsigned char calculateCRC(const unsigned char *buf, int bufSize) {
    unsigned char crc = 0;
    for (int i = 0; i < bufSize; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

int llopen(LinkLayer connectionParameters) {
    printf("Entrando em llopen...\n");
    LinkLayerState state = START;
    fd = openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate);
    currentRole = connectionParameters.role;
    if (fd < 0) {
        fprintf(stderr, "Erro ao abrir a porta serial\n");
        return -1;
    }

    printf("Porta serial aberta corretamente em llopen...\n");
    unsigned char byte;
    int timeout = connectionParameters.timeout;
    int retransmissions = connectionParameters.nRetransmissions;

    signal(SIGALRM, alarmHandler);

    switch (connectionParameters.role) {
        case LlTx: {
            printf("Modo Transmissor: A enviar trama SET para estabelecer conexão...\n");
            while (retransmissions > 0 && state != STOP_R) {
                sendSupervisionFrame(fd, A_TX, C_SET);
                printf("Trama SET enviada. Aguardando resposta UA...\n");

                alarm(timeout);
                alarmEnabled = 0;

                while (!alarmEnabled && state != STOP_R) {
                    if (readByteSerialPort(&byte) > 0) {
                        printf("DEBUG (llopen Tx): Estado = %d, Byte recebido = 0x%X\n", state, byte);
                        switch (state) {
                            case START:
                                if (byte == FLAG) state = FLAG_RCV;
                                break;
                            case FLAG_RCV:
                                if (byte == A_RX) state = A_RCV;
                                else if (byte != FLAG) state = START;
                                break;
                            case A_RCV:
                                if (byte == C_UA) state = C_RCV;
                                else if (byte == FLAG) state = FLAG_RCV;
                                else state = START;
                                break;
                            case C_RCV:
                                if (byte == (A_RX ^ C_UA)) state = BCC1_OK;
                                else if (byte == FLAG) state = FLAG_RCV;
                                else state = START;
                                break;
                            case BCC1_OK:
                                if (byte == FLAG) state = STOP_R;
                                else state = START;
                                break;
                            default:
                                break;
                        }
                    }
                }

                if (state == STOP_R) {
                    printf("Conexão estabelecida corretamente (UA recebido)\n");
                    alarm(0);
                    return fd;
                }

                printf("UA não recebido. Tentando novamente...\n");
                retransmissions--;
            }
            printf("Não foi possível estabelecer a conexão após %d tentativas.\n", connectionParameters.nRetransmissions);
            closeSerialPort();
            return -1;
        }

        case LlRx: {
            printf("Modo Receptor: Aguardando trama SET do transmissor...\n");
            while (state != STOP_R) {
                if (readByteSerialPort(&byte) > 0) {
                    printf("DEBUG (llopen Rx): Estado = %d, Byte recebido = 0x%X\n", state, byte);
                    switch (state) {
                        case START:
                            if (byte == FLAG) state = FLAG_RCV;
                            break;
                        case FLAG_RCV:
                            if (byte == A_TX) state = A_RCV;
                            else if (byte != FLAG) state = START;
                            break;
                        case A_RCV:
                            if (byte == C_SET) state = C_RCV;
                            else if (byte == FLAG) state = FLAG_RCV;
                            else state = START;
                            break;
                        case C_RCV:
                            if (byte == (A_TX ^ C_SET)) state = BCC1_OK;
                            else if (byte == FLAG) state = FLAG_RCV;
                            else state = START;
                            break;
                        case BCC1_OK:
                            if (byte == FLAG) state = STOP_R;
                            else state = START;
                            break;
                        default:
                            break;
                    }
                }
            }
            printf("Trama SET recebida corretamente. A enviar UA...\n");
            sendSupervisionFrame(fd, A_RX, C_UA);
            return fd;
        }

        default:
            return -1;
    }
}

int applyByteStuffing(const unsigned char *input, int length, unsigned char *output) {
    int stuffedIndex = 0;
    for (int i = 0; i < length; i++) {
        printf("DEBUG (applyByteStuffing): Byte original = 0x%X\n", input[i]);
        if (input[i] == FLAG) {
            output[stuffedIndex++] = ESCAPE;
            output[stuffedIndex++] = 0x5E;
            printf("DEBUG (applyByteStuffing): Aplicando stuffing FLAG -> ESCAPE + 0x5E\n");
        } else if (input[i] == ESCAPE) {
            output[stuffedIndex++] = ESCAPE;
            output[stuffedIndex++] = 0x5D;
            printf("DEBUG (applyByteStuffing): Aplicando stuffing ESCAPE -> ESCAPE + 0x5D\n");
        } else {
            output[stuffedIndex++] = input[i];
        }
    }
    return stuffedIndex;
}

int llwrite(const unsigned char *buf, int bufSize) {
    unsigned char frame[MAX_FRAME_SIZE];
    int frameIndex = 0;

    frame[frameIndex++] = FLAG;
    frame[frameIndex++] = A_TX;
    frame[frameIndex++] = C_DATA;
    frame[frameIndex++] = A_TX ^ C_DATA;

    unsigned char BCC2 = 0;
    for (int i = 0; i < bufSize; i++) {
        BCC2 ^= buf[i];
    }

    frameIndex += applyByteStuffing(buf, bufSize, &frame[frameIndex]);

    unsigned char stuffedBCC2[2];
    int stuffedBCC2Length = applyByteStuffing(&BCC2, 1, stuffedBCC2);
    for (int i = 0; i < stuffedBCC2Length; i++) {
        frame[frameIndex++] = stuffedBCC2[i];
    }

    frame[frameIndex++] = FLAG;

    signal(SIGALRM, alarmHandler);
    alarmEnabled = 0;

    LinkLayerState state = START;
    int attempts = 3;
    unsigned char byte;

    while (attempts > 0 && state != STOP_R) {
        writeBytesSerialPort(frame, frameIndex);
        estatisticas.tramasEnviadas++;  // Atualizar contagem de tramas enviadas
        printf("DEBUG (llwrite): Trama enviada. Aguardando confirmação...\n");

        alarm(5);
        alarmEnabled = 0;

        while (!alarmEnabled && state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                printf("DEBUG (llwrite): Byte recebido em resposta = 0x%X\n", byte);
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == A_RX) state = A_RCV;
                        else if (byte != FLAG) state = START;
                        break;
                    case A_RCV:
                        if (byte == C_RR) {
                            actualizarEstadisticasEnvio(1);
                            state = STOP_R;
                        } else if (byte == C_REJ) {
                            actualizarEstadisticasEnvio(0);
                            state = START;
                        } else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    default:
                        break;
                }
            }
        }

        if (state == STOP_R) {
            printf("Confirmação de recepção recebida (RR).\n");
            alarm(0);
            return frameIndex;
        } else {
            printf("Confirmação não recebida ou REJ recebido. Tentando novamente...\n");
            attempts--;
        }
    }
    printf("Erro: Não foi possível enviar a trama após várias tentativas.\n");
    return -1;
}

int applyByteDestuffing(const unsigned char *input, int length, unsigned char *output) {
    int destuffedIndex = 0;
    int escape = 0;

    for (int i = 0; i < length; i++) {
        if (escape) {
            if (input[i] == 0x5E) output[destuffedIndex++] = FLAG;
            else if (input[i] == 0x5D) output[destuffedIndex++] = ESCAPE;
            escape = 0;
        } else if (input[i] == ESCAPE) {
            escape = 1;
        } else {
            output[destuffedIndex++] = input[i];
        }
    }

    return destuffedIndex;
}

int llread(unsigned char *packet) {
    LinkLayerState state = START;
    unsigned char rawFrame[MAX_FRAME_SIZE];
    int rawIndex = 0;
    unsigned char byte;
    unsigned char BCC2 = 0;

    printf("DEBUG (llread): Aguardando trama de dados...\n");

    while (state != STOP_R) {
        if (readByteSerialPort(&byte) > 0) {
            printf("DEBUG (llread): Estado = %d, Byte recebido = 0x%X\n", state, byte);

            switch (state) {
                case START:
                    if (byte == FLAG) state = FLAG_RCV;
                    break;
                case FLAG_RCV:
                    if (byte == A_TX) state = A_RCV;
                    else if (byte != FLAG) state = START;
                    break;
                case A_RCV:
                    if (byte == C_DATA) state = C_RCV;
                    else if (byte == FLAG) state = FLAG_RCV;
                    else state = START;
                    break;
                case C_RCV:
                    if (byte == (A_TX ^ C_DATA)) state = BCC1_OK;
                    else if (byte == FLAG) state = FLAG_RCV;
                    else state = START;
                    break;
                case BCC1_OK:
                    if (byte != FLAG) {
                        rawFrame[rawIndex++] = byte;
                        state = DATA;
                    }
                    break;
                case DATA:
                    if (byte == FLAG) {
                        int destuffedSize = applyByteDestuffing(rawFrame, rawIndex, packet);
                        for (int i = 0; i < destuffedSize; i++) {
                            BCC2 ^= packet[i];
                        }

                        if (BCC2 == 0) {
                            printf("DEBUG (llread): Trama recebida corretamente. A enviar RR...\n");
                            sendSupervisionFrame(fd, A_RX, C_RR);
                            actualizarEstadisticasRecepcao();
                            state = STOP_R;
                        } else {
                            printf("Erro: BCC2 incorreto. A enviar REJ...\n");
                            sendSupervisionFrame(fd, A_RX, C_REJ);
                            actualizarEstadisticasEnvio(0);
                            return -1;
                        }
                    } else {
                        rawFrame[rawIndex++] = byte;
                    }
                    break;
                default:
                    break;
            }
        }
    }

    return rawIndex;
}

int llclose(int showStatistics) {
    LinkLayerState state = START;
    unsigned char byte;

    if (currentRole == LlTx) {
        printf("Modo Transmissor: A iniciar encerramento da conexão...\n");
        sendSupervisionFrame(fd, A_TX, C_DISC);
        printf("Trama DISC enviada. Aguardando resposta DISC do receptor...\n");

        while (state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == A_RX) state = A_RCV;
                        else if (byte != FLAG) state = START;
                        break;
                    case A_RCV:
                        if (byte == C_DISC) state = C_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case C_RCV:
                        if (byte == (A_RX ^ C_DISC)) state = BCC1_OK;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case BCC1_OK:
                        if (byte == FLAG) state = STOP_R;
                        else state = START;
                        break;
                    default:
                        break;
                }
            }
        }
        actualizarEstadisticasRecepcao();
        printf("Trama DISC recebida do receptor. A enviar UA para finalizar...\n");
        sendSupervisionFrame(fd, A_TX, C_UA);
    } else if (currentRole == LlRx) {
        printf("Modo Receptor: Aguardando trama DISC do transmissor...\n");

        while (state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == A_TX) state = A_RCV;
                        else if (byte != FLAG) state = START;
                        break;
                    case A_RCV:
                        if (byte == C_DISC) state = C_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case C_RCV:
                        if (byte == (A_TX ^ C_DISC)) state = BCC1_OK;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case BCC1_OK:
                        if (byte == FLAG) state = STOP_R;
                        else state = START;
                        break;
                    default:
                        break;
                }
            }
        }
        actualizarEstadisticasRecepcao();
        printf("Trama DISC recebida do transmissor. A enviar DISC para confirmar desconexão...\n");
        sendSupervisionFrame(fd, A_RX, C_DISC);

        state = START;
        while (state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == A_TX) state = A_RCV;
                        else if (byte != FLAG) state = START;
                        break;
                    case A_RCV:
                        if (byte == C_UA) state = C_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case C_RCV:
                        if (byte == (A_TX ^ C_UA)) state = BCC1_OK;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case BCC1_OK:
                        if (byte == FLAG) state = STOP_R;
                        else state = START;
                        break;
                    default:
                        break;
                }
            }
        }
        actualizarEstadisticasRecepcao();
        printf("UA recebido do transmissor. Encerramento da conexão completo.\n");
    }

    if (showStatistics) {
        mostrarEstatisticas();
    }

    closeSerialPort();
    return 0;
}