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

typedef enum {
    START,
    FLAG_RCV,
    A_RCV,
    C_RCV,
    //BCC_OK,
    DATA_READ,
    END
} state;
#define FALSE 0
#define TRUE 1
#define BUF_SIZE 11


volatile int STOP = FALSE;


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


    newtio.c_cc[VMIN] = 11;  // Blocking read until 5 chars received





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
    unsigned char buf[BUF_SIZE] = {0}; // +1: Save space for the final '\0' char

    int bytes_read;
    unsigned char data[BUF_SIZE] = {0};
    int i = 0;
    unsigned char BCC2_read;
    unsigned char xor_read;


    state currentstate = START;
    while (STOP==FALSE)
    {
        read(fd, buf, 1);
        printf("var = 0x%02X\n", (unsigned int)(buf[0] & 0xFF));

        switch(currentstate){
            case START:
                if(buf[0]==0x7E){
                currentstate = FLAG_RCV;
                }
                break;
            case FLAG_RCV:
                if(buf[0]==0x03){
                    currentstate = A_RCV;
                }
                break;
            case A_RCV:
                if(buf[0]==0x03){
                    currentstate = C_RCV;
                }
            break;

            case C_RCV:
                if(buf[0]==(0x03 ^ 0x03)){
                currentstate = DATA_READ;
                printf("to bcc ok\n");
                }
            break;
            
            /*
            case DATA_READ:

                if(buf[0] == 0x7E) {

                    //i--;
                    BCC2_read = data[i];
                    xor_read = data[0];

                    for(int j = 0; j<i-1; j++){
                        printf("valor da data: 0x%02X\n", data[j]);
                        xor_read = xor_read ^ data[j];

                    }
                    printf("BCC2 lido: 0x%02X\n", BCC2_read);
                    printf("BCC2 calculado: 0x%02X\n", xor_read);
                    currentstate = END;
                    STOP = TRUE;
                    printf("end\n");


                } else {
                    data[i] = buf[0];
                    i++;
                }

            break;
            */

            case DATA_READ:

                if (buf[0] == 0x7E) {

                // Verifica se há pelo menos 1 byte de dados + 1 byte de BCC2
                if (i < 1) {
                    printf("Frame vazio ou inválido\n");
                    currentstate = START;
                    i = 0;
                break;
                }

                // Último byte recebido antes do FLAG é o BCC2
                BCC2_read = data[i - 1];

                // Calcular XOR dos dados (exclui o BCC2)
                xor_read = 0x00;
                for (int j = 0; j < i - 1; j++) {
                    printf("valor da data[%d]: 0x%02X\n", j, data[j]);
                    xor_read ^= data[j];
                }

                printf("BCC2 lido: 0x%02X\n", BCC2_read);
                printf("BCC2 calculado: 0x%02X\n", xor_read);

                // Verificação do BCC2
                if (xor_read == BCC2_read) {
                    printf("BCC2 OK\n");
                    currentstate = END;
                } else {
                    printf("BCC2 ERROR\n");
                    currentstate = START;  // ou estado de erro, dependendo do teu protocolo
                }

                STOP = TRUE;
                i = 0; // reset buffer para próximo frame
                printf("end\n");

                } else {

                    // Guardar byte recebido
                    data[i] = buf[0];
                    i++;

                    // Proteção contra overflow (IMPORTANTE)
                    /*if (i >= MAX_DATA_SIZE) {
                        printf("Overflow de buffer\n");
                        currentstate = START;
                        i = 0;
                    }
                    */
                }

            break;

            /*
            case BCC_OK:
                if(buf[0]==0x7E){
                    currentstate = END;
                    STOP=TRUE;
                    printf("end\n");
                }
            break;
            */

            case END:
                break;
            }
        }
    unsigned char A = 0x01;
    unsigned char C = 0x07;
    unsigned char F = 0x7E;
    unsigned char BCC = A^C;
    unsigned char UA[5] = {F, A, C, BCC, F};
    int teste = write(fd, UA, 5);
    for(int i=0; i<teste; i++)
    {

        printf("UA = 0x%02X\n", UA[i]);

    }
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
