/* Voice chat (finding 131, port/host/voice.c): the host's microphone, encoder, decoders and mix.
   The game's side (who hears whom, how loud, where) is port/hle/online.c's. */
#ifndef DHD_VOICE_H
#define DHD_VOICE_H

#include <stdint.h>

/* A voice frame is an EVENT frame the server relays to the race as it is, its sender's slot
   stamped in by the server (online/server/src/room.ts relay): [VOICE_EVENT_VERSION, VOICE_EVENT,
   sequence low, sequence high, Opus packet]. online.c's EVENT_VOICE is the same number. */
enum { VOICE_EVENT_VERSION = 2, VOICE_EVENT = 11, VOICE_HEADER = 4, VOICE_PACKET_MAX = 400 };

void voice_open(void);                       /* once, as the output starts (sound_out_sdl.c) */
void voice_mix(int16_t *stereo, int frames); /* 48 kHz stereo; the device thread */
/* a voice frame from a race slot (host_service_voice_frame's work, on a host pointer; tests) */
void voice_receive(int slot, const uint8_t *in, int n);
void voice_reload_settings(void);            /* DHD_VOICE_* read again (the voice thread does it each second) */

#endif
