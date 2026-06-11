#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Button bitmask values matching libnx HidNpadButton_* / HiddbgNpadButton_*.
// Defined here (host-compilable) so button parsing can be unit-tested off
// device; input.c static_asserts these against the libnx headers.
#define BTN_A       (1ULL << 0)
#define BTN_B       (1ULL << 1)
#define BTN_X       (1ULL << 2)
#define BTN_Y       (1ULL << 3)
#define BTN_LSTICK  (1ULL << 4)
#define BTN_RSTICK  (1ULL << 5)
#define BTN_L       (1ULL << 6)
#define BTN_R       (1ULL << 7)
#define BTN_ZL      (1ULL << 8)
#define BTN_ZR      (1ULL << 9)
#define BTN_PLUS    (1ULL << 10)
#define BTN_MINUS   (1ULL << 11)
#define BTN_LEFT    (1ULL << 12)
#define BTN_UP      (1ULL << 13)
#define BTN_RIGHT   (1ULL << 14)
#define BTN_DOWN    (1ULL << 15)
#define BTN_HOME    (1ULL << 18)
#define BTN_CAPTURE (1ULL << 19)

// Case-insensitive button name -> mask. Returns false for unknown names.
bool button_from_name(const char *name, uint64_t *out_mask);

// Comma/space-free list helper used for documentation in error messages.
const char *button_names_list(void);
