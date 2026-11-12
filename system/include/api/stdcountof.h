/*
 * include/api/stdcountof.h
 * C23 countof macro for NexsOS1.
 */

#ifndef _STDCOUNTOF_H
#define _STDCOUNTOF_H

#ifndef countof
#define countof(a) (sizeof(a) / sizeof(*(a)))
#endif

#endif /* _STDCOUNTOF_H */
