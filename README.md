# pond

![pool preset, rain and breeze](doc/pool.png)

A numerically honest wave tank as screen candy. Linear dispersive water waves
(gravity + capillary, finite depth) on the surface of a rectangular or
circular basin with rigid walls, solved spectrally and advanced *exactly* in
time, then drawn as a 3-D basin — walls, floor, refraction, caustics, sky — that you can orbit and
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
    ./pond --shape disk --basin 10         # a round basin, 10 m across
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
| `n` | basin shape: rectangle ↔ disk (the field starts over) |
| `1` `2` `3` `4` | presets: tray 30 cm · pond 3 m · pool 12 m · sea 80 m |
| `[` `]` / `{` `}` | width / length (disk: diameter), 5 % per press, hold to sweep (live: the physics changes, the field is kept; aspect kept within 4:1) |
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

### The problem

Incompressible, irrotational flow of water of uniform depth $h$ in a basin
$\Omega$ with vertical walls. The velocity is $\mathbf u = \nabla\phi$, the free
surface is $z = \eta(\mathbf x, t)$ with $\mathbf x = (x, y)$, and

$$
\nabla^2 \phi = 0 \quad\text{in } -h < z < \eta,
\qquad
\frac{\partial\phi}{\partial z} = 0 \text{ at } z=-h,
\qquad
\frac{\partial\phi}{\partial n} = 0 \text{ on the walls}.
$$

At the free surface, the kinematic condition (the surface moves with the
fluid) and the dynamic condition (Bernoulli, with surface tension $\sigma$
and density $\rho$):

$$
\eta_t + \nabla_{\!h}\phi\cdot\nabla_{\!h}\eta = \phi_z,
\qquad
\phi_t + \tfrac12|\nabla\phi|^2 + g\eta
 - \frac{\sigma}{\rho}\,\nabla_{\!h}\!\cdot\!\frac{\nabla_{\!h}\eta}{\sqrt{1+|\nabla_{\!h}\eta|^2}} = 0
\qquad\text{at } z = \eta .
$$

### Linearisation and separation

For small steepness $|\nabla\eta| \ll 1$ the quadratic terms drop and the
surface conditions can be applied at $z = 0$:

$$
\eta_t = \phi_z, \qquad \phi_t = -g\eta + \frac{\sigma}{\rho}\nabla_{\!h}^2\eta
\qquad\text{at } z = 0 .
$$

Everything is now linear with $z$-independent coefficients, so separate
$\phi = \Phi(\mathbf x)\,Z(z)\,T(t)$. Laplace's equation splits into

$$
\nabla_{\!h}^2\Phi + k^2\Phi = 0 \quad\text{in }\Omega,\qquad \partial_n\Phi = 0 \text{ on } \partial\Omega,
\qquad\qquad
Z'' = k^2 Z,\quad Z'(-h)=0 \;\Rightarrow\; Z = \cosh k(z+h).
$$

The horizontal part is the Neumann eigenproblem of the basin. Its
eigenfunctions $\Phi_n$ with eigenvalues $k_n^2$ form an orthogonal basis for
functions on $\Omega$, and — this is the whole point — the shape of the basin
enters *only* through them. Expand

$$
\eta(\mathbf x,t) = \sum_n a_n(t)\,\Phi_n(\mathbf x),
\qquad
\phi(\mathbf x,z,t) = \sum_n b_n(t)\,\Phi_n(\mathbf x)\,\frac{\cosh k_n(z+h)}{\cosh k_n h},
$$

and the two surface conditions become, mode by mode,

$$
\dot a_n = k_n\tanh(k_n h)\, b_n,
\qquad
\dot b_n = -\Big(g + \frac{\sigma k_n^2}{\rho}\Big)\, a_n ,
$$

i.e. every mode is an independent harmonic oscillator,

$$
\ddot a_n + \omega_n^2 a_n = 0,
\qquad
\boxed{\;\omega_n^2 = \Big(g k_n + \frac{\sigma k_n^3}{\rho}\Big)\tanh(k_n h)\;}
$$

