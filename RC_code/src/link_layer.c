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
#define FALSE 0
#define TRUE 1
#define BUFFER_SIZE 256
#define TRANSMITTER 0
#define RECEIVER 1

// Definição dos estados 
#define START 0
#define FLAG_RECEIVED 1
#define A_RECEIVED 2
#define C_RECEIVED 3
#define BCC_VALID 4
#define STOP 5


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


// Manipulador do alarme
void alarmHandler(int signal) {
    alarmEnabled = FALSE;
    alarmCount++;
    printf("Alarme #%d\n", alarmCount);
}

// Função para configurar a porta serial
int configurePort(LinkLayer connectionParameters) {
    struct termios oldtio, newtio;
    
    // Abrir a porta serial
    fd = open(connectionParameters.serialPort, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("Erro ao abrir a porta serial");
        return -1;
    }

    // Obter a configuração atual da porta e guardar em oldtio
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("Erro ao obter a configuração da porta");
        return -1;
    }
    // Clear struct for new port settings
    // Configurar a nova estrutura para definir a porta
    memset(&newtio, 0, sizeof(newtio));


    newtio.c_cflag = connectionParameters.baudRate | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    // Set input mode
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1;
    newtio.c_cc[VMIN] = 0;

    // VTIME e VMIN should be changed in order to protect with a
    // timeout the reception of the following character(s)

    // Now clean the line and activate the settings for the port
    // tcflush() discards data written to the object referred to
    // by fd but not transmitted, or data received but not read,
    // depending on the value of queue_selector:
    //   TCIFLUSH - flushes data received but not read.

    // Limpar o buffer da porta e aplicar a nova configuração
    tcflush(fd, TCIOFLUSH);

    // Set new port settings
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("Erro ao configurar a porta");
        return -1;
    }

    return 0;
}
////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters){

    unsigned char buffer[BUFFER_SIZE];
    

    // Configuração da porta serial usando configurePort
    if (configurePort(connectionParameters) < 0) {
        return -1;
    }
   
    // Configuração do alarme
    (void) signal(SIGALRM, alarmHandler);

    if (connectionParameters.role == TRANSMITTER) {
        // Construir e enviar a trama SET
        unsigned char setFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_SET, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET, protocolo.FLAG};
        // Máquina de estados para processar a resposta UA
        int currentState = START;
        int res;
        // Retransmissão com base no número de tentativas
        for (int i = 0; i < connectionParameters.nRetransmissions; i++) {
            // Reiniciar alarmEnabled antes de enviar novamente
            alarmEnabled = FALSE;
            write(fd, setFrame, sizeof(setFrame));
            printf("Trama SET enviada (Tentativa %d)\n", i + 1);


            // Ativar o alarme
            alarmEnabled = TRUE;
            alarm(connectionParameters.timeout);
            globalTimeout = connectionParameters.timeout;

            // Processar a resposta UA utilizando a máquina de estados
            while (alarmEnabled && currentState != STOP) {
                unsigned char byte = 0;
                res = read(fd, &byte, sizeof(byte));
                if (res < 0) {
                    perror("Erro ao ler a trama");
                    return -1;
                }
                                // Atualizar o estado com base no byte recebido
                if (res > 0) {
                    switch (currentState) {
                        case START:
                            printf("Estado: START\n");
                            if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                            break;
                        case FLAG_RECEIVED:
                            printf("Estado: FLAG_RECEIVED\n");
                            if (byte == protocolo.A_RECEPTOR) currentState = A_RECEIVED;
                            else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                            else currentState = START;
                            break;
                        case A_RECEIVED:
                            printf("Estado: A_RECEIVED\n");
                            if (byte == protocolo.CTRL_UA) currentState = C_RECEIVED;
                            else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                            else currentState = START;
                            break;
                        case C_RECEIVED:
                            printf("Estado: C_RECEIVED\n");
                            if (byte == (protocolo.A_RECEPTOR ^ protocolo.CTRL_UA)) currentState = BCC_VALID;
                            else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                            else currentState = START;
                            break;
                        case BCC_VALID:
                            printf("Estado: BCC_VALID\n");
                            if (byte == protocolo.FLAG) {
                                currentState = STOP;
                            } else {
                                currentState = START;
                            }
                            break;
                        default:
                            printf("Estado: DEFAULT\n");
                            currentState = START;
                            break;
                    }
                }
            }
                
            // Verificar se a trama UA foi recebida corretamente
            if (currentState == STOP) {
                printf("Trama UA recebida corretamente!\n");
                alarm(0);  // Desativar o alarme
                alarmEnabled = FALSE;  // Reiniciar alarmEnabled
                return fd;
            } else {
                printf("Não foi recebida a trama UA, a retransmitir...\n");
                currentState = START; // Reiniciar a máquina de estados para a próxima tentativa
                alarmEnabled = FALSE;
            }
        }

        printf("Erro: não foi possível estabelecer a conexão após várias tentativas\n");
        return -1;
    }
    else if (connectionParameters.role == RECEIVER) {
        int currentState = START;
        while (1) {
            unsigned char byte = 0;
            int res = read(fd, &byte, sizeof(byte));
            if (res < 0) {
                perror("Erro ao ler a trama");
                return -1;
            }

            // Atualizar o estado com base no byte recebido
            if (res > 0) {
                switch (currentState) {
                    case START:
                        printf("Estado: START\n");
                        if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                        break;
                    case FLAG_RECEIVED:
                        printf("Estado: FLAG_RECEIVED\n");
                        if (byte == protocolo.A_TRANSMISSOR) currentState = A_RECEIVED;
                        else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                        else currentState = START;
                        break;
                    case A_RECEIVED:
                        printf("Estado: A_RECEIVED\n");
                        if (byte == protocolo.CTRL_SET) currentState = C_RECEIVED;
                        else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                        else currentState = START;
                        break;
                    case C_RECEIVED:
                        printf("Estado: C_RECEIVED\n");
                        if (byte == (protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET)) currentState = BCC_VALID;
                        else if (byte == protocolo.FLAG) currentState = FLAG_RECEIVED;
                        else currentState = START;
                        break;
                    case BCC_VALID:
                        printf("Estado: BCC_VALID\n");
                        if (byte == protocolo.FLAG) {
                            currentState = STOP;
                            unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, protocolo.CTRL_UA, protocolo.A_RECEPTOR ^ protocolo.CTRL_UA, protocolo.FLAG};
                            write(fd, uaFrame, sizeof(uaFrame));
                            printf("Trama UA enviada\n");
                            return fd;
                        } else {
                            currentState = START;
                        }
                        break;
                    default:
                        printf("Estado: DEFAULT\n");
                        currentState = START;
                        break;
                }
            }
        }
    }

    return -1;
    /*if (openSerialPort(connectionParameters.serialPort,
                       connectionParameters.baudRate) < 0)
    {
        return -1;
    }*/

    // TODO
}


   

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(const unsigned char *buf, int bufSize)
{
    unsigned char frame[BUFFER_SIZE];
    unsigned char response[BUFFER_SIZE];
    int ack_received = 0;
    int currentState = START;

    // Construir a trama de dados
    frame[0] = protocolo.FLAG;  // Início da trama
    frame[1] = protocolo.A_TRANSMISSOR;  // Endereço
    frame[2] = 0x00;  // Controle (número de sequência ou controle de fluxo)
    frame[3] = frame[1] ^ frame[2];  // BCC1 (controle de erros simples)
    memcpy(&frame[4], buf, bufSize);  // Adicionar os dados à trama
    frame[4 + bufSize] = protocolo.FLAG;  // Fim da trama

    // Enviar a trama de dados
    int bytes_written = write(fd, frame, 5 + bufSize);
    if (bytes_written < 0) {
        printf("Erro ao enviar a trama de dados\n");
        return -1;
    }

    // Configurar temporizador para aguardar resposta (ACK ou REJ)
    alarmEnabled = FALSE;
    signal(SIGALRM, alarmHandler);  // Configurar função de manuseio de timeout
    alarm(globalTimeout);

    // Máquina de estados para processar a resposta do receptor
    while (currentState != STOP) {
        int bytes_read = read(fd, response, sizeof(response));

        if (bytes_read > 0) {
            switch (currentState) {
                case START:
                    if (response[0] == protocolo.FLAG) {
                        currentState = FLAG_RECEIVED;
                    }
                    break;
                case FLAG_RECEIVED:
                    if (response[1] == protocolo.A_RECEPTOR) {
                        currentState = A_RECEIVED;
                    } else {
                        currentState = START;
                    }
                    break;
                case A_RECEIVED:
                    if (response[2] == protocolo.CTRL_RR) {
                        printf("RR recebido, trama enviada corretamente.\n");
                        currentState = STOP;
                        ack_received = 1;
                    } else if (response[2] == protocolo.CTRL_REJ) {
                        printf("REJ recebido, retransmitindo trama...\n");
                        currentState = STOP;
                    } else {
                        currentState = START;
                    }
                    break;
                default:
                    currentState = START;
                    break;
            }
        } else if (alarmEnabled == FALSE) {
            printf("Timeout, retransmitindo trama...\n");
            currentState = STOP; // Para sair e retransmitir fora do loop
        }
    }

    if (!ack_received) {
        printf("Erro: Não foi recebido ACK após o envio da trama.\n");
        return -1;
    }

    // Desligar o temporizador
    alarm(0);
    return 0;

}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet)
{
    // TODO

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(int showStatistics)
{
    // TODO

    int clstat = closeSerialPort();
    return clstat;
}
