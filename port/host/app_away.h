/* A phone's app away from the screen, as the host and the core see it (finding 227): present.c's
   app_away_wait sets it (host.c's host_app_away), the freeze watch reads it, and the return leaves
   one "came back" the core takes once after that present (host_service_host_came_back, kernel.c),
   which rebases the game clock (fps.c's port_clock_after_away). Plain C11 atomics:
   port/host/test_app_away.c drives the whole return path through it. GPL-3.0-or-later. */
#ifndef PORT_HOST_APP_AWAY_H
#define PORT_HOST_APP_AWAY_H

void app_away_set(int away);     /* 1 the app left, 0 it came back */
int app_away_now(void);          /* the app is away */
int app_away_came_back(void);    /* 1 once after each return, then 0 */

#endif
