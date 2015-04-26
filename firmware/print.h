#ifndef print_h__
#define print_h__

#include <avr/pgmspace.h>
#include "usb_debug_only.h"
#include <stdio.h>

// this macro allows you to write print("some text") and
// the string is automatically placed into flash memory :)
//#define print(s) print_P(PSTR(s))
#define print(s, ...) printf_P(PSTR(s), ##__VA_ARGS__)
#define pchar(c) usb_debug_putchar(c)

void print_P(const char *s);
void phex(unsigned char c);
void phex16(unsigned int i);

// Setup stdout to use usb_debug_only
void usb_stdout_init(void);

#endif