with the exact solution $a_n(t) = a_n(0)\cos\omega_n t + \dfrac{\dot a_n(0)}{\omega_n}\sin\omega_n t$.
This is Airy's dispersion relation with capillarity; the limits are the
familiar ones — $\omega^2 = gk$ in deep water, $\omega = k\sqrt{gh}$ in
shallow water, $\omega^2 = \sigma k^3/\rho$ for capillary ripples. The
minimum phase speed, $(4g\sigma/\rho)^{1/4} \approx 0.23$ m/s at
$\lambda \approx 1.7$ cm, is what makes a raindrop ring look the way it does.

### Damping

Viscosity is added the way Lamb does it (*Hydrodynamics* §349): for a
free-surface wave of wavenumber $k$ the amplitude decays as $e^{-2\nu k^2 t}$,
which is the leading-order effect of viscosity on an irrotational wave. To
that we add a uniform rate $\gamma_0$ standing in for wall and bottom boundary
layers. So each mode is taken to evolve as

$$
a_n(t) = e^{-\gamma_n t}\Big(A_n\cos\omega_n t + B_n\sin\omega_n t\Big),
\qquad
\gamma_n = 2\nu k_n^2 + \gamma_0 ,
$$

which is the solution of $\ddot a + 2\gamma\dot a + (\omega^2+\gamma^2)a = 0$; for
$\gamma \ll \omega$ that is the damped oscillator to $O(\gamma^2)$, and the
oscillation frequency is kept exactly at $\omega_n$ by construction.

### The integrator

The state of a mode is the pair $(A, B) = (a, \dot a/\omega)$ evaluated now.
Advancing by any $\Delta t$ is the rotation

$$
A + iB \;\longleftarrow\; (A + iB)\, e^{-\gamma\Delta t}\, e^{-i\omega\Delta t},
$$

exact for the model above, for any step size, with no stability constraint
and no numerical dispersion — the frequencies are the physical ones to
float precision. There is no time-stepping error to accumulate. Sources
are impulses: a drop adds $\Delta\eta$ (transformed into $\Delta A_n$), the
wavemaker adds $\Delta\eta_t$ (transformed into $\Delta B_n = \Delta\dot a_n/\omega_n$).
The $k = 0$ mode (the mean level) is pinned at zero.

The code keeps a fixed sub-step, $\Delta t = 1/240$ s at time-warp 1, with
lazily cached powers of the per-mode rotor, so a frame of $p$ sub-steps is one
pass over the modes for any $p$; that is a convenience, not a discretisation.

### The mode bases

**Rectangle** $[0,L_x]\times[0,L_y]$: the Neumann eigenfunctions are

$$
\Phi_{mn} = \cos\frac{m\pi x}{L_x}\cos\frac{n\pi y}{L_y},
\qquad
k_{mn}^2 = \pi^2\Big(\frac{m^2}{L_x^2} + \frac{n^2}{L_y^2}\Big).
$$

Sampled at cell centres $x_i = (i+\tfrac12)\Delta x$ these are exactly the
DCT-II basis vectors, and the DCT-II/DCT-III pair is an exact orthogonal
transform on the grid, so injection and reconstruction commute with no
leakage. Makhoul's algorithm does an $N$-point DCT with one $N$-point complex
FFT plus twiddles (`src/dct.c`, radix-2). The cells need not be square.

**Disk** of radius $R$: $\Phi_{mn} = J_m(k_{mn} r)\,e^{im\theta}$ with
$J_m'(k_{mn}R) = 0$. The implementation uses the discrete counterpart; see
below.

### Rendering, for completeness

Refraction of a ray $\mathbf I$ at a surface with unit normal $\mathbf N$ into
water of index $n = 1.333$ (Snell in vector form):

$$
\mathbf T = \frac{1}{n}\mathbf I - \Big(\frac{1}{n}(\mathbf N\!\cdot\!\mathbf I) + \sqrt{1 - \tfrac{1}{n^2}\big(1-(\mathbf N\!\cdot\!\mathbf I)^2\big)}\Big)\mathbf N .
$$

