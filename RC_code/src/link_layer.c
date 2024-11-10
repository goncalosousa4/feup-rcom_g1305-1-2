#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "serial_port.h"
#include "link_layer.h"

#define C_RR0 0xAA   // RR0: el receptor está listo para recibir la trama de información número 0
#define C_RR1 0xAB   // RR1: el receptor está listo para recibir la trama de información número 1
#define C_REJ0 0x54  // REJ0: el receptor rechaza la trama de información número 0 (se detectó un error)
#define C_REJ1 0x55  // REJ1: el receptor rechaza la trama de información número 1 (se detectó un error)
int tramaRx = 0;
// Enums para caracteres de controle e comandos de comunicação
typedef enum {
    FLAG = 0x7E,        //Usado para indicar o início e fim de uma trama
    ESCAPE = 0x7D       //Para aplicar byte stuffing
} ControlCharacters;

// Endereços utilizados no protocolo
typedef enum {
    Address_Transmitter = 0x03,
    Address_Receiver = 0x01
} Address;

// Comandos de controlo para estabelecer e encerrar a comunicação
typedef enum {
    Command_SET = 0x03,
    Command_UA = 0x07,
    Command_DISC = 0x0B,    //Encerra a comunicação
    Command_DATA = 0x01,    //Indica envio de dados
    Command_RR = 0x05,      //Reconhece recebimento correto de uma trama
    Command_REJ = 0x01      //Rejeita uma trama incorreta
} ControlCommands;

unsigned char frame[MAX_FRAME_SIZE];    // Array para armazenar uma trama temporária

extern int fd;
LinkLayerRole currentRole;  //Transmissor ou receptor
LinkLayer param;
// Estrutura para armazenar as estatísticas de conexão
typedef struct {
     int tramasEnviadas;
    int tramasRecebidas;
    int tramasRejeitadas;
    int tramasAceitas;
    int totalBytesTransmitidos;
    double tiempoTransmision; 
    double tiempoRecepcion;   
    double tiempoDesconexion; 
    double tiempoTransferencia;
} EstatisticasConexao;

// Instância global para as estatísticas
EstatisticasConexao estatisticas = {0, 0, 0, 0, 0.0, 0.0, 0.0};

// Estados utilizados na máquina de estados para o protocolo de ligação
typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    BCC1_OK,
    DATA,
    STOP_R
} LinkLayerState;

int timeout = 3;                
int retransmissions = 5;         
int alarmEnabled = 0;            
int alarmCount = 0;
clock_t desconexionStart;

clock_t conexionStart;

// Função de tratamento da interrupção de alarme
void alarmHandler(int signal) {
    alarmEnabled = 1;
    alarmCount++;
}

// Função para enviar uma trama de supervisão (controlo)
/*int enviarTramaSupervisao1(unsigned char address, unsigned char control) {
    unsigned char frame[5] = {0}; // Criação da trama de controlo
    frame[0] = FLAG;
    frame[1] = address;
    frame[2] = control;
    frame[3] = address ^ control;
    frame[4] = FLAG;
    int bytes = writeBytesSerialPort(frame, 5);
    if(bytes < 0){
         printf("Erro  ao enviar trama de supervisão\n");
        return -1;
    }
    printf("DEBUG (enviarTramaSupervisao):%d bytes written\n", bytes);
    printf("0x%02X 0x%02X 0x%02X 0x%02X 0x%02X\n", frame[0], frame[1], frame[2], frame[3], frame[4]);
    estatisticas.tramasEnviadas++;  // Atualiza a contagem de tramas enviadas na estatística
    // Wait until all bytes have been written to the serial port
    sleep(1);
    return 0;
}*/

void enviarTramaSupervisao(int fd, unsigned char address, unsigned char control) {
    unsigned char frame[5] = {FLAG, address, control, address ^ control, FLAG}; // Criação da trama de controlo
    writeBytesSerialPort(frame, 5);
    printf("DEBUG (enviarTramaSupervisao): A enviar frame de controlo: 0x%X\n", control);
    estatisticas.tramasEnviadas++;  // Atualiza a contagem de tramas enviadas na estatística
}

// Atualiza as estatísticas com base no resultado de uma trama enviada
void actualizarEstadisticasEnvio(int aceito) {
    if (aceito) {
        estatisticas.tramasAceitas++;       // Incrementa tramas aceitas se a trama foi recebida corretamente
    } else {
        estatisticas.tramasRejeitadas++;    // Incrementa tramas rejeitadas se ocorreu um erro na receção
    }
}

