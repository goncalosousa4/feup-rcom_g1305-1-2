#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "serial_port.h"
#include "link_layer.h"

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

// Simula un error en los datos con una probabilidad dada
int introduceError(float probability) {
    return ((float)rand() / RAND_MAX) < probability;
}

////////////////////////////////////////////////
// LLOPEN - Abre a conexão serial
////////////////////////////////////////////////
// Parâmetros: estrutura com os parâmetros de conexão
// Retorna: o descritor da porta serial se bem-sucedido, -1 caso contrário
int llopen(LinkLayer connectionParameters) {
    conexionStart = clock();            // Inicia o temporizador global da conexão
    LinkLayerState state = START;       // Estado inicial da máquina de estados
    fd = openSerialPort(connectionParameters.serialPort, connectionParameters.baudRate);
    currentRole = connectionParameters.role;

    // Verifica se a porta serial foi aberta corretamente
    if (fd < 0) {
        fprintf(stderr, "Error al abrir la conexión serial\n");
        return -1;
    }

    unsigned char byte;
    timeout = connectionParameters.timeout;     // Define o tempo limite para retransmissão
    retransmissions = connectionParameters.nRetransmissions;    // Define o número de retransmissões permitidas
    signal(SIGALRM, alarmHandler);

    clock_t start = clock();    // Inicia o temporizador para monitorar o tempo de conexão
    printf("DEBUG (llopen): Iniciando conexión en modo %s\n", connectionParameters.role == LlTx ? "Transmisor" : "Receptor");

    switch (connectionParameters.role) {
        // Caso o rol seja de Transmissor
        case LlTx:
            while (retransmissions > 0 && state != STOP_R) {
                // Envia o comando SET para iniciar a conexão
                enviarTramaSupervisao(fd, Address_Transmitter, Command_SET);
                printf("DEBUG (llopen Tx): SET enviado, esperando UA...\n");
                alarmEnabled = 0;  // Reinicia el estado de la alarma
                alarm(timeout);    // Configura el temporizador de la alarma

                while (!alarmEnabled && state != STOP_R) {
                    // Lê bytes da porta serial e processa-os de acordo com o estado atual
                    if (readByteSerialPort(&byte) > 0) {
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
                                break;
                        }
                    }
                }

                // Se a conexão foi estabelecida (estado STOP_R alcançado)
                if (state == STOP_R) {
                    alarm(0);   // Desativa o alarme
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
            closeSerialPort();
            return -1;

        // Caso o rol seja de Receptor
        case LlRx:
            while (state != STOP_R) {
                if (readByteSerialPort(&byte) > 0) {
                     // Lê bytes da porta serial e processa-os de acordo com o estado atual
                    switch (state) {
                         case START:
                            if (byte == FLAG) state = FLAG_RCV;
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
            // Registra o tempo de recepção
            estatisticas.tiempoRecepcion += (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;

            // Envia o comando UA para confirmar a conexão
            enviarTramaSupervisao(fd, Address_Receiver, Command_UA);
            printf("DEBUG (llopen Rx): Conexión establecida y UA enviado.\n");
            return fd;

        // Retorna erro se o rol não for válido
        default: 
            return -1;
    }
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
    printf("DEBUG: Iniciando función llwrite.\n");
    unsigned char frame[MAX_FRAME_SIZE];
    int frameIndex = 0;
    estatisticas.tiempoTransferencia = (double)clock();
    estatisticas.totalBytesTransmitidos += bufSize;
    
    // Início da trama com FLAG e endereço de transmissão
    frame[frameIndex++] = FLAG;
    frame[frameIndex++] = Address_Transmitter;

    // Define o comando de dados e o campo de verificação BCC1
    frame[frameIndex++] = Command_DATA;
    frame[frameIndex++] = Address_Transmitter ^ Command_DATA;

    
    // Calcula el CRC de los datos
    unsigned char CRC = calculateCRC(buf, bufSize);
    // Aplica byte stuffing ao conteúdo dos dados
    frameIndex += applyByteStuffing(buf, bufSize, &frame[frameIndex]);

    // Aplica byte stuffing ao BCC2 e adiciona à trama
    unsigned char stuffedCRC[2];
    int stuffedCRCLength = applyByteStuffing(&CRC, 1, stuffedCRC);
    for (int i = 0; i < stuffedCRCLength; i++) {
        frame[frameIndex++] = stuffedCRC[i];
    }
    
    // Finaliza a trama com FLAG
    frame[frameIndex++] = FLAG;

    int attempts = retransmissions;     // Número de tentativas permitidas para retransmissão
    clock_t start = clock();            // Inicia temporizador para contabilizar tempo de transmissão

    // Envia a trama e aguarda confirmação (RR)
    while (attempts > 0) {
        // Envia a trama pela porta serial
        writeBytesSerialPort(frame, frameIndex);
        printf("DEBUG (llwrite): Trama enviada. Aguardando confirmación...\n");
        alarmEnabled = 0;  // Reinicia el estado de la alarma
        alarm(timeout);    // Configura el temporizador de la alarma

        unsigned char byte;
        LinkLayerState state = START;

        // Máquina de estados para verificar a confirmação de recebimento (RR)
        while (!alarmEnabled && state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        else state = START;  
                        break;
                
                    case FLAG_RCV:
                        if (byte == Address_Receiver) state = A_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;  
                        else state = START;   
                        break;

                    case A_RCV:
                        if (byte == Command_RR) state = STOP_R;
                        else if (byte == FLAG) state = FLAG_RCV;  
                        else state = START;  
                        break;

                    default:
                        state = START;  // Garante que, em qualquer caso inesperado, retorna a START
                        break;
                }
            }
        }

        // Se a confirmação de recebimento foi recebida (estado STOP_R)
        if (state == STOP_R) {
            alarm(0);
            estatisticas.tiempoTransmision += (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
            return frameIndex;
        }

        // Caso o alarme seja ativado (tempo esgotado), registrar tempo de desconexão e tentar novamente
        printf("DEBUG (llwrite): Tiempo de espera agotado, reintentando...\n");
        desconexionStart = clock();
        estatisticas.tiempoDesconexion += (double)(clock() - desconexionStart) * 1000.0 / CLOCKS_PER_SEC;
        attempts--;
    }

    printf("DEBUG (llwrite): Error, no se pudo enviar la trama correctamente.\n");
    // Retorna erro se todas as tentativas falharem
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
    printf("DEBUG: Iniciando función llread.\n");

    LinkLayerState state = START;               // Estado inicial da máquina de estados
    unsigned char rawFrame[MAX_FRAME_SIZE];     // Buffer para armazenar a trama recebida
    int rawIndex = 0;                           // Índice para armazenar bytes na trama recebida
    unsigned char byte;                         // Verificação de erro da trama de dados
    unsigned char CRC = 0;
    int attempts = retransmissions;
    clock_t start = clock();                    // Marca o início do tempo de receção

    // Loop para tentativa de recepção com controle de retransmissão
    while (attempts > 0) {
        printf("DEBUG (llread): Aguardando trama de datos...\n");
        alarmEnabled = 0;  // Reinicia el estado de la alarma
        alarm(timeout);    // Configura el temporizador de la alarma

        // Máquina de estados para receber a trama
        while (!alarmEnabled && state != STOP_R) {
            if (readByteSerialPort(&byte) > 0) {
                printf("DEBUG (llread): Estado = %d, Byte recibido = 0x%X\n", state, byte);

                switch (state) {
                    case START:
                        if (byte == FLAG) state = FLAG_RCV;
                        break;
                    case FLAG_RCV:
                        if (byte == Address_Transmitter) state = A_RCV;
                        else if (byte != FLAG) state = START;
                        break;
                    case A_RCV:
                        if (byte == Command_DATA) state = C_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case C_RCV:
                        if (byte == (Address_Transmitter ^ Command_DATA)) state = BCC1_OK;
                        else if (byte == FLAG) state = FLAG_RCV;
                        else state = START;
                        break;
                    case BCC1_OK:
                        if (byte != FLAG) {
                            rawFrame[rawIndex++] = byte;    // Armazena bytes de dado
                            state = DATA;
                        }
                        break;
                    case DATA:
                        if (byte == FLAG) {
                            printf("DEBUG (llread): FLAG final detectado, terminando leitura de trama\n");
                            state = STOP_R;     // Alcança o estado final ao receber o FLAG final
                        } else {
                            rawFrame[rawIndex++] = byte;    // Armazena mais bytes de dados
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        // Processa a trama após o estado final ser alcançado
        if (state == STOP_R) {
            int destuffedSize = applyByteDestuffing(rawFrame, rawIndex, packet);
            
            
            // Calcula el CRC de los datos recibidos (excluyendo el último byte, que es el CRC recibido)
            CRC = calculateCRC(packet, destuffedSize - 1);

            /*if (introduceError(0.05)) {  
                packet[0] ^= 0xFF;  
            }*/

            if (CRC == packet[destuffedSize - 1]) {
                printf("DEBUG (llread): Trama recebida corretamente. A enviar RR...\n");
                enviarTramaSupervisao(fd, Address_Receiver, Command_RR);        // Envia reconhecimento se BCC2 é correto
                actualizarEstadisticasRecepcao();
                estatisticas.tiempoRecepcion += (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
                return destuffedSize - 1;        // Retorna o tamanho dos dados recebidos (sem o BCC2)
            } 
            
            else {
                printf("Erro: BCC2 incorreto. A enviar REJ...\n");
                enviarTramaSupervisao(fd, Address_Receiver, Command_REJ);       // Envia rejeição se BCC2 é incorreto
                actualizarEstadisticasEnvio(0);
                attempts--;
                state = START;      // Reinicia para o estado inicial
                rawIndex = 0;
            }
        } else {
            printf("DEBUG (llread): Tiempo de espera agotado, reintentando...\n");
            desconexionStart = clock();
            estatisticas.tiempoDesconexion += (double)(clock() - desconexionStart) * 1000.0 / CLOCKS_PER_SEC;
            attempts--;
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
    clock_t start = clock();    // Tempo de início para medir a duração da desconexão

    // Se o rol atual é de Transmissor (LlTx)
    if (currentRole == LlTx) {
        // Envia a trama DISC para iniciar a desconexão
        enviarTramaSupervisao(fd, Address_Transmitter, Command_DISC);

        // Loop para tentar receber o DISC do receptor e confirmar o encerramento
        while (state != STOP_R && retransmissions > 0) {
            alarmEnabled = 0;  // Reinicia el estado de la alarma
            alarm(timeout);    // Configura el temporizador de la alarma

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
                            else if (byte == FLAG) state = FLAG_RCV;  
                            else state = START;                       
                            break;

                        case A_RCV: 
                            if (byte == Command_DISC) state = C_RCV;
                            else if (byte == FLAG) state = FLAG_RCV;  
                            else state = START;                      
                            break;

                        case C_RCV: 
                            if (byte == (Address_Receiver ^ Command_DISC)) state = BCC1_OK;
                            else if (byte == FLAG) state = FLAG_RCV;  
                            else state = START;                      
                            break;

                        case BCC1_OK: 
                            if (byte == FLAG) state = STOP_R;         
                            break;

                        default:
                            break;
                    }
                }
            }
            retransmissions--;    // Diminui as tentativas caso não haja resposta
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
                        else if (byte == FLAG) state = FLAG_RCV;  
                        else state = START;                       
                        break;

                    case A_RCV:
                        if (byte == Command_DISC) state = C_RCV;
                        else if (byte == FLAG) state = FLAG_RCV;  
                        else state = START;                       
                        break;

                    case C_RCV: 
                        if (byte == (Address_Transmitter ^ Command_DISC)) state = BCC1_OK;
                        else if (byte == FLAG) state = FLAG_RCV;  
                        else state = START;                       
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
    int C = 9600; // Capacidad del enlace en bits por segundo
    int R = estatisticas.totalBytesTransmitidos * 8; // Total de bits transmitidos
    double eficiencia = (double)R / (estatisticas.tiempoTransferencia * C);


    // Exibe as estatísticas se showStatistics estiver ativo
     if (showStatistics) {
        mostrarEstatisticas();
        double tiempoTotalConexion = (double)(clock() - conexionStart) * 1000.0 / CLOCKS_PER_SEC;
        printf("Tempo total da conexao: %.2f ms\n", tiempoTotalConexion);
        printf("Tiempo total de transferencia: %.2f segundos\n", estatisticas.tiempoTransferencia);
        printf("Total de bits transmitidos (R): %d bits\n", R);
        printf("Eficiencia del protocolo (S): %.2f\n", eficiencia);
    }
    // Fecha a porta serial e retorna sucesso
    closeSerialPort();
    return 0;
}