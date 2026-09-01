# pond

![pool preset, rain and breeze](doc/pool.png)

A numerically honest wave tank as screen candy. Linear dispersive water waves
(gravity + capillary, finite depth) on the surface of a rectangular basin with
rigid walls, solved spectrally and advanced *exactly* in time, then drawn as a
3-D basin — walls, floor, refraction, caustics, sky — that you can orbit and
zoom, with walls and bottom switchable to glass. A top-down CPU renderer is
kept as `--2d`.

C17. Dependencies: SDL2 and an OpenGL 3.3 core context (macOS ships 4.1;
Mesa/NVIDIA on Linux; WebGL2 in the browser). No GL loader library: the
handful of entry points used are fetched through `SDL_GL_GetProcAddress`
(`src/gl.h`). Same source builds for Linux, macOS and Emscripten.

## Build

    make            # native: needs SDL2 dev package (apt: libsdl2-dev, brew: sdl2)
    make test       # DCT against the O(N^2) definition; mode frequency against Airy
    make bench      # headless timings of the CPU path on a 512^2 grid
    make web        # needs emsdk on PATH; output in build/web/, serve it statically

    ./pond                                 # 512^2 grid, 1280x800 window, "pond" preset
    ./pond --basin 25x12 --depth 1.8       # any rectangle, in metres
    ./pond --preset 3 --scene rain,breeze  # pool, with sources already on
    ./pond --glass 1 --preset 3 --scene breeze   # floor only: bright water, caustics, no walls
    ./pond --glass 3 --cam 40,-20,1.3      # start looking up through a glass bottom
    ./pond --glass 4 --preset 4 --scene breeze   # a block of sea with no container
    ./pond --2d                            # top-down CPU renderer (no GL needed)
    ./pond --nomsaa                        # if window creation fails on an odd display
    ./pond --frames 120 --snap3d shot.bmp  # scripted screenshot
    ./pond --bench 300 --preset 4 --scene breeze --snap sea.bmp

In the browser the grid defaults to 256^2; append `?grid=512` to the URL for
the finer mesh (single-threaded WASM, so budget accordingly; 512 is the cap,
the heap is fixed at 128 MB). `make web
WEB_DIR=docs` puts the build where GitHub Pages can serve it from `/docs`.
Use `h` for the help there (Firefox keeps F1 for itself); on a touch
screen, one finger is the finger, two fingers orbit and pinch-zoom.

## Controls

`F1` or `h` shows the full list in the window. In short:

