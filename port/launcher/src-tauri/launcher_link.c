/* C ABI for the desktop bridge; policy stays shared. GPL-3.0-or-later. */
#include "../../host/launcher_link.h"
const char *dhd_launcher_link(const char *url) { return launcher_link(url); }
