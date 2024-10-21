// Link layer header.
// NOTE: This file must not be changed.

#ifndef _LINK_LAYER_H_
#define _LINK_LAYER_H_


// SIZE of maximum acceptable payload.
// Maximum number of bytes that application layer should send to link layer
#define MAX_PAYLOAD_SIZE 1000

// MISC
#define FALSE 0
#define TRUE 1
#define BUFFER_SIZE 256

// Definir os estados possíveis usando macros
#define START 0
#define FLAG_RECEIVED 1
#define A_RECEIVED 2
#define C_RECEIVED 3
#define BCC_VALID 4
#define STOP 5



typedef enum
{
    LlTx,
    LlRx,
} LinkLayerRole;

typedef struct
{
    char serialPort[50];
    LinkLayerRole role;
    int baudRate;
    int nRetransmissions;
    int timeout;
} LinkLayer;

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


// Declarar manipulador do alarme
void alarmHandler(int signal);

// Open a connection using the "port" parameters defined in struct linkLayer.
// Return "1" on success or "-1" on error.
int llopen(LinkLayer connectionParameters);

// Send data in buf with size bufSize.
// Return number of chars written, or "-1" on error.
int llwrite(const unsigned char *buf, int bufSize);

// Receive data in packet.
// Return number of chars read, or "-1" on error.
int llread(unsigned char *packet);

// Close previously opened connection.
// if showStatistics == TRUE, link layer should print statistics in the console on close.
// Return "1" on success or "-1" on error.
int llclose(int showStatistics);

#endif // _LINK_LAYER_H_
