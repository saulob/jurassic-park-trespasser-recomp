# HD loading art

The game's loading screen shows one 640x480 render per level (`data\menu\li_*_0.tga`, 15-bit, 1998). The port draws
that picture again at the screen's resolution (de-dithered, framed, over its own blurred surroundings) and can use a
picture drawn for it instead: put `<slot>.png` or `<slot>.jpg` here and the build copies it next to `trespasser.exe`,
into `assets\frontend\loading_hd`. A missing or unreadable file only means the game's own picture is shown.

| Slot       | Shown for (scene file)                        | Game picture               |
|------------|-----------------------------------------------|----------------------------|
| `li_be`    | `be.scn` (New Game starts here)               | `menu\li_be_0.tga`         |
| `li_jr`    | `jr.scn`                                      | `menu\li_jr_0.tga`         |
| `li_pv`    | `pv.scn` (not in the retail data)             | `menu\li_pv_0.tga`         |
| `li_ij`    | `ij.scn`                                      | `menu\li_ij_0.tga`         |
| `li_it`    | `it.scn`                                      | `menu\li_it_0.tga`         |
| `li_lab`   | `lab.scn`                                     | `menu\li_lab_0.tga`        |
| `li_a1`    | `as.scn`                                      | `menu\li_a1_0.tga`         |
| `li_a2`    | `as2.scn`                                     | `menu\li_a2_0.tga`         |
| `li_sum`   | `sum.scn`                                     | `menu\li_sum_0.tga`        |
| `li_other` | any other scene (`TestScene.scn`, start-up `temp.scx`) | `menu\li_other_0.tga` |

The game picks the row itself (CLoaderWnd, 0x00415C40): the first scene name equal to the level's, without case, else
`li_other`.

Each picture is drawn in the same frame as the game's: 4:3, opaque, 2880x2160 recommended (anything from 1440x1080 up
works; larger pictures are made smaller when a level loads, other shapes are cropped by up to a third, then
letterboxed). The progress plate covers the band from 18% to 82% of the width and from 76% to 96% of the height:
keep what identifies the level out of it. Keep the look of the game's own render of that place.
