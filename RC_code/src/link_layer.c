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
int alarmEnabled = 0;
int alarmCount = 0;

// Manipulador do alarme
void alarmHandler(int signal) {
    alarmEnabled = 0;
    alarmCount++;
    printf("Alarme #%d\n", alarmCount);
}
////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters){

    struct termios oldtio, newtio;
    unsigned char buffer[BUFFER_SIZE];

    // Abrir o serial port
    fd = open(connectionParameters.serialPort, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror("Erro ao abrir a porta serial");
        return -1;
    }

    // Guardar a configuração anterior e configurar a porta
    if (tcgetattr(fd, &oldtio) == -1) {
        perror("Erro ao obter a configuração da porta");
        return -1;
    }
    
    memset(&newtio, 0, sizeof(newtio));
    newtio.c_cflag = connectionParameters.baudRate | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1;
    newtio.c_cc[VMIN] = 0;

    tcflush(fd, TCIOFLUSH);
    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("Erro ao configurar a porta");
        return -1;
    }

    // Configuração do alarme
    (void) signal(SIGALRM, alarmHandler);

    if (connectionParameters.role == TRANSMITTER) {
        // Construir e enviar a trama SET
        unsigned char setFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_SET, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET, protocolo.FLAG};
        
        while (alarmCount < connectionParameters.nRetransmissions) {
            // Enviar a trama SET
            write(fd, setFrame, sizeof(setFrame));
            printf("A enviar a trama SET\n");

            // Ativar o alarme
            alarmEnabled = 1;
            alarm(connectionParameters.timeout);

            // Esperar pela resposta UA
            while (alarmEnabled) {
                int res = read(fd, buffer, BUFFER_SIZE); // Ler para o buffer de 256 bytes
                if (res >= 5 && buffer[0] == protocolo.FLAG && buffer[1] == protocolo.A_RECEPTOR && 
                    buffer[2] == protocolo.CTRL_UA && buffer[3] == (protocolo.A_RECEPTOR ^ protocolo.CTRL_UA) && buffer[4] == protocolo.FLAG) {
                    printf("Trama UA recebida\n");
                    alarm(0);
                    tcsetattr(fd, TCSANOW, &oldtio);
                    return fd;
                }
            }

            printf("Não foi recebida a trama UA, a retransmitir...\n");
        }

        // Se o número máximo de tentativas for atingido
        printf("Erro: não foi recebida a trama UA após várias tentativas\n");
        tcsetattr(fd, TCSANOW, &oldtio);
        return -1;

    } else if (connectionParameters.role == RECEIVER) {
        // Esperar pela trama SET
        while (1) {
            int res = read(fd, buffer, BUFFER_SIZE); // Ler para o buffer de 256 bytes
            if (res >= 5 && buffer[0] == protocolo.FLAG && buffer[1] == protocolo.A_TRANSMISSOR && buffer[2] == protocolo.CTRL_SET && buffer[3] == (protocolo.A_TRANSMISSOR ^ protocolo.CTRL_SET) && buffer[4] == protocolo.FLAG) {
                printf("Trama SET recebida\n");

                // Construir e enviar a trama UA
                unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, protocolo.CTRL_UA, protocolo.A_RECEPTOR ^ protocolo.CTRL_UA, protocolo.FLAG};
                write(fd, uaFrame, sizeof(uaFrame));
                printf("A enviar a trama UA\n");

                tcsetattr(fd, TCSANOW, &oldtio);
                return fd;
            }
        }
    }

    // Restaurar a configuração original se algo falhar
    tcsetattr(fd, TCSANOW, &oldtio);
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
    // TODO

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
