#ifndef _MATH_H
#define _MATH_H

#include <stdint.h>

/* Minimal math.h for Doom port */

int abs(int x);
double fabs(double x);

int sin_fp(int x);
int cos_fp(int x);
int fixmul(int a, int b);

#endif
