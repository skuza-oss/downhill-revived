/* The browser's network module (port/web/build_net.py): what www/net.js needs besides net.c's
   host_service_net_* exports, which take this module's own addresses (GUEST is the identity on
   wasm32). net.js copies a NetLobby or NetLobbyInfo from here into the game's memory as is: both
   modules are wasm32, so the layouts agree. */
#include "../../net/net.h"

int web_net_lobby_size(void) { return (int)sizeof(NetLobby); }
int web_net_info_size(void) { return (int)sizeof(NetLobbyInfo); }
