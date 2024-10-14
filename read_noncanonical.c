// Read from serial port in non-canonical mode
//
// Modified by: Eduardo Nuno Almeida [enalmeida@fe.up.pt]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400
#define _POSIX_SOURCE 1 // POSIX compliant source

#define FALSE 0
#define TRUE 1

#define BUF_SIZE 5

#define FLAG 0x7E

//Commands sent by the Transmitter and Replies sent by the Receiver
#define A_SENDER 0x03
//Commands sent by the Receiver and Replies sent by the Transmitter
#define A_RECEIVER 0x01

#define CTRL_SET 0x03
#define CTRL_UA 0x07

enum state{
    START,
    FLAG_RECEIVED,
    A_RECEIVED,
    C_RECEIVED,
    BCC_VALID,
    STOP
};

//volatile int STOP = FALSE;
int fd;

int write_packet_command(unsigned char fieldA, unsigned char fieldC){
    // Create string to send
    unsigned char buf[BUF_SIZE] = {0};
    buf[0] = FLAG;
    buf[1] = fieldA;
    buf[2] = fieldC;
    buf[3] = fieldA ^ fieldC;
    buf[4] = FLAG;
    int bytes = write(fd, buf, BUF_SIZE);
    if(bytes < 0){
        printf("Error writing packet command\n");
        return -1;
    }
    printf("%d bytes written\n", bytes);
    printf("0x%02X 0x%02X 0x%02X 0x%02X\n", buf[0], buf[1], buf[2], buf[4]);
    // Wait until all bytes have been written to the serial port
    sleep(1);
    return 0;
}

int establish_ua(){
    enum state establish_state = START;
    
    while(establish_state != STOP){
        //Now we will check if we received any byte
        unsigned char byte = 0;
        int bytes;
        if((bytes = read(fd, &byte, sizeof(byte))) < 0){
            printf("Error receiving UA, attempt \n");
            return -1;
        }
        //If we received a byte, let's update the state machine
        if(bytes > 0){
            switch(establish_state){
                case START:{
                    printf("START");
                    if(byte == FLAG) establish_state = FLAG_RECEIVED;
                    break;
                }
                case FLAG_RECEIVED:{
                    printf("FLAG_RECEIVED");
                    if(byte == A_SENDER) establish_state = A_RECEIVED;
                    else if(byte == FLAG) establish_state = FLAG_RECEIVED;
                    else establish_state = START;
                    break;
                }
                case A_RECEIVED:{
                    printf("A_RECEIVED");
                    if(byte == CTRL_SET) establish_state = C_RECEIVED;
                    else if(byte == FLAG) establish_state = FLAG_RECEIVED;
                    else establish_state = START;
                    break;
                }
                case C_RECEIVED:{
                    printf("C_RECEIVED");
                    if(byte == (A_SENDER ^ CTRL_SET)) establish_state = BCC_VALID;
                    else if(byte == FLAG) establish_state = FLAG_RECEIVED;
                    else establish_state = START;
                    break;
                }
                case BCC_VALID:{
                    printf("BCC_VALID");
                    if(byte == FLAG) establish_state = STOP;
                    else establish_state = START;
                    break;
                }
                default:{
                    printf("default");
                    establish_state = START;
                }
            }
        }
        
    } 

    if(write_packet_command(A_SENDER, CTRL_UA) != 0){
        printf("Error writing UA command\n");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    // Program usage: Uses either COM1 or COM2
    const char *serialPortName = argv[1];

    if (argc < 2)
    {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n",
               argv[0],
               argv[0]);
        exit(1);
    }

    // Open serial port device for reading and writing and not as controlling tty
    // because we don't want to get killed if linenoise sends CTRL-C.
    fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0)
    {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio;
    struct termios newtio;

    // Save current port settings
    if (tcgetattr(fd, &oldtio) == -1)
    {
        perror("tcgetattr");
        exit(-1);
    }

    // Clear struct for new port settings
    memset(&newtio, 0, sizeof(newtio));

    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    // Set input mode (non-canonical, no echo,...)
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 1; // Inter-character timer unused
    newtio.c_cc[VMIN] = 0;  // Blocking read until 5 chars received

    // VTIME e VMIN should be changed in order to protect with a
    // timeout the reception of the following character(s)

    // Now clean the line and activate the settings for the port
    // tcflush() discards data written to the object referred to
    // by fd but not transmitted, or data received but not read,
    // depending on the value of queue_selector:
    //   TCIFLUSH - flushes data received but not read.
    tcflush(fd, TCIOFLUSH);

    // Set new port settings
    if (tcsetattr(fd, TCSANOW, &newtio) == -1)
    {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    establish_ua();
    sleep(1);
    // Restore the old port settings
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
    {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);

    return 0;
}


