#ifndef _MATH_H
#define _MATH_H

#include <stdint.h>

/* Minimal math.h for Doom port */

#define abs(x) ((x) < 0 ? -(x) : (x))
#define fabs(x) ((x) < 0 ? -(x) : (x))

int sin_fp(int x);
int cos_fp(int x);
int fixmul(int a, int b);

#endif
