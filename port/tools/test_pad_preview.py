"""host_pad_vibrate's preview gate (port/hle/pad_preview.h, finding 124): the motor values the game
sends every field must not cancel PC SETTINGS' buzz, switching Vibration off must stop it at once,
and after the preview's window the game's own values apply again. Pure logic: no game, no SDL.
    python port/tools/test_pad_preview.py"""
import testprog

exe = testprog.host('pad-preview', [testprog.ROOT / 'port/hle/test_pad_preview.c'])
testprog.run(exe)
