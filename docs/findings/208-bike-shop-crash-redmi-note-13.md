# 208: the Bike Shop closes the game on a Redmi Note 13 4G (issue #78)

Status: in progress

Branch `fix/bike-shop-android` from `main` ad0135bd, started 2026-11-12.

## Goal and acceptance

Issue #78: on a Redmi Note 13 4G (Xiaomi 23129RA5FL, Snapdragon 685, Adreno 610, Android 15) the
game closes every time the player enters the Bike Shop, on 0.7.14 and 0.7.16. On the Mi 11i (same
2400x1080 screen, 120 Hz) the Bike Shop works. There is no such phone here.

Done when: the crash's place is named (our function, what it reads, the guest address) with the
most likely cause and how sure that is; a fix if the evidence supports one, else diagnostics
that pin it from the next shared log, and the exact question for the reporter. Offline behaviour
unchanged; `check.py port` and `check.py docs` green; the Android debug APK compiles.

Out of scope: the Options > Posters crash (207), touch skipping of the flyover (209), the online
rider code.

## Progress

- [x] 2026-11-12: finding reserved.
