// user/include/termios.h  (create this file)
#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <stdint.h>

typedef unsigned long tcflag_t;
typedef unsigned char cc_t;
typedef unsigned long speed_t;

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[20]; // NCCS is usually 20 or so
  speed_t ibaud;
  speed_t obaud;
};

// Minimal stubs for the functions kilo uses
#define ICANON 0x00000002
#define ECHO 0x00000008
#define ISIG 0x00000001
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

#endif