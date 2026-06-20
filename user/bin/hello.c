#include <os1.h>
#include <stdint.h>

// standard stringhe

uint8_t ubyte1;  // 0..255
int8_t byte1;    // -128..127
uint16_t ubyte2; // 0..65535
uint32_t ubyte4; // 0..(2^32 - 1)
int64_t byte8;
size_t vol; // grande quanto un puntatore (64 bit qui)

// standard os1

typedef int32_t pid_t;   // identificatore di processo
typedef int64_t ssize_t; // dimensione con segno (letture/scritture)
typedef int64_t off_t;   // offset in un file
typedef int64_t time_t;  // tempo in secondi
typedef uint32_t mode_t; // permessi file
typedef uint32_t uid_t, gid_t;

// dichiarazioni funzioni programma

int eta = 25;          // un intero
float altezza = 1.78;  // un numero con la virgola (32 bit)
double pigreco = 3.14; // un numero con la virgola (64 bit)
char iniziale = 'M';   // un singolo carattere

int signum(int n);
int plus(int n);
int tocounter(int n1, int n2);

// programm

int signum(int n) {
  if (n < 0)
    return -1;
  if (n > 0)
    return 1;
  return 0;
}

int plus(int n) { return n++; }

int tocounter(int n1, int n2) {
  int x = n1;
  int y = n2;

  while (x < y) {
    int r = signum(x);
    if (r < 0) {
      print("negativo\n");
    } else if (r == 0) {
      print("zero\n");
    } else {
      print("positivo\n");
    }

    x++;
    return 0;
  }
  return 0;
}

// main

int main(void) { return 0; }