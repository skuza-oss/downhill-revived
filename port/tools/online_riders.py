"""Scripted riders in an online race (finding 206): the pad scripts the online test tools give their
games, and the switch without which those scripts do nothing.

DHD_RACE_PAD drives a rider only with DHD_RACE_SYNC=1: the script's seconds count on the race clock
from GO, which autopad.c's race_sync_vsync keeps only with that switch (without it the online
riders of mosh_round_check.py and cross_race.py took no input at all: finding 204, Surprises).
rider_env() gives both. Every race's clock restarts at its countdown, so the same script rides
each race of a room.

A menu script (DHD_NAV, DHD_AUTOPAD, DHD_AUTOPAD_FRAMES) replaces the keyboard: host_pad_read takes
the script's buttons and never reads the keyboard, so a game driven by posted keys
(hidden_game.Game.key) must have none; hidden_game refuses the mix. DHD_RACE_PAD alone is no menu
script, but it only acts from a menu script's countdown on (autopad.c host_pad_script)."""

PEDAL = '0:400:cross'
BRAKE = '0:400:triangle'


def attacks(start, every, count, hold=0.2):
    """A punch (Square) and a kick (Circle) in turn, every `every` seconds of the race from `start`."""
    return ','.join(f'{start + k * every:.1f}:{hold}:{"square" if k % 2 == 0 else "circle"}' for k in range(count))


def pad(*parts):
    """One DHD_RACE_PAD script from several (empty parts left out)."""
    return ','.join(p for p in parts if p)


def rider_env(script):
    """The environment that makes a game's rider follow script in an online (or offline) race."""
    return dict(DHD_RACE_SYNC='1', DHD_RACE_PAD=script)
