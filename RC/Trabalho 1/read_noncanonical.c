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

typedef enum {
    START,
    FLAG_READ,
    A_READ,
    C_READ,
    BCC_READ,
    STOP_ME,
    DATA_READ,
} statenames;

void initME();
statenames currentState;

void initME(){
    
    currentState = START;

}

volatile int STOP = FALSE;

int main(int argc, char *argv[])
{
    initME();

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
    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
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
    newtio.c_cc[VTIME] = 0; // Inter-character timer unused
    newtio.c_cc[VMIN] = 10;  // Blocking read until 5 chars received

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

    // Loop for input
    //unsigned char buf[BUF_SIZE + 1] = {0}; // +1: Save space for the final '\0' char

    //while (STOP == FALSE)
    //{
        // Returns after 5 chars have been input
        // int bytes = read(fd, buf, BUF_SIZE);
        //buf[bytes] = '\0'; // Set end of string to '\0', so we can printf

        //printf(":%s:%d\n", buf, bytes);
        /*for(int i=0; i<bytes; i++)
        {
            printf("SET = 0x%02X\n", buf[i]);
        }
        */
        //if (buf[0] == 'z')
            //STOP = TRUE;
    //}
    unsigned char buf = 0;
    int bytes_read;
    unsigned char data[BUF_SIZE] = {0};
    int i = 0;
    unsigned char BCC2_read;
    unsigned char xor_read = 0;

    while (currentState != STOP_ME)
    {
        switch (currentState)
        {
            
            case START:

                bytes_read = read(fd, &buf, 1);
                if (bytes_read == 1 && buf == 0x7E){
                    currentState = FLAG_READ;
                    printf("FLAG = 0x%02X\n", buf);
                } else if (bytes_read != 1 || buf != 0x7E){
                    currentState = START;
                }

            break;

            case FLAG_READ:

                bytes_read = read(fd, &buf, 1);
                if (bytes_read == 1 && buf == 0x03){
                    currentState = A_READ;
                    printf("A = 0x%02X\n", buf);
                } else if (bytes_read == 1 && buf == 0x7E){
                    currentState = FLAG_READ;
                } else if (bytes_read != 1 || buf != 0x03){
                    currentState = START;
                }

            break;

            case A_READ:

                bytes_read = read(fd, &buf, 1);
                if (bytes_read == 1 && buf == 0x03){
                    currentState = C_READ;
                    printf("C = 0x%02X\n", buf);
                } else if (bytes_read == 1 && buf == 0x7E){
                    currentState = FLAG_READ;
                } else if (bytes_read != 1 || buf != 0x03){
                    currentState = START;
                }
            
            break;

            case C_READ:

                bytes_read = read(fd, &buf, 1);
                if(bytes_read == 1 && buf == (0x03)^(0x03)){
                    currentState = BCC_READ;
                    printf("BCC = 0x%02X\n", buf);
                } else if (bytes_read == 1 && buf == 0x7E){
                    currentState = FLAG_READ;
                } else if (bytes_read != 1 || buf != (0x03)^(0x03)){
                    currentState = START;
                }

            break;

            case BCC_READ:

                bytes_read = read(fd, &buf, 1);
                if (bytes_read == 1 && buf == 0x7E){
                    currentState = DATA_READ;
                    printf("FLAG = 0x%02X\n", buf);
                } else if (bytes_read != 1 || buf != 0x7E){
                    currentState = START;
                }

            break;

            case DATA_READ:

                bytes_read = read(fd, &buf, 1);
                if(bytes_read == 1 && buf == 0x7E) {

                    i--;
                    BCC2_read = data[i];

                    for(int j = 0; j<i-1; j++){
                        printf("valor da data: 0x%02X\n", data[j]);
                        xor_read = xor_read ^ data[j];
                    }
                    printf("BCC2 lido: 0x%02X\n", BCC2_read);
                    printf("BCC2 lido: 0x%02X\n", xor_read);
                    currentState = STOP_ME;

                } else {

                    data[i] = buf;
                    i++;

                }

            break;
        }   
    }

    
    
    
    unsigned char A = 0x01;
    unsigned char C = 0x07;
    unsigned char F = 0x7E;
    unsigned char BCC = A^C;
    unsigned char UA[BUF_SIZE] = {F, A, C, BCC, F};

    int teste = write(fd, UA, BUF_SIZE);

    /*
    for(int i=0; i<teste; i++)
    {
        printf("SET = 0x%02X\n", UA[i]);
    }
    */

    // The while() cycle should be changed in order to respect the specifications
    // of the protocol indicated in the Lab guide

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
