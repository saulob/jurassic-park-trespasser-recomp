# Launcher header art

`header-art.png` is this port's own header art, drawn in code by `tools/make-header-art.py` (no game or film asset):
an island at dusk seen from the jungle, with a volcanic peak, the sun setting over the sea, mist, sauropods wading
and palm and tree-fern silhouettes. It is 3840x432 (8.89:1) RGBA, like the other ports' header art. A horizontal
alpha mask lets the launcher's own header gradient show through on the left: alpha is 0 up to 22% of the width, rises
along a smoothstep curve and reaches full at 55%, so the art has no visible edge. Left of 58% there is only sky and
low ridges, so nothing shows half-faded behind the title. Regenerate it with (Pillow and NumPy installed):

```powershell
py -3 tools\make-header-art.py
```

The launcher draws it over the header gradient at the header's full height, flush with the header's top, right and
bottom; a narrow window cuts it from the left (`launcher/header_art.h`). Without the file it keeps the gradient alone.
The build copies it next to `trespasser.exe`, into `assets\launcher`.

# Sponsor heart

`sponsor-heart.svg` is the source of truth for the heart in the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`), the same file as in the other ports. `sponsor-heart.png` is the runtime
asset, a white 64x64 rendering of it with a transparent background. Do not edit it by hand; regenerate it after every
SVG change with rsvg-convert (MSYS2 `mingw-w64-clang-x86_64-librsvg`):

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```

The launcher draws it before the button label, as a square slightly taller than the label's text.

# Page icons

`icons/` holds the sidebar's eight SVGs, the source of truth, with one file per page and no rendered copies. They are
compiled into the launcher (`launcher/page_icons.cpp`), so they ship as no loose file. `icons/SOURCES.md` records where
each one came from and its license.
