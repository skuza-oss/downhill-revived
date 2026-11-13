# Translating the launcher

The launcher (the window before the game, on PC, Android and iOS) takes every word it shows from
this folder. `en.json` is the source: one line per string, `"key": "English words"`. A language
is one more file of the same shape, named by its code: `pt-BR.json`, `es.json`, `fr.json`,
`de.json`, `it.json`, `ja.json`, `zh-Hans.json`.

## Add or update a language

1. Copy `en.json` to `<code>.json` (or open the existing file) and translate the words on the
   right. Never change a key (the left side).
2. Add the code to `LANGUAGES` in `../i18n.js`, in the order the Language row should list them.
3. Translate `language.name` as the language's own name ("Deutsch", "日本語").
4. Check it: `python tools/lang_check.py --list` lists the strings still missing (the launcher
   shows English for those) and fails on a key that `en.json` doesn't have, a placeholder renamed or
   a bold span lost. `python tools/check.py docs` runs the same check.

## Rules

- **Plain and short.** The launcher talks like a friend who knows the game: short sentences, no
  marketing, no exclamation marks.
- **Placeholders stay as they are**: `{version}`, `{count}`, `{size}` and the like are filled in
  by the launcher. Move them wherever your grammar needs them, but keep the name between the braces.
- **Bold spans stay bold**: `**{version}**` and `**your own copy**` are drawn in bold; keep the
  `**` around the matching words.
- **Counts**: a key ending in `.one` and `.other` (`server.races.one`) is one string per count
  form. Add the forms your language uses (`.zero`, `.two`, `.few`, `.many`; Unicode CLDR's plural
  rules), and leave out the ones it doesn't.
- **The game's own words stay English.** The game's menus are English only (their font has only
  Latin letters), so the names players see there are not translated: OPTIONS, REVIVED SETTINGS,
  SETTINGS, ONLINE, SINGLE EVENT, TOUCH CONTROLS, EXIT GAME, PLAY in capitals, and the PS2's button
  names (Cross, Circle, L1). The choices in the settings rows (`4:3`, `Wide`, `Borderless`, `60 fps`)
  come from the game's own table and stay English as well.
- **Names stay**: Downhill Revived, Downhill Domination, PlayStation 2, PCSX2, Discord, Ko-fi, GitHub,
  dhd.ini, SCUS-97177.

## How the launcher picks a language

The Language row (Settings > Game, shown once there is more than one language) saves the choice in
dhd.ini (`language = de`). Without one, the launcher takes the first of the system's languages that
has a file here (an exact code, then a code it starts with, then the same language: `pt-PT` takes
`pt-BR`), else English.
