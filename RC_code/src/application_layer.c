// Application layer protocol implementation

#include <stdio.h>
#include <string.h>
#include "link_layer.h"
#include "application_layer.h"

void applicationLayer(const char *serialPort, const char *role, int baudrate, int nTries, int timeout, const char *filename) {
    LinkLayer connectionParameters;

    // Configurar los parámetros de conexión
    strcpy(connectionParameters.serialPort, serialPort);
    connectionParameters.baudRate = baudrate;
    connectionParameters.nRetransmissions = nTries;
    connectionParameters.timeout = timeout;
    connectionParameters.role = (strcmp(role, "tx") == 0) ? LlTx : LlRx;

    printf("Llamando a llopen desde applicationLayer...\n");

    // Llamada a llopen para establecer la conexión
    int fd = llopen(connectionParameters);
    if (fd < 0) {
        fprintf(stderr, "Error en llopen: No se pudo establecer la conexión.\n");
        return;
    }

    printf("Conexión establecida correctamente en applicationLayer.\n");

    if (connectionParameters.role == LlTx) {  // Modo transmisor
        // Trama de prueba para verificar stuffing y destuffing
       unsigned char testFrame[] = {
    0x7E, 0x45, 0x7D, 0x52,  // Secuencia inicial de prueba con FLAG y ESCAPE
    0x10, 0x3A, 0x7E, 0x55,  // Bytes de datos adicionales
    0x7D, 0x5E, 0x20, 0x40,  // FLAG con stuffing aplicado, y otros datos
    0x30, 0x7E, 0x60, 0x7D,  // Más datos con FLAG y ESCAPE
    0x5D, 0x50, 0x70, 0x7E   // Secuencia final con FLAG para verificar límite
    };
    int frameSize = sizeof(testFrame);

        printf("Enviando trama de prueba para verificar stuffing y destuffing...\n");
        if (llwrite(testFrame, frameSize) < 0) {
            fprintf(stderr, "Error en llwrite: No se pudo enviar la trama de prueba.\n");
            llclose(1);
            return;
        }
        printf("Trama de prueba enviada correctamente.\n");

    } else {  // Modo receptor
        unsigned char buffer[MAX_FRAME_SIZE];
        int bytesRead;

        printf("Esperando recibir la trama de prueba...\n");

        // Recibir y verificar la trama de prueba
        bytesRead = llread(buffer);
        if (bytesRead > 0) {
            printf("Trama de prueba recibida correctamente. Datos recibidos:\n");
            for (int i = 0; i < bytesRead; i++) {
                printf("0x%X ", buffer[i]);
            }
            printf("\n");
        } else {
            fprintf(stderr, "Error en llread: No se pudo recibir la trama de prueba.\n");
        }
    }

    // Llamada a llclose para cerrar la conexión
    printf("Llamando a llclose desde applicationLayer...\n");
    if (llclose(1) < 0) {
        fprintf(stderr, "Error en llclose: No se pudo cerrar la conexión correctamente.\n");
    } else {
        printf("Conexión cerrada correctamente en applicationLayer.\n");
    }
}