// Incrementa a contagem de tramas recebidas
void actualizarEstadisticasRecepcao() {
    estatisticas.tramasRecebidas++;
}

// Exibe as estatísticas da conexão
void mostrarEstatisticas() {
    printf("=== Estatísticas da Conexão ===\n");
    printf("Tramas Enviadas: %d\n", estatisticas.tramasEnviadas);
    printf("Tramas Recebidas: %d\n", estatisticas.tramasRecebidas);
    printf("Tramas Rejeitadas: %d\n", estatisticas.tramasRejeitadas);
    printf("Tramas Aceitas: %d\n", estatisticas.tramasAceitas);
     printf("Total de bytes transmitidos: %d bytes\n", estatisticas.totalBytesTransmitidos);
    printf("Tempo total de transmissão: %.2f ms\n", estatisticas.tiempoTransmision);
    printf("Tempo total de receção: %.2f ms\n", estatisticas.tiempoRecepcion);
    printf("Tempo total de desconexão: %.2f ms\n", estatisticas.tiempoDesconexion);
    printf("===============================\n");
}

// Função para calcular o CRC (verificação de redundância cíclica) de um buffer de dados
/*unsigned char calculateCRC(const unsigned char *buf, int bufSize) {
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
}*/

// Función para calcular el BCC2 como un XOR simple de los datos
unsigned char calculateBCC2(const unsigned char *buf, int bufSize) {
    unsigned char bcc2 = 0;
    for (int i = 0; i < bufSize; i++) {
        bcc2 ^= buf[i];
    }
    return bcc2;
}

// Simula un error en los datos con una probabilidad dada
/*int introduceError(float probability) {
    return ((float)rand() / RAND_MAX) < probability;
}*/

