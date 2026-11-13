/* A phone's app away from the screen: app_away.h. GPL-3.0-or-later. */
#include "app_away.h"

#include <stdatomic.h>

static atomic_int away, came_back;

void app_away_set(int now_away)
{
    atomic_store(&away, now_away);
    if (!now_away) atomic_store(&came_back, 1);
}

int app_away_now(void) { return atomic_load(&away); }

int app_away_came_back(void) { return atomic_exchange(&came_back, 0); }
