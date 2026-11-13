# Translations

One table per language: `i18n/<lang>.txt` (UTF-8), loaded with `fantatech <game dir> --lang <lang>`
or `FT_LANG=<lang>`. The engine looks in `i18n/` in the working directory, next to the exe, and at
the repo root.

```
# comment
@font: C:\Windows\Fonts\arial.ttf     optional, font for translated text (default MS Gothic)
@name: English                          optional, the name the settings screen lists
a21003: The die is cast.               dialogue: the script's $id
36b4171d: Start from the beginning     menus etc.: FNV-1a hash of the Japanese, 8 hex digits
```

- Empty text = not translated yet; the original line image is shown.
- `jp.txt` has no entries at all: it's the original Japanese, there so the settings screen lists
  it like any other language.
- `\n` forces a line break; long lines wrap and are centred under the picture.
- `%cmnd` menu entries ("look: 下駄箱") translate the verb and the label as separate keys.
- The `## engine` section is fantatech's own UI (the save panel); its Japanese stays in the
  template even with `--bare`, since it's ours.

Generate or refresh a table from your own copy of the game:

```
ftconv i18n-template mana01 -o i18n\en.txt --from i18n\en.txt
```

`--from` keeps the translations already in the file. The template puts the Japanese line above
each key as a comment; add `--bare` to leave it out (only `script:line` stays) when a table is
going into the repo, since the game's script text is not ours to publish.
