/* Voice chat in the browser (finding 131): port/host/voice.c is built into the network module
   (port/web/build_net.py, DHD_VOICE_WEB) with libopus for wasm32 (port/tools/opus.py
   wasm_objects). voice.c's hearing side runs as it does natively, its packets straight from the
   connection through net_voice_hook (client.c); its sending side takes the page's microphone 20 ms
   at a time (voice_web_capture). What voice.c asks of the native host comes from the worker here:
   the settings (the worker's live environment, so PC SETTINGS applies at once), the push-to-talk
   key (the page's keys, not while a text field has them) and the log. */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

__attribute__((import_module("env"), import_name("web_getenv"))) int web_getenv(const char *name, char *out, int cap);
__attribute__((import_module("env"), import_name("web_key_held"))) int web_key_held(unsigned key);

/* host_api.h's host_getenv: the worker's value now. A few calls' answers stay valid at once
   (voice.c reads its settings one after another). */
const char *host_getenv(const char *name)
{
    static char values[8][160];
    static unsigned next;
    char *out = values[next++ % 8];
    return web_getenv(name, out, (int)sizeof values[0]) ? out : NULL;
}

void hle_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);   /* net.js prints the module's stderr in the page's log */
    va_end(ap);
}

/* The page's keys (worker.js host_service_key_held: a text field open holds them all). */
int keyboard_focused(void) { return 1; }
int keyboard_is_down(unsigned typed) { return web_key_held(typed); }
int text_input_active(void) { return 0; }
