#pragma once

#include <stdbool.h>

// Power State Control (PSC) integration. Sysmodules holding sockets MUST
// participate in sleep/wake transitions: bsdsockets aborts (crashing the
// whole sleep transition; see omm psc error 2165-1001) if a client still
// has live sockets / in-flight IPC when it suspends. We register as a PSC
// PM module with a dependency on the sockets backend so we are told to
// quiesce first, and resumed after it on wake.

typedef enum {
    PowerEvent_None,  // no pending transition
    PowerEvent_Sleep, // close all sockets, then call power_ack()
    PowerEvent_Wake,  // call power_ack(), then resume normal operation
} PowerEvent;

// Registers the PSC PM module. Must be called while an sm session is open
// (i.e. from __appInit for the sysmodule). Returns false when PSC is
// unavailable (dev .nro build); power_poll() then always returns None.
bool power_init(void);

// Non-blocking check for a pending power transition. The caller must
// acknowledge every Sleep/Wake event via power_ack() (after quiescing
// sockets, in the Sleep case).
PowerEvent power_poll(void);

// Acknowledges the most recent request returned by power_poll().
void power_ack(void);

void power_exit(void);

// --- Agent-requested power actions ---------------------------------------------

typedef enum {
    PowerAction_None = 0,
    PowerAction_Sleep,    // sleep mode; wake requires physical interaction
    PowerAction_Restart,  // reboot; server returns after boot (boot2)
    PowerAction_PowerOff, // full shutdown; power button required
} PowerAction;

// Initializes the spsm session used to execute power actions. Must be called
// while an sm session is open. Returns false when unavailable (dev .nro).
bool power_spsm_init(void);
void power_spsm_exit(void);

// Schedules an action to be executed by the server loop AFTER the current
// HTTP response has been sent and the connection closed (so the client gets
// its confirmation before the console goes away).
void power_schedule(PowerAction action);
PowerAction power_take_scheduled(void);

// True when power actions can be executed (spsm session is up).
bool power_actions_available(void);

// Executes the action. Returns false on failure.
bool power_perform(PowerAction action);