| | |
|---|---|
| click / shift-click on the water | drop / big drop |
| drag on the water | finger |
| drag off the water, ctrl/alt + drag, right or middle drag, arrow keys | orbit the camera |
| wheel, PgUp/PgDn, `o` | zoom, reset camera |
| `t` | container: opaque → floor only → glass walls → glass walls + bottom → none |
| `1` `2` `3` `4` | presets: tray 30 cm · pond 3 m · pool 12 m · sea 80 m |
| `[` `]` / `{` `}` | width / length, 5 % per press, hold to sweep (live: the physics changes, the field is kept; aspect kept within 4:1) |
| `,` `.` / `\` | depth / make it square again at the same area |
| `r` `i`/`I` | rain on/off, rate |
| `b` | breeze (directional wind sea) on/off |
| `p` `k`/`K` | plane wavemaker on/off, wavelength |
| `-` `=` | time warp |
| `x`/`X` | extra uniform damping |
| `g`/`G` `f` | display gain, floor pattern |
| `c` `space` `s` `q` | clear, pause, screenshot (bmp), quit |

In `--2d` mode the wheel is the time warp, `v` toggles a height-map view and
`h` prints the help to the terminal.

## What it computes

Small-amplitude potential flow with a free surface (Airy theory):

    omega^2(k) = (g k + sigma k^3 / rho) tanh(k h)
    gamma(k)   = 2 nu k^2 + gamma0                (Lamb §349, plus a knob)

The eigenmodes of a rectangular basin with vertical walls are
`cos(pi m x / Lx) cos(pi n y / Ly)`, which is exactly the DCT-II basis with
samples at cell centres. So the state lives in mode space as a pair
`(A, B) = (eta_hat, eta_hat_t / omega)` per mode, and advancing by `dt` is

    (A + iB) <- (A + iB) exp(-gamma dt) exp(-i omega dt)

No CFL condition, no stability limit, no numerical dispersion; `dt` can be
whatever the frame took. Rendering needs one inverse 2-D DCT per frame;
real-space sources (drops, finger, paddle) are accumulated in a buffer and
injected with one forward DCT on the frames where they occur. The paddle is
separable, `profile(x) * 1(y)`, so it only touches the plane-wave modes
`(m, 0)` and costs one 1-D transform. The breeze is applied directly in mode space (k^-4 spectrum above a peak, cos^2 directional
spread about the x axis). The mean level (0,0) is pinned: the pool keeps its
water.

Frame stepping uses a fixed sub-step (1/240 s at warp 1) and lazily cached
powers of the per-mode rotor, so a frame of `p` sub-steps is a single pass
over the modes regardless of `p`.

The DCT is Makhoul's: one N-point complex FFT plus twiddles, radix-2, ~150
lines in `src/dct.c`.

## 3-D view (`src/view3d.c`)

World units are metres, y up. The height field goes to the GPU as an R32F
texture; the surface mesh is one vertex per cell, the vertex shader fetches
the height and finite-difference normal with `texelFetch`, so nothing needs
float filtering (WebGL2-safe). Per fragment:

- from above: `refract` the view ray at the surface, intersect it with the
  interior of the basin box, shade whatever it hits — floor pattern times the
  caustic light map, or wall tiles, or the outside world if that face is
  glass — attenuate by the actual path length in water, add Schlick Fresnel
  times a procedural sky that contains the sun (so glints come out of the
  same reflection, scaled by the correct Fresnel factor);
- from below: the same with the index inverted, so beyond the critical angle
  you get total internal reflection of the floor, and Snell's window inside
  it.

Caustics are computed on the CPU every frame by refracting the sun through
each surface cell and splatting where the ray lands on the floor (bilinear,
then a 3×3 binomial blur). That is the forward ray map, so folds and the
wall's shadow on the floor come out on their own; the light map is an R8
texture the floor and the surface shader both read.

Container modes (`t`):

- **opaque** — tiled walls and floor;
- **floor only** — the walls hold the water but are not drawn; the floor
  continues a little outside the basin as a table, and a refracted ray that
  would have hit a wall leaves the water there and lands on the table
  instead, so the water stays as bright as with opaque walls and the
  caustics remain. For the caustic pass the surface is continued past the
  wall planes by its even (mirror) extension — which is exactly what the
  cosine basis represents — so the pattern runs to the edge instead of
  stopping at a phantom wall shadow;
- **glass walls**, **glass walls + bottom** — Fresnel-reflecting glass,
  far faces first; what a refracted ray hits through glass is the outside
  world, which is why the water reads darker in these modes; the strip of
  floor a wall would have shadowed is lit flat, as sunlight through glass
  would;
- **none** — surface, four sides and a flat bottom face, a block of water
  held by nothing.

![floor only](doc/floor-only.png)
![no walls, from above and below](doc/no-walls.png)

The basin can be any rectangle. The mode grid is fixed (nx = ny), so a
non-square basin has non-square cells; the solver doesn't care (it uses the
true wavenumber of each mode) and the renderer takes dx and dy separately.

Whenever walls are not drawn, the water body's vertical faces at the wall
planes (top edge on the surface mesh) are drawn as a translucent volume with
the water's own Fresnel reflection of the sky, opacity following the
absorption over a basin length — a tray is nearly clear, a 12 m pool is
murky. The camera can go below the floor.

Text is an 8×8 public-domain bitmap font rasterised on the CPU into a
window-sized RGBA overlay, updated only when the HUD changes; the glyph
scale follows the drawable height and shrinks until the help panel fits.

## 2-D renderer (`src/render.c`, `--2d`)

All from the height field in real units, so a 1 mm ripple in a tray and a
10 cm swell in a pool look right relative to each other:

- unit normal from the gradient;
- the floor pattern sampled where a vertical ray lands after paraxial
  refraction, `(x, y) + h (1 - 1/n) grad eta`;
- caustics as `1 / |det J|` of that surface-to-floor map,
  `J = I + h (1 - 1/n) Hess(eta)`, clamped at the folds and faded for deep
  floors (crests focus, troughs diverge);
- Schlick Fresnel for a viewer straight above, reflecting a sky gradient;
- a sun glint `(r · sun)^32` plus a broad fill light;
- round-trip absorption `exp(-2 mu h)` per channel (red first, then green) and
  a scattering colour that takes over as the floor fades;
- sRGB output.

Depth and basin size are physical: the tray (2 cm) shows almost no
refraction, only glints; the 12 m pool at 2 m depth is all caustics; the sea
preset is deep enough that only the surface shading survives.

## Budget

On a 512^2 grid the CPU does, per frame: one inverse DCT (~25 Mflop), one
pass over 262k modes, and the caustic splat (262k refractions). That is
~15 ms on a slow Xeon and well under 10 on a recent laptop; the GPU side
(262k-vertex mesh, a few full-screen passes) is trivial for anything with a
real GPU. WASM is single-threaded here on purpose (pthreads need COOP/COEP
headers, which static hosts rarely serve), so 256^2 is the sensible browser
default.

Possible speedups, none done yet: two rows per complex FFT in the DCT, and
moving the caustic splat to the GPU (render the surface mesh into the light
map with additive blending, which is the same forward map).

## Where it could go

- **Nonlinearity**: the High-Order Spectral method (West et al. 1987,
  Dommermuth & Yue 1987) is pseudo-spectral and a direct extension of this
  solver; order M = 1 is exactly what is here.
- **Arbitrary basin shapes** are the one thing the spectral approach cannot
  do; that is Boussinesq finite-difference territory, with approximate
  dispersion.
- **GLSL renderer**, as above.

## Layout

    src/dct.[ch]      radix-2 FFT, DCT-II/III, 2-D row–column
    src/wave.[ch]     dispersion tables, rotor propagation, sources
    src/view3d.[ch]   GL scene: shaders, meshes, caustics, camera, overlay
    src/gl.h          the GL 3.3 / GLES 3.0 subset used, loaded via SDL
    src/text.[ch]     8x8 bitmap text into an RGBA canvas (src/font8x8.h)
    src/render.[ch]   top-down CPU shading (--2d)
    src/main.c        SDL2 window, input, timing, bench, Emscripten loop
    tests/            dct and wave tests (run with `make test`)
    web/shell.html    Emscripten shell

The 3-D path was exercised under Xvfb with Mesa's llvmpipe (OpenGL 4.5 core).
The web build compiles cleanly with Emscripten 3.1.6 (WebGL2, same GLSL with a
`#version 300 es` header) but has not been run in a browser here.

Emscripten notes: emsdk is the easy route. The Debian/Ubuntu `emscripten`
package ships a frozen cache without the SDL2 port, so `-sUSE_SDL=2` fails
until you copy `/usr/share/emscripten/.emscripten` somewhere writable, set
`FROZEN_CACHE = False` and `CACHE` to a writable copy of the cache, and point
`EM_CONFIG` at it. `-sMINIFY_HTML=0` is in the Makefile because that package's
HTML minifier is broken with current Node.

## License

GNU General Public License v3.0 or later — see `LICENSE`. The 8×8 bitmap
font in `src/font8x8.h` is public domain (Daniel Hepper, after Marcel
Sondaar / IBM) and is included unchanged.
