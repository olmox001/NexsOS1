#ifndef _TERMIOS_H
#define _TERMIOS_H

/*
 * Minimal POSIX <termios.h> for the OS1 userspace libc.
 *
 * OS1 has no line discipline: a window already delivers input character by
 * character through read()/input_poll_event (see the kilo port). tcgetattr /
 * tcsetattr (lib.c) are therefore safe no-ops that succeed, so raw-mode code
 * ported from POSIX (base-nexs, linenoise-style editors) compiles and runs
 * unchanged. Implemented in the libc layer, NOT as an OS1 syscall.
 */

typedef unsigned long tcflag_t;
typedef unsigned char cc_t;
typedef unsigned long speed_t;

#define NCCS 20

struct termios {
  tcflag_t c_iflag;
  tcflag_t c_oflag;
  tcflag_t c_cflag;
  tcflag_t c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
  speed_t c_ispeed;
  speed_t c_ospeed;
};

/* c_lflag bits */
#define ISIG   0x00000001
#define ICANON 0x00000002
#define ECHO   0x00000008
#define IEXTEN 0x00008000

/* c_cc indices */
#define VMIN  6
#define VTIME 5

/* tcsetattr optional_actions */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);

#endif /* _TERMIOS_H */
