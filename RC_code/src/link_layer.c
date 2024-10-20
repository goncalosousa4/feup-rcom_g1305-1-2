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

// Função para configurar a porta serial
// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: 0 se a configuração foi bem-sucedida, -1 caso contrário
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
// LLOPEN - Abre a conexão serial
////////////////////////////////////////////////

// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: o descritor da porta serial se bem-sucedido, -1 caso contrário
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
        // Estado inicial da máquina de estados
        int currentState = START;
        int res;

        // Retransmissão com base no número de tentativas
        for (int i = 0; i < connectionParameters.nRetransmissions; i++) {
            // Reiniciar o estado e enviar a trama SET
            alarmEnabled = FALSE;
            write(fd, setFrame, sizeof(setFrame));
            printf("Trama SET enviada (Tentativa %d)\n", i + 1);


            // Ativar o alarme para esperar pela resposta UA
            alarmEnabled = TRUE;
            alarm(connectionParameters.timeout);
            globalTimeout = connectionParameters.timeout;

            // Processar a resposta UA usando a máquina de estados
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
                currentState = START; 
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

}


   

////////////////////////////////////////////////
// LLWRITE - Envia uma trama de dados
////////////////////////////////////////////////
// Parâmetros:
//   buf: ponteiro para o buffer que contém os dados a serem enviados
//   bufSize: tamanho do buffer de dados
// Retorna:
//   0 se a trama for enviada com sucesso e confirmada, -1 em caso de erro
int llwrite(const unsigned char *buf, int bufSize)
{
    unsigned char frame[BUFFER_SIZE];
    unsigned char stuffedFrame[BUFFER_SIZE];
    unsigned char response[BUFFER_SIZE];
    int ack_received = 0;
    int currentState = START;
    int stuffedIndex = 4; // Iniciar após os campos FLAG, A, C e BCC1
    int retries = 0;
    

    // Construir a trama de dados
    frame[0] = protocolo.FLAG;  // Início da trama
    frame[1] = protocolo.A_TRANSMISSOR;  // Endereço
    frame[2] = 0x00;  // Controle (número de sequência ou controle de fluxo)
    frame[3] = frame[1] ^ frame[2];  // BCC1 (controle de erros simples)

    // Adicionar os dados à trama e aplicar byte stuffing
    for (int i = 0; i < bufSize; i++) {
        // Se o byte for igual ao FLAG ou ao caracter de escape (0x7D), aplicar stuffing
        if (buf[i] == protocolo.FLAG) {
            stuffedFrame[stuffedIndex++] = 0x7D; // Carácter de escape
            stuffedFrame[stuffedIndex++] = 0x5E; // Substituir FLAG por sequência segura
        } else if (buf[i] == 0x7D) {
            stuffedFrame[stuffedIndex++] = 0x7D; // Carácter de escape
            stuffedFrame[stuffedIndex++] = 0x5D;  // Substituir 0x7D por sequência segura
        } else {
            stuffedFrame[stuffedIndex++] = buf[i]; // Adicionar byte diretamente
        }
    }

    // Calcular o BCC2 (controle de erros para os dados) e aplicar byte stuffing se necessário 
    unsigned char BCC2 = 0x00;
    for (int i = 0; i < bufSize; i++) {
        // Calcular BCC2 com XOR de todos os bytes dos dados
        BCC2 ^= buf[i];
    }
    // Aplicar byte stuffing ao BCC2, se necessário
    if (BCC2 == protocolo.FLAG) {
        stuffedFrame[stuffedIndex++] = 0x7D;
        stuffedFrame[stuffedIndex++] = 0x5E;
    } else if (BCC2 == 0x7D) {
        stuffedFrame[stuffedIndex++] = 0x7D;
        stuffedFrame[stuffedIndex++] = 0x5D;
    } else {
        stuffedFrame[stuffedIndex++] = BCC2;
    }
    // Adicionar o FLAG final para indicar o fim da trama
    stuffedFrame[stuffedIndex++] = protocolo.FLAG;  


   // Loop de retransmissão tentar enviar a trama até atingir o limite de tentativas
    while (retries < globalTimeout) {
        // Enviar a trama de dados com byte stuffing aplicado
        int bytes_written = write(fd, stuffedFrame, stuffedIndex);
        if (bytes_written < 0) {
            printf("Erro ao enviar a trama de dados\n");
            return -1;
        }
        printf("Trama enviada (tentativa %d)\n", retries + 1);

        // Configurar temporizador para aguardar resposta (ACK ou REJ)
        alarmEnabled = FALSE;
        signal(SIGALRM, alarmHandler); 
        alarm(globalTimeout);

        // Máquina de estados para processar a resposta do receptor
        while (currentState != STOP && alarmEnabled) {
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
                        // Verificar se o byte de controle é RR (ACK) ou REJ (NACK)
                        if (response[2] == protocolo.CTRL_RR) {
                            printf("RR recebido, trama enviada corretamente.\n");
                            currentState = STOP;
                            ack_received = 1;// Indicar que o ACK foi recebido com sucesso
                        } else if (response[2] == protocolo.CTRL_REJ) {
                            printf("REJ recebido, retransmitindo trama...\n");
                            currentState = STOP; // Reiniciar para retransmissão
                        } else {
                            currentState = START; // Reiniciar se não for o byte esperado
                        }
                        break;
                    default:
                        currentState = START;
                        break;
                }
            } else if (!alarmEnabled) {
                printf("Timeout, retransmitindo trama...\n");
                break;
            }
        }
        // Se o ACK foi recebido, desativar o alarme e retornar sucesso
        if (ack_received) {
            // Desligar o temporizador e sair se o ACK foi recebido
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
int llread(unsigned char *packet)
{
    unsigned char frame[BUFFER_SIZE];
    unsigned char response[5];
    int currentState = START;
    int index = 0;
    unsigned char BCC2 = 0x00;
    int received_packet = 0;

    while (!received_packet) {
        // Ler um byte da porta serial
        int bytes_read = read(fd, &frame[index], 1);
        if (bytes_read < 0) {
            perror("Erro ao ler a trama");
            return -1; // Retorna erro se a leitura falhar
        }

        if (bytes_read > 0) {
            // Máquina de estados para processar cada byte recebido
            switch (currentState) {
                case START:
                    // Verificar se recebemos o FLAG inicial
                    if (frame[index] == protocolo.FLAG) {
                        currentState = FLAG_RECEIVED;
                    }
                    break;
                case FLAG_RECEIVED:
                    // Verificar se o byte é o endereço esperado (A_TRANSMISSOR)
                    if (frame[index] == protocolo.A_TRANSMISSOR) {
                        currentState = A_RECEIVED;
                    } else if (frame[index] == protocolo.FLAG) {
                        // Continuar no estado FLAG_RECEIVED se receber outro FLAG
                        currentState = FLAG_RECEIVED;
                    } else {
                        currentState = START;
                    }
                    break;
                case A_RECEIVED:
                // Verificar se o byte é o valor de controle para dados
                    if (frame[index] == 0x00) {  
                        currentState = C_RECEIVED;
                    } else if (frame[index] == protocolo.FLAG) {
                        currentState = FLAG_RECEIVED;
                    } else {
                        currentState = START;
                    }
                    break;
                case C_RECEIVED:
                    // Verificar se o BCC1 é válido (A_TRANSMISSOR ^ Controle)
                    if (frame[index] == (protocolo.A_TRANSMISSOR ^ 0x00)) {
                        currentState = BCC_VALID;
                    } else if (frame[index] == protocolo.FLAG) {
                        currentState = FLAG_RECEIVED;
                    } else {
                        currentState = START;
                    }
                    break;
                case BCC_VALID:
                    // Verificar se recebemos o FLAG de fechamento e calcular o BCC2
                    if (frame[index] == protocolo.FLAG) {
                        // Realizar o byte destuffing antes de verificar o BCC2
                        unsigned char destuffedFrame[BUFFER_SIZE];
                        int destuffedIndex = 0;
                        // Loop para destuffing dos bytes recebidos
                        for (int i = 4; i < index; i++) {
                            if (frame[i] == 0x7D) {
                                // Substituir sequências stuffed pelos valores originais
                                if (frame[i + 1] == 0x5E) {
                                    destuffedFrame[destuffedIndex++] = 0x7E;
                                    i++; // Ignorar o próximo byte
                                } else if (frame[i + 1] == 0x5D) {
                                    destuffedFrame[destuffedIndex++] = 0x7D;
                                    i++; // Ignorar o próximo byte
                                }
                            } else {
                                destuffedFrame[destuffedIndex++] = frame[i];
                            }
                        }

                        // Calcular o BCC2 usando o frame destuffed
                        for (int i = 0; i < destuffedIndex - 1; i++) {
                            BCC2 ^= destuffedFrame[i];
                        }

                        // Verificar se o BCC2 é válido
                        if (BCC2 == destuffedFrame[destuffedIndex - 1]) {
                            printf("Trama recebida corretamente!\n");
                            response[0] = protocolo.FLAG;
                            response[1] = protocolo.A_RECEPTOR;
                            response[2] = protocolo.CTRL_RR;
                            response[3] = response[1] ^ response[2];
                            response[4] = protocolo.FLAG;
                            write(fd, response, 5);

                            // Indicar que o pacote foi recebido corretamente
                            received_packet = 1;

                            // Copiar os dados destuffed para o packet
                            memcpy(packet, destuffedFrame, destuffedIndex - 1);
                            return destuffedIndex - 1; // Retornar o tamanho do pacote sem FLAG, A, C e BCC1
                        } else {
                            printf("Erro: BCC2 incorreto, enviando REJ.\n");
                             // Enviar REJ (NACK) se o BCC2 não for válido
                            response[0] = protocolo.FLAG;
                            response[1] = protocolo.A_RECEPTOR;
                            response[2] = protocolo.CTRL_REJ;
                            response[3] = response[1] ^ response[2];
                            response[4] = protocolo.FLAG;
                            write(fd, response, 5);
                            currentState = START;
                        }
                    } else {
                        // Continuar recebendo bytes até encontrar o FLAG de fechamento
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
int llclose(int showStatistics)
{
    // Configurar o manipulador de alarme
    signal(SIGALRM, alarmHandler);
    int retransmitions = connectionP.nRetransmissions;
    int state = START;
    unsigned char byte;

    // Se o role for de transmissor (TRANSMITTER)
    if (connectionP.role == TRANSMITTER) {
        // Transmissor tenta fechar a conexão enviando DISC
        while (retransmitions > 0 && state != STOP) {
            // Construir e enviar a trama DISC
            unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, 0x0B, protocolo.A_TRANSMISSOR ^ 0x0B, protocolo.FLAG};
            write(fd, discFrame, sizeof(discFrame));
            printf("Trama DISC enviada (Tentativa %d)\n", connectionP.nRetransmissions - retransmitions + 1);

            // Ativar o alarme
            alarmEnabled = TRUE;
            alarm(connectionP.timeout);
            // Reiniciar a máquina de estados
            state = START;

            // Processar a resposta (esperando DISC de volta)
            while (alarmEnabled && state != STOP) {
                if (read(fd, &byte, 1) > 0) {
                    switch (state) {
                        case START:
                            // Verifica se o FLAG inicial é recebido
                            if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                            break;
                        case FLAG_RECEIVED:
                             // Verifica se o byte é o endereço do receptor esperado
                            if (byte == protocolo.A_RECEPTOR) state = A_RECEIVED;
                            else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                            else state = START;
                            break;
                        case A_RECEIVED:
                            // Verifica se o byte é o comando DISC
                            if (byte == 0x0B) state = C_RECEIVED;
                            else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                            else state = START;
                            break;
                        case C_RECEIVED:
                            // Verifica se o BCC1 é válido (A_RECEPTOR ^ DISC)
                            if (byte == (protocolo.A_RECEPTOR ^ 0x0B)) state = BCC_VALID;
                            else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                            else state = START;
                            break;
                        case BCC_VALID:
                            // Verifica se o FLAG final é recebido
                            if (byte == protocolo.FLAG) state = STOP;
                            else state = START;
                            break;
                        default:
                            state = START;
                            break;
                    }
                }
            }
            retransmitions--;
        }
         // Verificar se a trama DISC foi recebida corretamente
        if (state != STOP) {
            printf("Erro: não foi possível fechar a conexão após várias tentativas\n");
            return -1;
        }

        // Enviar a trama UA para finalizar o processo
        unsigned char uaFrame[5] = {protocolo.FLAG, protocolo.A_TRANSMISSOR, protocolo.CTRL_UA, protocolo.A_TRANSMISSOR ^ protocolo.CTRL_UA, protocolo.FLAG};
        write(fd, uaFrame, sizeof(uaFrame));
        printf("Trama UA enviada para finalizar\n");

    } else if (connectionP.role == RECEIVER) {
        // Receptor espera a trama DISC do transmissor
        while (state != STOP) {
            if (read(fd, &byte, 1) > 0) {
                switch (state) {
                    case START:
                    // Verifica se o FLAG inicial é recebido
                        if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        break;
                    case FLAG_RECEIVED:
                    // Verifica se o byte é o endereço do transmissor
                        if (byte == protocolo.A_TRANSMISSOR) state = A_RECEIVED;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case A_RECEIVED:
                    // Verifica se o byte é o comando DISC
                        if (byte == 0x0B) state = C_RECEIVED;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case C_RECEIVED:
                    // Verifica se o BCC1 é válido (A_TRANSMISSOR ^ DISC)
                        if (byte == (protocolo.A_TRANSMISSOR ^ 0x0B)) state = BCC_VALID;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case BCC_VALID:
                    // Verifica se o FLAG final é recebido
                        if (byte == protocolo.FLAG) state = STOP;
                        else state = START;
                        break;
                    default:
                        state = START;
                        break;
                }
            }
        }

        // Enviar a trama DISC de volta para confirmar a desconexão
        unsigned char discFrame[5] = {protocolo.FLAG, protocolo.A_RECEPTOR, 0x0B, protocolo.A_RECEPTOR ^ 0x0B, protocolo.FLAG};
        write(fd, discFrame, sizeof(discFrame));
        printf("Trama DISC enviada para confirmar desconexão\n");

        // Esperar pela trama UA do transmissor
        state = START;
        while (state != STOP) {
            if (read(fd, &byte, 1) > 0) {
                switch (state) {
                    case START:
                    // Verifica se o FLAG inicial é recebido
                        if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        break;
                    case FLAG_RECEIVED:
                    // Verifica se o byte é o endereço do transmissor
                        if (byte == protocolo.A_TRANSMISSOR) state = A_RECEIVED;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case A_RECEIVED:
                    // Verifica se o byte é o comando UA
                        if (byte == protocolo.CTRL_UA) state = C_RECEIVED;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case C_RECEIVED:
                    // Verifica se o BCC1 é válido (A_TRANSMISSOR ^ UA)
                        if (byte == (protocolo.A_TRANSMISSOR ^ protocolo.CTRL_UA)) state = BCC_VALID;
                        else if (byte == protocolo.FLAG) state = FLAG_RECEIVED;
                        else state = START;
                        break;
                    case BCC_VALID:
                    // Verifica se o FLAG final é recebido
                        if (byte == protocolo.FLAG) state = STOP;
                        else state = START;
                        break;
                    default:
                        state = START;
                        break;
                }
            }
        }

        printf("Trama UA recebida, conexão fechada\n");
    }

    // Mostrar estatísticas se showStatistics for verdadeiro
    if (showStatistics) {
        printf("\n---ESTATÍSTICAS---\n\n Número de timeouts: %d\n", alarmCount);
    }

    // Restaurar a configuração da porta
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("Erro ao restaurar configuração da porta");
        return -1;
    }

    close(fd);
    return 0;
    
}