////////////////////////////////////////////////
// LLOPEN - Abre a conexão serial
////////////////////////////////////////////////
// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: o descritor da porta serial se bem-sucedido, -1 caso contrário
int llopen(LinkLayer connectionParameters) {
    conexionStart = clock();            // Inicia o temporizador global da conexão
    fd = openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate);
    currentRole = connectionParameters.role;

    // Verifica se a porta serial foi aberta corretamente
    if (fd < 0) {
        fprintf(stderr, "Error al abrir la conexión serial\n");
        return -1;
    }

    timeout = connectionParameters.timeout;     // Define o tempo limite para retransmissão
    retransmissions = connectionParameters.nRetransmissions;    // Define o número de retransmissões permitidas
    int attempt_count = 0;
    clock_t start = clock();    // Inicia o temporizador para monitorar o tempo de conexão
    printf("DEBUG (llopen): Iniciando conexión en modo %s\n", connectionParameters.role == LlTx ? "Transmisor" : "Receptor");
    LinkLayerState state = START;
    (void)signal(SIGALRM, alarmHandler);

    switch (connectionParameters.role) {
        
        // Caso o rol seja de Transmissor
        case LlTx:{
            while (retransmissions > 0) {
                unsigned char supFrame[5] = {FLAG, Address_Transmitter, Command_SET, Address_Transmitter ^ Command_SET, FLAG};
                write(fd, supFrame, 5);
                alarm(timeout);
                alarmEnabled = 0;
                while (state != STOP_R  && !alarmEnabled) {
                
                    unsigned char byte = 0;
                    int bytes;

                    if((bytes = readByteSerialPort(&byte)) < 0){
                        printf("DEBUG (llopen Tx): Error receiving UA\n");
                        return -1;
                    }
                    
                    if(bytes){
            
                        switch (state) {
                            case START: 
                                if (byte == FLAG) state = FLAG_RCV; 
                                break;
                            case FLAG_RCV:  
                                if (byte == Address_Receiver) state = A_RCV;
                                else if (byte == FLAG) state = FLAG_RCV;  
                                else state = START;  // Reseta caso o byte não seja esperado
                                break;
                            case A_RCV:
                                if (byte == Command_UA) state = C_RCV;
                                else if (byte == FLAG) state = FLAG_RCV;  
                                else state = START;  // Reseta caso o byte não seja esperado
                                break;
                            case C_RCV:
                                if (byte == (Address_Receiver ^ Command_UA)) state = BCC1_OK;
                                else if (byte == FLAG) state = FLAG_RCV;  
                                else state = START;  
                                break;
                            case BCC1_OK:
                                if (byte == FLAG) state = STOP_R;
                                else state = START;  
                                break;
                            default:
                                state = START;
                                break;
                            
                        }
                    }
                }
                // Se a conexão foi estabelecida (estado STOP_R alcançado)
                if (state == STOP_R) {
                    estatisticas.tiempoTransmision += (double)(clock() - start) / CLOCKS_PER_SEC;
                    printf("DEBUG (llopen Tx): Conexión establecida correctamente.\n");
                    return fd;
                }

                // Se o alarme foi acionado (timeout)
                if (alarmEnabled) {
                    desconexionStart = clock();
                    estatisticas.tiempoDesconexion += (double)(clock() - desconexionStart) * 1000.0 / CLOCKS_PER_SEC;
                }
                retransmissions--;
                printf("DEBUG (llopen Tx): Reintento restante = %d\n", retransmissions);
            }
            // Caso não seja possível estabelecer a conexão após todas as tentativas
            printf("DEBUG (llopen Tx): Error, no se pudo establecer la conexión.\n");
            return -1;
        }

        // Caso o rol seja de Receptor
        case LlRx:{
            
            while(state != STOP_R) {
                unsigned char byte;
                int bytes;
                if((bytes = readByteSerialPort(&byte)) < 0){
                    attempt_count++;
                    printf("DEBUG (llopen Rx): Error receiving UA\n");
                    return -1;
                }
                if(bytes > 0) {
                     // Lê bytes da porta serial e processa-os de acordo com o estado atual
                    switch (state) {
                        case START:
                            if (byte == FLAG) state = FLAG_RCV;
                            else state = START;
                            break;
                        case FLAG_RCV:
                            if (byte == Address_Transmitter) state = A_RCV;
                            else if (byte == FLAG) state = FLAG_RCV;  
                            else state = START;  
                            break;
                        case A_RCV:
                            if (byte == Command_SET) state = C_RCV;
                            else if (byte == FLAG) state = FLAG_RCV;  
                            else state = START;  
                            break;
                        case C_RCV:
                            if (byte == (Address_Transmitter ^ Command_SET)) state = BCC1_OK; 
                            else state = START;  
                            break;
                        case BCC1_OK:
                            if (byte == FLAG) state = STOP_R;
                            else state = START;  
                            break;
                        default:
                            state = START;
                            break;
                    }
                }
            }
            unsigned char uaFrame[5] = {FLAG, Address_Receiver, Command_UA, Address_Receiver ^ Command_UA, FLAG};
            write(fd, uaFrame, 5);

            // Registra o tempo de recepção
            estatisticas.tiempoRecepcion += (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
            printf("DEBUG (llopen Rx): Conexión establecida y UA enviado.\n");
            return fd;
        }
        // Retorna erro se o rol não for válido
        default: 
            break;
    }
    return -1;
}

