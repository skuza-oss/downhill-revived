"""Voice chat (finding 131): port/host/voice.c with the real libopus (port/tools/opus.py) and SDL3
(port/tools/sdl3.py) -- a tone encoded, received as voice frames, reordered, concealed, decoded and
mixed; gain, pan and mute; eight talkers; malformed, oversized and flooding frames; and the
proximity rules (port/hle/voice_rules.h). No microphone, network or game: host_service_voice_*
and voice_mix are driven directly, as online.c and the output's device thread do. Windows only
(libopus is built for win-x64)."""
import testprog
import opus
import sdl3


def main():
    host = testprog.ROOT / 'port/host'
    opus_lib, opus_include = opus.build()
    sdl_lib, sdl_include = sdl3.build()
    exe = testprog.host('voice', [host / 'voice.c', host / 'test_voice.c'],
                        includes=[host, opus_include, sdl_include, testprog.ROOT / 'port/include'],
                        defines=['DHD_VOICE=1'], libs=[opus_lib, sdl_lib] + sdl3.SYSTEM_LIBS)
    testprog.run(exe)


if __name__ == '__main__':
    main()
