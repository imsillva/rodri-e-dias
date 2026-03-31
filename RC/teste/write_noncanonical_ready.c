// Write to serial port in non-canonical mode
//
// Modified by: Eduardo Nuno Almeida [enalmeida@fe.up.pt]

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400
#define _POSIX_SOURCE 1 // POSIX compliant source

#define FALSE 0
#define TRUE 1

#define BUF_SIZE 5
#define BUF_SIZE2 1000

int fd;
// teste
struct termios oldtio;
struct termios newtio;

int alarmEnabled = FALSE;
int alarmCount = 0;

void alarmHandler(int signal) {
  // Can be used to change a flag that increases the number of alarms
  alarmEnabled = FALSE;
  alarmCount++;
  printf("Alarm #%d received\n", alarmCount);
}

int llopen(const char *port) {
  // 1. Abrir a porta série
  fd = open(port, O_RDWR | O_NOCTTY);
  if (fd < 0) {
    perror(port);
    return -1;
  }

  // Guardar configurações atuais
  if (tcgetattr(fd, &oldtio) == -1) {
    perror("tcgetattr");
    return -1;
  }

  // Limpar e configurar nova estrutura
  memset(&newtio, 0, sizeof(newtio));
  newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
  newtio.c_iflag = IGNPAR;
  newtio.c_oflag = 0;
  newtio.c_lflag = 0;

  // TIMEOUTS: Fundamental para o alarme e a máquina de estados não encravarem
  newtio.c_cc[VTIME] = 1; // 0.1 segundos de timeout
  newtio.c_cc[VMIN] = 0;  // Non-blocking read

  tcflush(fd, TCIOFLUSH);

  // Aplicar configurações
  if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
    perror("tcsetattr");
    return -1;
  }
  printf("Porta série configurada.\n");

  unsigned char buf[5];
  buf[0] = 0x7E;
  buf[1] = 0x03;
  buf[2] = 0x03;        // C da trama SET
  buf[3] = 0x03 ^ 0x03; // BCC1
  buf[4] = 0x7E;

  typedef enum { START, FLAG_RCV, A_RCV, C_RCV, BCC_OK, END } state;
  state currentstate = START;

  int STOP = FALSE;
  alarmCount = 0;
  alarmEnabled = FALSE;

  while (alarmCount < 3 && STOP == FALSE) {
    // Se o alarme não está ativo, (re)transmite a trama
    if (alarmEnabled == FALSE) {
      write(fd, buf, 5);
      printf("Trama SET enviada (Tentativa %d). À espera de UA...\n",
             alarmCount + 1);
      alarmEnabled = TRUE;
      alarm(3);             // Ativa alarme para 3 segundos
      currentstate = START; // Reinicia a máquina para a nova leitura
    }

    unsigned char byte_lido;
    // Lê um byte. Como VTIME=1, se não houver nada, ele não bloqueia para
    // sempre.
    if (read(fd, &byte_lido, 1) > 0) {
      switch (currentstate) {
      case START:
        if (byte_lido == 0x7E)
          currentstate = FLAG_RCV;
        break;
      case FLAG_RCV:
        if (byte_lido == 0x03)
          currentstate = A_RCV;
        else if (byte_lido != 0x7E)
          currentstate = START;
        break;
      case A_RCV:
        if (byte_lido == 0x07)
          currentstate = C_RCV; // Esperamos o UA (0x07)!
        else if (byte_lido == 0x7E)
          currentstate = FLAG_RCV;
        else
          currentstate = START;
        break;
      case C_RCV:
        if (byte_lido == (0x03 ^ 0x07))
          currentstate = BCC_OK; // Verifica o BCC do UA
        else if (byte_lido == 0x7E)
          currentstate = FLAG_RCV;
        else
          currentstate = START;
        break;
      case BCC_OK:
        if (byte_lido == 0x7E) {
          currentstate = END;
          STOP = TRUE;
          alarm(0); // DESLIGA O ALARME!
          printf("Trama UA recebida com sucesso. Ligação estabelecida!\n");
        } else {
          currentstate = START;
        }
        break;
      case END:
        break;
      }
    }
  }

  // Verifica se saímos do ciclo por sucesso ou por exceder retransmissões
  if (STOP == FALSE) {
    printf("Falha: Limite máximo de retransmissões excedido.\n");
    return -1;
  }

  return 1; // Sucesso
}

int llclose() {
  // Wait until all bytes have been written to the serial port
  sleep(1);

  // Restore the old port settings
  if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
    perror("tcsetattr");
    exit(-1);
  }

  close(fd);
  return 0;
}
volatile int STOP = FALSE;

int main(int argc, char *argv[]) {

  // Set alarm function handler.
  // Install the function signal to be automatically invoked when the timer
  // expires, invoking in its turn the user function alarmHandler
  struct sigaction act = {0};
  act.sa_handler = &alarmHandler;
  if (sigaction(SIGALRM, &act, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }
  // Program usage: Uses either COM1 or COM2
  const char *serialPortName = argv[1];

  if (argc < 2) {
    printf("Incorrect program usage\n"
           "Usage: %s <SerialPort>\n"
           "Example: %s /dev/ttyS1\n",
           argv[0], argv[0]);
    exit(1);
  }

  // Open serial port device for reading and writing, and not as controlling tty
  // because we don't want to get killed if linenoise sends CTRL-C.
  llopen(serialPortName);

  // Create string to send
  unsigned char buf[BUF_SIZE] = {0};

  // In non-canonical mode, '\n' does not end the writing.
  // Test this condition by placing a '\n' in the middle of the buffer.
  // The whole buffer must be sent even with the '\n'.

  int bytes = write(fd, buf, BUF_SIZE);
  printf("%d bytes written\n", bytes);

  // Enable alarm in t seconds
  int t = 3;
  alarm(t);

  buf[bytes] = '\0';

  typedef enum { START, FLAG_RCV, A_RCV, C_RCV, BCC_OK, END } state;

  state currentstate = START;

  unsigned char buf2[BUF_SIZE2] = {0};

  llclose();

  return 0;
}