// Realiza o byte stuffing nos dados de entrada
int applyByteStuffing(const unsigned char *input, int length, unsigned char *output) {
    int stuffedIndex = 0;       // Índice para o array de saída
    
    // Percorre cada byte do array de entrada
    for (int i = 0; i < length; i++) {
        printf("DEBUG (applyByteStuffing): Byte original = 0x%X\n", input[i]);
        
        // Verifica se o byte atual é um FLAG
        if (input[i] == FLAG) {
            output[stuffedIndex++] = ESCAPE;
            output[stuffedIndex++] = 0x5E;
            printf("DEBUG (applyByteStuffing): FLAG detectado, aplicando stuffing -> ESCAPE + 0x5E\n");
        } 
        // Verifica se o byte atual é um ESCAPE
        else if (input[i] == ESCAPE) {
            output[stuffedIndex++] = ESCAPE;
            output[stuffedIndex++] = 0x5D;
            printf("DEBUG (applyByteStuffing): ESCAPE detectado, aplicando stuffing -> ESCAPE + 0x5D\n");
        } 
        // Caso não seja FLAG nem ESCAPE, copia o byte para o array de saída sem alteração
        else {
            output[stuffedIndex++] = input[i];
            printf("DEBUG (applyByteStuffing): Byte sem alteração = 0x%X\n", input[i]);
        }
    }
    printf("DEBUG (applyByteStuffing): Tamanho final após stuffing = %d\n", stuffedIndex);
    // Retorna o tamanho do array de saída após stuffing
    return stuffedIndex;
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
    if (estatisticas.tiempoTransferencia == 0) {
        estatisticas.tiempoTransferencia = (double)clock();
    }

    unsigned char frame[MAX_FRAME_SIZE];
    int frameIndex = 0;
    
    frame[frameIndex++] = FLAG;
    frame[frameIndex++] = Address_Transmitter;
    frame[frameIndex++] = Command_DATA;
    frame[frameIndex++] = Address_Transmitter ^ Command_DATA;

    unsigned char BCC2 = calculateBCC2(buf, bufSize);
    frameIndex += applyByteStuffing(buf, bufSize, &frame[frameIndex]);
    frameIndex += applyByteStuffing(&BCC2, 1, &frame[frameIndex]);
    frame[frameIndex++] = FLAG;

    int tentativas = retransmissions;
    while (tentativas > 0) {
        // Enviar la trama completa
        writeBytesSerialPort(frame, frameIndex);
        alarmEnabled = 0;
        alarm(timeout);
        estatisticas.tramasEnviadas++;

        unsigned char byte;
        LinkLayerState state = START;
        while (!alarmEnabled && state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == Address_Receiver) state = A_RCV;
                        break;
                    case A_RCV:
                        if (byte == C_RR0 || byte == C_RR1) {
                            state = STOP_R;  // Confirmación de recepción correcta
                        } else if (byte == C_REJ0 || byte == C_REJ1) {
                            // Reiniciar la transmisión al recibir REJ
                            state = START;
                            printf("DEBUG (llwrite): REJ received, resending frame...\n");
                            break; // Salir del bucle interno para reenviar la trama completa
                        }
                        break;
                    default:
                        state = START;
                        break;
                }
            }
        }

        // Si se recibió RR, confirmar y avanzar
        if (state == STOP_R && (byte == C_RR0 || byte == C_RR1)) {
            alarm(0);
            actualizarEstadisticasEnvio(1);
            estatisticas.totalBytesTransmitidos += bufSize;

            return frameIndex;  // Confirmación exitosa, avanza al siguiente paquete
        }

        // Si se recibió REJ, reducir el contador de intentos y reiniciar
        tentativas--;
        desconexionStart = clock();
        estatisticas.tiempoDesconexion += (double)(clock() - desconexionStart) * 1000.0 / CLOCKS_PER_SEC;
    }

    // Si todos los intentos fallan, retorno con error
    actualizarEstadisticasEnvio(0);
    printf("DEBUG (llwrite): Error, no se pudo enviar la trama correctamente.\n");
    return -1;
}

