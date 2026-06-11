#include "buttons.h"

#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    uint64_t mask;
} ButtonMap;

static const ButtonMap kButtons[] = {
    { "A",       BTN_A },
    { "B",       BTN_B },
    { "X",       BTN_X },
    { "Y",       BTN_Y },
    { "L",       BTN_L },
    { "R",       BTN_R },
    { "ZL",      BTN_ZL },
    { "ZR",      BTN_ZR },
    { "PLUS",    BTN_PLUS },
    { "START",   BTN_PLUS },
    { "MINUS",   BTN_MINUS },
    { "SELECT",  BTN_MINUS },
    { "UP",      BTN_UP },
    { "DOWN",    BTN_DOWN },
    { "LEFT",    BTN_LEFT },
    { "RIGHT",   BTN_RIGHT },
    { "DUP",     BTN_UP },
    { "DDOWN",   BTN_DOWN },
    { "DLEFT",   BTN_LEFT },
    { "DRIGHT",  BTN_RIGHT },
    { "LSTICK",  BTN_LSTICK },
    { "RSTICK",  BTN_RSTICK },
    { "HOME",    BTN_HOME },
    { "CAPTURE", BTN_CAPTURE },
};

bool button_from_name(const char *name, uint64_t *out_mask) {
    for (size_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); i++) {
        if (strcasecmp(name, kButtons[i].name) == 0) {
            *out_mask = kButtons[i].mask;
            return true;
        }
    }
    return false;
}

const char *button_names_list(void) {
    return "A,B,X,Y,L,R,ZL,ZR,PLUS,MINUS,UP,DOWN,LEFT,RIGHT,LSTICK,RSTICK,HOME,CAPTURE";
}
