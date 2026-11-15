# Translated text

Every language's text, compiled into the executable (CMake embeds every `*.txt` here). Only text: no audio, menu
picture, video or program of the packages.

## Releases (converted, not edited by hand)

| File | Content | Source | Credit |
|---|---|---|---|
| `pt-BR.strings.txt` | Brazilian Portuguese GameStrings | TresCom, Brazilian Portuguese Official Translation | DreamWorks Interactive; GameStrings conversion by Nick3069 |
| `fr.strings.txt` | French GameStrings (è and ê put back the right way round) | TresCom, French Official Translation | DreamWorks Interactive; Nick3069 |
| `de.strings.txt` | German GameStrings | TresCom, German Official Translation | DreamWorks Interactive; Nick3069 |
| `es.strings.txt` | Spanish GameStrings | TresCom, Spanish Official Translation | DreamWorks Interactive; Nick3069 |
| `ru.strings.txt` | Russian GameStrings | TresCom, Russian Unofficial Translation (Game Strings) | cynodesmus |
| `ru.subtitles.txt` | Russian voice captions (`Stream.tpa`, `Effects.tpa`) | TresCom, Russian Fan Translation (Subtitles) | cynodesmus |
| `it.subtitles.txt` | Italian voice captions (`Stream.tpa`), timings adjusted by hand | TresCom, Italian Fan Translation (Subtitles) v1.0.0 | KeyofBlueS |
| `en.subtitles.txt` | the canonical English captions and timings of every sample the retail or the restored audio plays (made by `tools/stream_fix_pack.cpp`) | TresCom, English Stream TPA Fix (Subtitle Timing Fix), March 14, 2026 | KeyofBlueS |

The downloads are hosted and preserved by the TresCom community and state no licence (`docs/LEGAL.md`).

## The port's completion (written for Trespasser Recompiled)

| File | Content |
|---|---|
| `pt-BR`, `fr`, `de`, `es` `.subtitles.port.txt` | every voice caption of the English game (486 samples, the restored dialogue included), with `en.subtitles.txt`'s lines and times |
| `ru.subtitles.port.txt` | the Russian captions with the canonical lines and times, and the restored lines; it replaces the release's lines of the same sample |
| `it.subtitles.port.txt` | the 76 `Effects.tpa` captions the Italian release does not have |
| `it.strings.port.txt` | the whole Italian game text |
| `de.strings.port.txt` | the developer hint placeholders 18000 and 18002-18010 the German release lacks |
| `ru.strings.port.txt` | the strings the Russian release lacks, and the mouse, space and arrow key names it left in English |
| `<language>.interface.txt` | the words on the menu pictures (official wording where a release has it), the legal lines, the controls card, the objectives, the Debug, Cheats and Video Test panels, the compass, the Quick Save name and notices, and the 29 lines of the beach tutorial (`Tutorial::` keys) |
| `en.interface.txt` | the English key of every interface text; `keep=` names the languages that write a text as English does |

The beach tutorial's lines are the English text of the level's own TEXT actions (inside `be.GRF`), not string IDs; the
port matches them by that text and shows the `Tutorial::` translation, which writes the controls as tokens so the key or
button of the device in use is drawn. The loading screen's two labels (World Initialization, Level File Copy) and the
Options labels are GameStrings 10010-10055: the game's own text boxes are translated where the frontend and the loading
screen read them, as well as in the translated copies of the menu `.ddf` files.

A release's text is used first; a completion file only adds what it lacks, except captions: a caption completion replaces the
release's lines of the same sample because it carries the corrected timing. String 10 (the saves' file name) is never
translated. The tutorial's captions write controls as tokens (`<JUMP>`, `<HAND>`, `<LOOK>`...) that the game shows as the
key or button of the device in use.

## Format

UTF-8, `\n` line ends. `*.strings*.txt`: a header line, then `ID<TAB>text` per string, with `\\`, `\n`, `\r` and `\t`
escaped. `*.subtitles*.txt`: a header line, then `S<TAB>pack<TAB>sample` (the sample's ID in the TPA, hex) followed by one
`C<TAB>time<TAB>text` per caption, the time in the game's 0.2 s units. `*.interface.txt`: a header line, then
`key<TAB>text`; a key is the English text, or `Context::Text` where the same English means different things.

## Regenerating and checking

The release files: extract the downloads and run `translation_pack <folder> assets\translations` (`docs/BUILD.md`);
each source is recognised by name, size and SHA-256, so anything else in the folder is ignored. `l10n_tests` proves
that every language covers every GameStrings ID, every captioned sample of the English game and every interface key,
with the same placeholders and control tokens, no English left, every character in the language's code page and in
Arial; `l10n_tests --inventory` also lists the game's menu files. `frontend_tests` takes the lettering out of every
retail menu picture and checks that each language's word fits its plate.