//Esta função remove bytes de escape de uma trama recebida, restaurando os bytes originais
int applyByteDestuffing(const unsigned char *input, int length, unsigned char *output) {

    int destuffedIndex = 0;     // Índice para o array de saída
    int escape = 0;             // Indicador que verifica se o byte ESCAPE foi encontrado

    for (int i = 0; i < length; i++) {

        // Se o byte ESCAPE já foi encontrado, verifica o próximo byte
        if (escape) {
            if (input[i] == 0x5E) {
                output[destuffedIndex++] = FLAG;
                printf("DEBUG (applyByteDestuffing): ESCAPE seguido de 0x5E, convertendo para FLAG\n");
            } 
            else if (input[i] == 0x5D) {
                output[destuffedIndex++] = ESCAPE;
                printf("DEBUG (applyByteDestuffing): ESCAPE seguido de 0x5D, convertendo para ESCAPE\n");
            }
            escape = 0;     // Reinicia o indicador de ESCAPE
        } 
        else if (input[i] == ESCAPE) {
            escape = 1;     // Marca que o byte ESCAPE foi encontrado e aguarda o próximo byte
            printf("DEBUG (applyByteDestuffing): Byte ESCAPE detectado, aguardando próximo byte\n");
        } 
         // Deteta o FLAG final e para o processamento
        else if (input[i] == FLAG && i == length - 1) { 
            printf("DEBUG (applyByteDestuffing): FLAG final detectado, parando processamento\n");
            break;
        } 
        // Copia o byte sem alteração se não houver destuffing
        else {
            output[destuffedIndex++] = input[i];
            printf("DEBUG (applyByteDestuffing): Byte sem alteração = 0x%X\n", input[i]);
        }
    }
    printf("DEBUG (applyByteDestuffing): Tamanho final após destuffing = %d\n", destuffedIndex);
    // Retorna o tamanho final do array de saída após o destuffing
    return destuffedIndex;
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
    LinkLayerState state = START;
    unsigned char frame[MAX_FRAME_SIZE];
    int frameIndex = 0;
    unsigned char byte;
    int tentativas = retransmissions;
    
    // Loop principal de tentativas de leitura
    while (tentativas > 0) {
        alarmEnabled = 0;
        alarm(timeout);
        printf("DEBUG (llread): Inicio do loop de leitura, tentativas restantes = %d\n", tentativas);
        
        // Loop para ler bytes enquanto o alarme não dispara e o estado final não é alcançado
        while (!alarmEnabled && state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                printf("DEBUG (llread): Estado = %d, Byte recebido = 0x%X\n", state, byte);
                switch (state) {
                    case START:
                        if (byte == FLAG) {
                            state = FLAG_RCV;
                            printf("DEBUG (llread): Transição para FLAG_RCV\n");
                        }
                        break;
                    case FLAG_RCV:
                        if (byte == Address_Transmitter) {
                            state = A_RCV;
                            printf("DEBUG (llread): Transição para A_RCV\n");
                        }
                        break;
                    case A_RCV:
                        if (byte == Command_DATA) {
                            state = C_RCV;
                            printf("DEBUG (llread): Transição para C_RCV (Command_DATA)\n");
                        } else if (byte == Command_DISC) {
                            printf("DEBUG (llread): Command_DISC recebido, desconectando...\n");
                            return -2;
                        }
                        break;
                    case C_RCV:
                        if (byte == (Address_Transmitter ^ Command_DATA)) {
                            state = BCC1_OK;
                            printf("DEBUG (llread): BCC1 OK, transição para DATA\n");
                        }
                        break;
                    case BCC1_OK:
                        if (byte != FLAG) {
                            frame[frameIndex++] = byte;
                            state = DATA;
                            printf("DEBUG (llread): Transição para DATA, dado recebido = 0x%X\n", byte);
                        }
                        break;
                    case DATA:
                        if (byte == FLAG) {
                            state = STOP_R;
                            printf("DEBUG (llread): FLAG de fim recebido, transição para STOP_R\n");
                        } else {
                            frame[frameIndex++] = byte;
                            printf("DEBUG (llread): Dado adicionado ao frame = 0x%X\n", byte);
                        }
                        break;
                    default:
                        state = START;
                        printf("DEBUG (llread): Estado desconhecido, reiniciando para START\n");
                        break;
                }
            }
        }

        // Processa a trama recebida se o estado final for alcançado
        if (state == STOP_R) {
            int destuffedSize = applyByteDestuffing(frame, frameIndex, packet);
            unsigned char BCC2 = calculateBCC2(packet, destuffedSize - 1);
            printf("DEBUG (llread): Tamanho após destuffing = %d, BCC2 calculado = 0x%X\n", destuffedSize, BCC2);
            
            // Verifica o BCC2 para garantir a integridade dos dados
            if (BCC2 == packet[destuffedSize - 1]) {
                printf("DEBUG (llread): Trama recebida corretamente. A enviar RR...\n");
                if (tramaRx == 0) {
                    enviarTramaSupervisao(fd, Address_Receiver, C_RR0);
                } else {
                    enviarTramaSupervisao(fd, Address_Receiver, C_RR1);
                }
                tramaRx = (tramaRx + 1) % 2;
                actualizarEstadisticasRecepcao();
                estatisticas.tramasRecebidas++;
                return destuffedSize - 1;
            } else {
                // Envia REJ se o BCC2 for incorreto
                printf("DEBUG (llread): Erro: BCC2 incorreto. A enviar REJ...\n");
                if (tramaRx == 0) {
                    enviarTramaSupervisao(fd, Address_Receiver, C_REJ0);
                } else {
                    enviarTramaSupervisao(fd, Address_Receiver, C_REJ1);
                }
                tentativas--;
                state = START;
                frameIndex = 0;
                printf("DEBUG (llread): Reinicio após REJ, tentativas restantes = %d\n", tentativas);
            }
        } else if (alarmEnabled) {
            // Control del tiempo de espera, registra y reinicia
            printf("DEBUG (llread): Tiempo de espera agotado, reintentando...\n");
            desconexionStart = clock();
            estatisticas.tiempoDesconexion += (double)(clock() - desconexionStart) * 1000.0 / CLOCKS_PER_SEC;
            tentativas--;
            state = START;  // Reinicia el estado en caso de timeout
            frameIndex = 0; // Reinicia el índice del frame
            printf("DEBUG (llread): Reinicio após timeout, tentativas restantes = %d\n", tentativas);
        }
    }

    printf("DEBUG (llread): Error, no se pudo recibir la trama correctamente.\n");
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
    printf("DEBUG: Iniciando función llclose.\n");
    LinkLayerState state = START;
    clock_t start = clock();
    // Se o rol atual é de Transmissor (LlTx)
    if (currentRole == LlTx) {
         // Envia a trama DISC para iniciar a desconexão
        enviarTramaSupervisao(fd, Address_Transmitter, Command_DISC);
         // Loop para tentar receber o DISC do receptor e confirmar o encerramento
        while (state != STOP_R && retransmissions > 0) {
            alarmEnabled = 0;
            alarm(timeout);

            unsigned char byte;
            while (!alarmEnabled && state != STOP_R) {
                if (readByteSerialPort(&byte) > 0) {
                    // Máquina de estados para verificar e processar o DISC do receptor
                    switch (state) {
                        case START:
                            if (byte == FLAG) state = FLAG_RCV;
                            break;
                        case FLAG_RCV:
                            if (byte == Address_Receiver) state = A_RCV;
                            break;
                        case A_RCV:
                            if (byte == Command_DISC) state = C_RCV;
                            break;
                        case C_RCV:
                            if (byte == (Address_Receiver ^ Command_DISC)) state = BCC1_OK;
                            break;
                        case BCC1_OK:
                            if (byte == FLAG) state = STOP_R;
                            break;
                        default:
                            break;
                    }
                }
            }
            
        }
        // Envia a trama de confirmação UA após receber DISC do receptor
        enviarTramaSupervisao(fd, Address_Transmitter, Command_UA);
        estatisticas.tiempoTransmision += (double)(clock() - start) / CLOCKS_PER_SEC;
    } 
    // Caso o rol seja Receptor (LlRx)
    else if (currentRole == LlRx) {
        while (state != STOP_R) {
            // Loop para tentar receber o DISC do transmissor
            unsigned char byte;
            if (readByteSerialPort(&byte) > 0) {
                // Máquina de estados para processar o DISC do transmissor
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == Address_Transmitter) state = A_RCV;
                        break;
                    case A_RCV:
                        if (byte == Command_DISC) state = C_RCV;
                        break;
                    case C_RCV:
                        if (byte == (Address_Transmitter ^ Command_DISC)) state = BCC1_OK;
                        break;
                    case BCC1_OK:
                        if (byte == FLAG) state = STOP_R;
                        break;
                    default:
                        break;
                }
            }
        }
        // Envia o DISC ao transmissor para confirmar a desconexão
        enviarTramaSupervisao(fd, Address_Receiver, Command_DISC);
        estatisticas.tiempoRecepcion += (double)(clock() - start) / CLOCKS_PER_SEC;
    }
    estatisticas.tiempoTransferencia = ((double)clock() - estatisticas.tiempoTransferencia) / CLOCKS_PER_SEC;

    
    
    // Calcula a eficiência e exibe estatísticas, se solicitado
    
    int C = 9600; // Capacidade do enlace em bits por segundo
    int R = estatisticas.totalBytesTransmitidos * 8; // Total de bits transmitidos
    double eficiencia = ((double)R / (estatisticas.tiempoTransferencia * C)) * 100;

    // Exibe as estatísticas se showStatistics estiver ativo
    if (showStatistics) {
        mostrarEstatisticas();
        double tiempoTotalConexion = (double)(clock() - conexionStart) * 1000.0 / CLOCKS_PER_SEC;
        printf("BaudRate: %d\n", C);
        printf("Tempo total da conexão: %.2f ms\n", tiempoTotalConexion);
        printf("Tempo total de transferência: %.2f segundos\n", estatisticas.tiempoTransferencia);
        printf("Total de bits transmitidos (R): %d bits\n", R);
        printf("Eficiência do protocolo (S): %.2f%%\n", eficiencia);
    }
     // Fecha a porta serial e retorna sucesso
    closeSerialPort();
    return 0;

}