Caustics are the forward map of the sun through the surface: a surface point
$\mathbf p$ lands on the floor at $\mathbf q = \mathbf p + (h+\eta)\,\mathbf T_{xz}/(-\mathbf T_y)$,
and the floor irradiance, in units of the flat-water irradiance, is
$\sum_{\text{sheets}} 1/|\det \partial\mathbf q/\partial\mathbf p|$. The code
evaluates this by splatting each cell's area at its landing point, which sums
the sheets and produces the folds automatically. Reflectance is Schlick's
approximation to Fresnel, $R = R_0 + (1-R_0)(1-\cos\theta)^5$ with
$R_0 = ((n-1)/(n+1))^2 = 0.02$; absorption along a path $s$ in water is
Beer–Lambert, $e^{-\mu s}$ per channel.

### Validity

The model is linear: it is exact for the linearised problem and an
approximation to real water for steepness $ka \ll 1$. The default drops have
slopes up to ~0.15, which is already where Stokes corrections would be a few
percent; the display gain multiplies slopes for the eye only, not in the
physics. What nonlinearity would add is discussed under "Where it could go".

### The disk (`src/disk.c`)

Same physics, different eigenbasis. On a disk the Neumann eigenfunctions are
$J_m(kr)e^{im\theta}$ with $J_m'(kR)=0$; in polar coordinates the Helmholtz
operator separates as

$$
\nabla_{\!h}^2 = \frac1r\frac{\partial}{\partial r}\Big(r\frac{\partial}{\partial r}\Big) + \frac{1}{r^2}\frac{\partial^2}{\partial\theta^2}.
$$

The grid is polar, $n_t$ angles × $n_r$ rings ($n_t$ = grid, $n_r$ = grid/2),
with cell centres at $\rho_i = (i+\tfrac12)/n_r$ of the radius. An FFT in
$\theta$ makes the angular part exact: mode $m$ sees $-m^2/r^2$. For the radial
part the code does not sample Bessel functions; it takes the eigenvectors of
the *discrete* radial operator on the unit disk, written in flux form so that
the centre and the wall need no special treatment:

$$
(L_m f)_i = \frac{1}{\rho_i}\,\frac{\rho_{i+1/2}(f_{i+1}-f_i) - \rho_{i-1/2}(f_i - f_{i-1})}{h^2} - \frac{m^2}{\rho_i^2} f_i,
\qquad h = 1/n_r,
$$

with $\rho_{-1/2} = 0$ (no flux through the centre) and the flux through the
outer face $\rho_{n_r - 1/2} = 1$ set to zero (rigid wall). Multiplying by
$\rho_i$ gives a symmetric tridiagonal matrix $T_m$ and the generalised
problem $T_m f = -\kappa^2 W f$ with $W = \mathrm{diag}(\rho_i)$; with
$S_m = W^{-1/2} T_m W^{-1/2}$ this is an ordinary symmetric tridiagonal
eigenproblem $S_m g = -\kappa^2 g$, $f = W^{-1/2}g$. Because $S_m$ is
symmetric its eigenvectors $G_m$ are exactly orthonormal, so the radial
transform pair

$$
c = G_m^{\!\top} W^{1/2}\hat\eta_m \qquad\text{and}\qquad \hat\eta_m = W^{-1/2} G_m\, c
$$

is exactly inverse — the disk's counterpart of the DCT's orthogonality, and
what keeps repeated injection stable. The physical wavenumber is
$k_{mn} = \kappa_{mn}/R$, so the basis is built once for the unit disk and
resizing the basin only rebuilds the $\omega$ table. Eigenvalues by implicit
QL, eigenvectors by inverse iteration; $\kappa_{mn}$ agrees with the Bessel
roots to $10^{-7}$ for the lowest modes and $10^{-4}$ at the fifth
(`tests/test_disk.c`), as an $O(h^2)$ scheme should. The basis
($M \times n_r \times n_r$ floats, 67 MB at 512 × 256) takes about a second.

Per frame the disk costs $n_r$ complex FFTs of length $n_t$ plus one
$n_r\times n_r$ matrix–vector product per angular mode and plane; with the
products written in explicit 4-lane vectors (`vector_size`, which maps to
SSE, NEON and wasm SIMD) that is about the same as the rectangle's DCT, and
memory-bound. Radial modes beyond the grid's radial Nyquist,
$\kappa > \pi n_r$ — eigenvectors living on the one or two innermost rings,
artefacts of the tiny cells there — are dropped at injection; besides being
unphysical they decayed into denormal floats, which cost a hundred times a
normal multiply and made the first version crawl. The rotor pass flushes
anything below $10^{-30}$ to zero for the same reason. The wall paddle is
separable in $(r,\theta)$ and goes straight into mode space through the few
angular modes a 60° sector contains. Everything downstream — rotor
propagation, damping, the breeze band, drops — is shared code indexing a flat
mode array. The renderer uses a polar mesh (ring 0 drawn as the centre fan
with a single averaged normal), a cylinder in the refraction intersection,
cylindrical walls, and the same Cartesian caustic light map, splatted with
polar cell areas so flat water still comes out at 1.

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

**Nonlinearity (HOS).** Keep the potential-flow problem but not the
linearisation. In Zakharov's form the state is $\eta(\mathbf x)$ and the
surface potential $\psi(\mathbf x) = \phi(\mathbf x, \eta)$, and the exact
free-surface equations are

$$
\eta_t = (1+|\nabla\eta|^2)\,\phi_z - \nabla\psi\cdot\nabla\eta,
\qquad
\psi_t = -g\eta - \tfrac12|\nabla\psi|^2 + \tfrac12(1+|\nabla\eta|^2)\phi_z^2 + \tfrac{\sigma}{\rho}\kappa(\eta),
$$

where $\phi_z$ at the surface is the Dirichlet-to-Neumann operator applied to
$\psi$. The High-Order Spectral method (West et al. 1987, Dommermuth & Yue
1987) expands that operator in powers of $\eta$ to order $M$ and evaluates
each term pseudo-spectrally — products in real space, derivatives in mode
space — so a step costs $O(M^2)$ transforms and needs a real time integrator
(RK4, with the linear part handled by the same exact rotor as now as an
integrating factor). $M = 1$ is exactly this program. What $M = 3$ adds:
Stokes' crests — sharper peaks, flatter troughs, the bound harmonics that
ride on a wave; an amplitude-dependent frequency, $\omega \approx \omega_0(1+\tfrac12 k^2a^2)$
in deep water; and genuine wave–wave interaction — resonant quartets that
move energy across the spectrum, the Benjamin–Feir instability that turns a
regular wave train into groups, and the focusing that makes freak waves. A
wind sea stops being a frozen superposition and evolves. It cannot break: as
steepness approaches ~0.3 the expansion diverges and one has to filter or
dissipate. The walls survive the change of model because a rigid wall is a
mirror symmetry of the full equations too, so the even-extended field the
DCT represents stays consistent under the nonlinear products. The cost is the
transforms: on the CPU a 512² step is tens of milliseconds, so HOS at 60 fps
means the FFTs move to the GPU, which is the larger project.

**Caustics on the GPU.** The forward map is exactly what rasterisation does.
Render the surface mesh into the light-map framebuffer with a vertex shader
that refracts the sun at each vertex and outputs the landing point on the
floor as the vertex position; each triangle then covers the floor area it
illuminates, and a fragment shader writes its irradiance,
$|\det\,\partial\mathbf p/\partial\mathbf q|$, which is `dFdx`/`dFdy` of the
original surface coordinates. Additive blending sums the sheets, so folds and
overlaps come out right by construction — better than the CPU splat, which
is single-sheet with a clamp. Requirements: a framebuffer object, a float or
half-float colour attachment, additive blending and screen-space derivatives,
all core in OpenGL 3.0/3.3 and in WebGL2 (with `EXT_color_buffer_float`,
which every current browser exposes). Any GPU that runs the program at all,
including a 2011-era Intel HD under Mesa, has these. The light map never
comes back to the CPU, and the whole caustic pass — ~5 ms of CPU per frame
now — disappears. The transforms themselves could follow the same road later.

**Arbitrary basin shapes**, ellipses included, mean numerically computed
Neumann eigenmodes and a dense transform; see the discussion of the disk
above for why the circle is special.

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
