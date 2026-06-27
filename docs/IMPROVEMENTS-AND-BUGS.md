# EmbeddedPaint — Improvements & Bugs

> **Scope & framing.** EmbeddedPaint is an **MVP proof-of-concept cartridge** for the
> Pico console: a working MS-Paint-style freehand drawing app on an STM32F411RE driving
> a 128×160 ST7735. It proves the input → framebuffer → display pipeline end-to-end on
> real hardware. This document is the **honest defect-and-improvement register** for that
> MVP: every item below is grounded in the actual source (`Core/Src/main.c`, 691 lines,
> verified 2026-06-27).
>
> Two categories are kept strictly separate:
>
> - **BUG / DEFECT** — something the code does *today* that is wrong, fragile, or
>   misleading, with a concrete fix.
> - **FULL-RELEASE SCOPE** — a capability the MVP deliberately does **not** have yet,
>   closed by a named, always-planned story on the console roadmap. These are *not*
>   failures of the PoC; they are the boundary the PoC was drawn at.
>
> Where an MVP design choice looks odd (indirect render, no DMA, two drivers with
> opposite byte order), I name the planned story that resolves it rather than pretending
> the gap isn't there.

---

## Severity-ranked summary

| #  | Severity | Item | Type | What's wrong / missing | Planned story |
|----|----------|------|------|------------------------|---------------|
| 1  | 🔴 High   | Indirect render — paint only writes framebuffer | BUG | Painted pixels reach the LCD only when the cursor later *moves off* them; a fast stroke outruns the redraw and leaves gaps | `PLAT-DISPLAY-DIRECT-PAINT` |
| 2  | 🔴 High   | `LCD_Restore_Area` = up to 100 windowed SPI writes per move | BUG (perf) | Each cursor move re-addresses and transmits 10×10 pixels one-at-a-time over an 8 MHz polled SPI bus — the dominant frame cost | `PLAT-ST7735-DMA-BLIT` |
| 3  | 🔴 High   | Endianness disagrees with spaceShip driver | BUG | This file emits RGB565 **big-endian** `{hi, lo}`; spaceShip emits **little-endian** `{lo, hi}`. Two copy-pasted drivers, opposite byte order | `PLAT-SHARED-ST7735-DRIVER` |
| 4  | 🟡 Med    | `paint` / `saveColorToArray` prototype vs definition arg mismatch | BUG (latent) | Prototype declares `(y, x)`, definition declares `(x, y)`; compiles (same types) but actively misleads every reader and any future caller | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| 5  | 🟡 Med    | `LCD_courser` ignores its `color` argument | BUG | The cursor is **always black** regardless of the `color` passed; the parameter is dead and misleading | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| 6  | 🟢 Low    | Stray root `main.c` containing only `//test` | BUG (hygiene) | A dead 1-line file at repo root shadows the real entry point conceptually and confuses tooling | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| 7  | 🟢 Low    | `#include "main.h"` twice | BUG (hygiene) | Harmless (header guard) but a clear copy-paste artifact | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| 8  | 🟢 Low    | Dead `loadColorToArray` + shadowed globals | BUG (hygiene) | Prototyped, never defined; `current_x/y`, `old_x/y` globals shadowed by `main()` locals | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| 9  | 🟢 Low    | Swatch overdraw 20×20 into 16×16 window | BUG (cosmetic) | `LCD_OnSetup_color_choice` loops 400 px into a 16×16-pixel address window (16×16 = 256) — 144-px overspill | `GAME-PAINT-CARTRIDGE-REFACTOR` |
| —  | ⚪ Scope  | Undo / save-load / flood-fill / brush size / true eraser / larger palette | FULL-RELEASE | Deliberately out of MVP scope; the PoC ships a 6-colour pen with clear-screen only | `GAME-PAINT-FEATURE-PARITY` |

Severity legend: 🔴 visible/structural defect that blocks the console story or degrades the
core loop · 🟡 latent correctness/readability hazard · 🟢 hygiene/cosmetic · ⚪ planned
scope boundary, not a defect.

---

## 1. 🔴 Indirect render — `paint()` only writes the framebuffer

**Description.** Painting and display are decoupled. `paint()` does not draw anything; it
only writes the 10×10 block into `frame_buffer[][]`. A painted pixel is *flushed* to the
LCD only later, indirectly, when the cursor moves off that pixel and `LCD_Restore_Area()`
repaints the now-vacated cell from the buffer.

**Evidence** (`Core/Src/main.c`):

```c
void paint(uint8_t x, uint8_t y, uint16_t color){
    saveColorToArray(x, y, color);          // writes RAM only — no SPI, no LCD
}
```

In the main loop the only display writes per move are the cursor sprite and the *restore*
of the previous cell:

```c
LCD_Restore_Area(old_x, old_y);             // flushes buffer for the OLD cell to LCD
paint(new_x, new_y, paint_colors[color_index]); // RAM only
LCD_courser(new_x, new_y, 0);               // draws the (black) cursor
```

So the freshly painted cell under the cursor is hidden behind the black cursor sprite and
does not appear on-screen until a *future* move uncovers it via the restore path.

**Impact.** A fast joystick stroke that paints several cells per loop iteration can paint
faster than the restore path uncovers them, leaving visible gaps along the stroke. The
draw is also gated by the movement `threshold = 5`, so sub-threshold motion paints
nothing and looks unresponsive. Functionally the app works for slow, deliberate strokes
(the MVP demo case) but the render model is the wrong shape for a responsive tool.

**Fix.** Make paint *direct*: when a cell is painted, blit it to the LCD immediately
(write the 10×10 region to the panel in the same operation that writes the framebuffer),
rather than relying on a later restore to reveal it. The framebuffer stays the source of
truth for restore/clear, but it is no longer the *only* path pixels reach the screen.
Long term this folds into the shared dirty-rect display service (item 2).

**Fail-before / pass-after test.** Drive a scripted joystick ramp that paints N cells in a
single loop iteration (move delta > 1 cell-width per tick), then read back the panel
region (or the framebuffer-vs-screen diff via the test harness). **Before:** painted cells
under/ahead of the cursor are missing on-screen until a later move. **After:** every
painted cell is present on the panel within the same frame it was committed.

**Planned story:** `PLAT-DISPLAY-DIRECT-PAINT` — immediate-mode paint commit, always
planned as part of the responsive-input milestone.

---

## 2. 🔴 `LCD_Restore_Area` does up to 100 windowed SPI writes per cursor move

**Description.** Restoring the 10×10 cell under the old cursor position is done **one pixel
at a time**, and each pixel re-sends a full ST7735 address-window command sequence before
its single 2-byte colour write.

**Evidence** (`Core/Src/main.c`, `LCD_Restore_Area`):

```c
for (int dx = 0; dx < 10; dx++) {
  for (int dy = 0; dy < 10; dy++) {
    ...
    LCD_CommandMode(0x2A);                 // column addr  (4 data bytes)
    LCD_DataMode(0x00); LCD_DataMode(px);
    LCD_DataMode(0x00); LCD_DataMode(px);
    LCD_CommandMode(0x2B);                 // row addr     (4 data bytes)
    LCD_DataMode(0x00); LCD_DataMode(py);
    LCD_DataMode(0x00); LCD_DataMode(py);
    LCD_CommandMode(0x2C);                 // RAM write
    ...
    HAL_SPI_Transmit(&hspi1, data, 2, HAL_MAX_DELAY);  // ONE pixel
  }
}
```

That is a **1-pixel address window set 100 times**. Each pixel costs ~11 byte-wide SPI
transactions (3 command bytes + 8 address bytes + 2 colour bytes) plus the GPIO bit-bang
of DC/CS around each. Every transfer is a blocking `HAL_SPI_Transmit(..., HAL_MAX_DELAY)`
— **no DMA today** — over an SPI clock of **8 MHz** (`SPI_BAUDRATEPRESCALER_2` on a 16 MHz
APB2; the PLL is disabled, so APB2 is the raw 16 MHz HSI). The CPU spins on each byte.

**Impact.** This is the dominant per-move cost and the single biggest throughput
bottleneck in the app. Two compounding factors make it worse than it needs to be:

1. **Clock.** `SystemClock_Config()` runs `RCC_PLL_NONE` / `FLASH_LATENCY_0`, so
   SYSCLK = HCLK = APB1 = APB2 = **16 MHz** — roughly 16% of the part's 100 MHz ceiling.
   Enabling the PLL alone would multiply SPI throughput and CPU headroom several-fold.
2. **No DMA + per-pixel windowing.** The panel supports a single address window plus a
   contiguous RAM-write stream; restoring a 10×10 block should be **one** window set
   followed by **one** 200-byte streamed write, not 100 separate windowed writes.

**Fix (two independent, additive wins).**

| Fix | Effort | Win |
|-----|--------|-----|
| Set the address window **once** for the whole 10×10 region, then stream the 200 bytes in a single transfer | Low | Removes ~99% of the command/address overhead |
| Enable PLL → 100 MHz core + push the streamed write onto **SPI DMA** so the CPU isn't byte-blocked | Med | Frees the CPU and raises raw bus throughput |

These are the "I profiled it and found the bottleneck" wins for the console: the clock and
the per-pixel windowing, not the application logic.

> **PoC honesty:** DMA, double-buffering, PLL-at-100 MHz, and a dirty-rect blit do **not**
> exist in the code today. They are roadmap items, named here, not present capabilities.

**Fail-before / pass-after test.** Instrument a move that triggers a restore and count SPI
transactions (or wall-clock the restore via a GPIO toggle on a logic analyser / DWT cycle
counter). **Before:** ~100 windowed writes / hundreds of byte-transactions per restore at
8 MHz. **After:** one window set + one streamed transfer per restore, and (with PLL+DMA)
the CPU is free during the transfer. Assert a transaction-count ceiling so the regression
is enforced.

**Planned story:** `PLAT-ST7735-DMA-BLIT` (DMA + single-window block blit) on top of
`PLAT-CLOCK-PLL-100MHZ` (PLL enable). Always-planned platform milestone.

---

## 3. 🔴 Endianness disagrees with the spaceShip driver

**Description.** EmbeddedPaint and spaceShipGame each carry their **own copy-pasted**
ST7735 driver, and the two pack the 16-bit RGB565 colour into the 2-byte SPI payload in
**opposite byte order**.

**Evidence.**

EmbeddedPaint (`Core/Src/main.c`) — **big-endian, high byte first**, used consistently in
`LCD_Fill`, `LCD_OnSetup_color_choice`, `LCD_courser`, and `LCD_Restore_Area`:

```c
uint8_t data[] = {color >> 8, color & 0xFF};   // {hi, lo}
```

spaceShipGame (`Core/Src/main.c`, verified read-only) — **little-endian, low byte first**:

```c
uint8_t data[] = {color & 0xFF, color >> 8};   // {lo, hi}
```

**Impact.** Each driver was hand-tuned on-device, so each *looks* correct on its own panel
(and both games also force `0x21` INVON, meaning colours were eyeballed under display
inversion). But the two cannot share colour constants or a single driver as-is: the same
`0xF800` would render as a different colour through the other driver. For a 3-game console
that must present one ST7735 service, this is a hard reconciliation blocker — colour
palettes are not portable across the two games until the byte order is unified.

**Fix.** Build **one** shared ST7735 driver with a single, documented byte order
(big-endian / high-byte-first matches the ST7735 default RGB565 expectation and is the
natural choice). Migrate both games to it and delete both private copies. Pin the byte
order with a test so it can never silently diverge again.

**Fail-before / pass-after test.** A host-side unit test that packs a known set of RGB565
constants (red `0xF800`, green `0x07E0`, blue `0x001F`) through the shared driver's pack
function and asserts the exact byte sequence. **Before:** the two games produce different
byte sequences for the same constant. **After:** both games call the shared packer and the
asserted bytes match a single golden vector.

**Planned story:** `PLAT-SHARED-ST7735-DRIVER` — extract the one true display driver; the
always-planned platform-spine work that the standalone cartridges were a proof-of-concept
for.

---

## 4. 🟡 `paint` / `saveColorToArray` prototype-vs-definition argument mismatch

**Description.** The forward declarations name the parameters in **one** order; the
definitions name them in the **opposite** order. It compiles because the types are
identical (`uint8_t, uint8_t, uint16_t`), so the swap is silent — but it actively misleads
anyone reading or calling the functions.

**Evidence** (`Core/Src/main.c`):

```c
// prototypes (lines 81–82)
void paint(uint8_t y, uint8_t x, uint16_t color);
void saveColorToArray(uint8_t y, uint8_t x, uint16_t color);

// definitions (lines 606, 614)
void paint(uint8_t x, uint8_t y, uint16_t color){ saveColorToArray(x, y, color); }
void saveColorToArray(uint8_t x, uint8_t y, uint16_t color) { ... }
```

The call site passes `paint(new_x, new_y, ...)` and the buffer is indexed
`frame_buffer[x + dx][y + dy]`, so the *definition's* `(x, y)` order is the one that's
actually consistent with `frame_buffer[LCD_WIDTH][LCD_HEIGHT]`. The prototype is simply
wrong. (This also feeds the broader axis-naming confusion across the project — the paint
`pin_layout.txt` claims `A1 = Y / PB1 = X`, while the spaceShip ADC comments claim the
reverse; one is wrong, and inconsistent parameter naming makes it harder to ever pin
down.)

**Impact.** No runtime bug *today* because the types match, but it is a latent trap: the
moment someone trusts the prototype's `(y, x)` and reorders a call, the axes silently swap.
It is exactly the kind of footgun a shared API must not ship with.

**Fix.** Make the prototype match the definition (`(x, y, color)`), and document the
coordinate convention (`x` = column 0..127, `y` = row 0..159) once at the driver boundary.

**Fail-before / pass-after test.** A compile-with-`-Werror`/lint gate plus a unit test that
paints at `(x=5, y=20)` and asserts `frame_buffer[5][20]` (not `[20][5]`) holds the colour.
**Before:** prototype and definition disagree (lint flag / reviewer confusion). **After:**
signatures match and the indexing test passes against the documented convention.

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR` — the cartridge-contract refactor pass
that cleans the public surface before the game is wrapped in the shell.

---

## 5. 🟡 `LCD_courser` ignores its `color` argument (cursor is always black)

**Description.** `LCD_courser(uint8_t x, uint8_t y, uint16_t color)` takes a `color`
parameter but never uses it; it hard-codes black.

**Evidence** (`Core/Src/main.c`, `LCD_courser`):

```c
void LCD_courser(uint8_t x, uint8_t y, uint16_t color) {
    ...
    uint16_t black = 0x0000;
    uint8_t data[] = { (uint8_t)(black >> 8), (uint8_t)(black & 0xFF) };  // color param unused
    ...
}
```

Both call sites already pass `0` for `color`, so the behaviour is consistent — but the
parameter is dead and signals an intent (a coloured cursor) the code never honours.

**Impact.** Misleading API surface. A future caller passing a non-black colour will be
silently ignored. It also forecloses a real usability win: a cursor that previews the
current pen colour (or contrasts with the canvas) would make the hover/paint distinction
obvious to the user — today the only feedback that a colour is selected is the swatch.

**Fix.** Either honour the `color` argument (draw the cursor outline/fill in the passed
colour, e.g. preview the current pen colour) or remove the parameter entirely if a fixed
black cursor is the deliberate design. Honouring it is the better UX and costs nothing.

**Fail-before / pass-after test.** Call `LCD_courser(x, y, 0xF800)` and read back the
cursor region's centre pixel. **Before:** pixel is `0x0000` (black) regardless of arg.
**After:** pixel reflects the passed colour (or the parameter is gone and the call site no
longer passes a dead value).

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR`.

---

## 6. 🟢 Stray root `main.c` containing only `//test`

**Description.** There is a second `main.c` at the **repository root** whose entire content
is a comment. The real entry point is `Core/Src/main.c`.

**Evidence.** `/home/user/EmbeddedPaint/main.c`:

```c
//test
```

**Impact.** Dead file. It is not part of the STM32CubeIDE build (the project compiles
`Core/Src/main.c`), but it is a trap for grep/tooling and for any new build system that
globs `**/main.c`. Pure noise.

**Fix.** Delete `/home/user/EmbeddedPaint/main.c`.

**Fail-before / pass-after test.** A repo-hygiene check (CI `find . -name main.c` outside
`Core/Src`) that fails if a stray top-level `main.c` exists. **Before:** the check finds
the root file. **After:** only `Core/Src/main.c` remains.

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR` (hygiene sweep).

---

## 7. 🟢 `#include "main.h"` duplicated

**Description.** `main.h` is included twice, back-to-back.

**Evidence** (`Core/Src/main.c`, lines 10–11):

```c
#include "main.h"
#include "main.h"
```

**Impact.** Harmless — the header guard makes the second include a no-op — but it's a
clear copy-paste artifact and a small readability/quality smell. There's also a mangled
duplicate comment a few lines down (`/* Private includes --` followed by the full
`/* Private includes ... */` line), same category.

**Fix.** Remove the duplicate `#include` (and tidy the mangled comment line).

**Fail-before / pass-after test.** Lint rule / simple `grep -c '#include "main.h"'`
assertion == 1. **Before:** count is 2. **After:** count is 1.

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR` (hygiene sweep).

---

## 8. 🟢 Dead `loadColorToArray` prototype + shadowed globals

**Description.** Two related dead-code / shadowing smells.

**Evidence** (`Core/Src/main.c`):

- `uint16_t loadColorToArray(uint8_t y, uint8_t x);` is **prototyped** (line 83) but
  **never defined or called** — a dead declaration (also with the `(y, x)` arg-order
  inconsistency from item 4).
- File-scope globals `current_x/current_y` (line 45) and `old_x/old_y` (line 46) are
  **shadowed** by `main()`'s own locals (`int old_x = 40; int old_y = 90;` at lines
  114–115; the position logic uses the locals, not the globals). The globals are
  effectively dead.

**Impact.** Confusion and rot risk: a reader cannot tell which `old_x` is authoritative,
and the unused globals invite a future edit that "fixes" the wrong variable. The
`loadColorToArray` prototype implies a buffer-read path that doesn't exist.

**Fix.** Delete the dead `loadColorToArray` prototype. Remove the shadowing — keep a single
authoritative position state (drop the unused globals, or make the loop use them and drop
the locals). Document which is the source of truth.

**Fail-before / pass-after test.** Compile with `-Wshadow -Wunused`. **Before:** warnings
on `old_x/old_y` shadowing and the unused globals/prototype. **After:** clean build, single
position-state owner.

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR`.

---

## 9. 🟢 Colour-swatch overdraw — 20×20 pixels into a 16×16 window

**Description.** The selected-colour swatch is drawn into a 16×16-pixel address window but
the fill loop pushes 20×20 = 400 pixels.

**Evidence** (`Core/Src/main.c`, `LCD_OnSetup_color_choice`):

```c
uint16_t x_start = 80, x_end = 95;   // 16 columns inclusive (80..95)
uint16_t y_start = 10, y_end = 25;   // 16 rows inclusive (10..25)
...
for(uint32_t i = 0; i < (20 * 20); i++) {   // 400 px into a 16x16 = 256-px window
    HAL_SPI_Transmit(&hspi1, data, 2, HAL_MAX_DELAY);
}
```

**Impact.** Cosmetically harmless because the swatch is a solid single colour — the extra
144 pixels wrap within the same window and just rewrite the same cells — but it's wasted
SPI traffic and an off-by-design count that will misbehave the instant the swatch becomes
multi-colour or the window changes. (Same family as the spaceShip `LCD_Fill` overspill,
which is worse there because the loop count and window genuinely disagree.)

**Fix.** Loop exactly `width * height` (here 16×16 = 256), derived from the window bounds
rather than a hard-coded `20`. Better: route through the shared driver's
`fill_rect(x, y, w, h, color)` so the count is computed from the rect, not duplicated.

**Fail-before / pass-after test.** Assert the swatch fill issues exactly `w*h` colour
writes for the configured window. **Before:** 400 writes for a 256-px window. **After:**
256 writes, derived from bounds.

**Planned story:** `GAME-PAINT-CARTRIDGE-REFACTOR` (folds into the shared `fill_rect`).

---

## Full-Release scope — deliberately not in the MVP

These are **not bugs.** They are the planned feature boundary. The MVP ships a usable
6-colour freehand pen with a clear-screen reset; everything below is the parity work the
PoC was always meant to grow into. The 40 KB `frame_buffer[128][160]` already in RAM is
the foundation several of these build on.

| Feature | Why it's out of MVP | What it needs | Planned story |
|---------|---------------------|---------------|---------------|
| **Undo / redo** | Stroke history not modelled in MVP | A stroke/command stack or snapshot ring over the existing framebuffer | `GAME-PAINT-UNDO` |
| **Save / load drawing** | No persistence service yet (shared console need) | Flash-backed persistence of the framebuffer (shared with high-score storage) | `PLAT-PERSISTENCE` → `GAME-PAINT-SAVELOAD` |
| **Flood fill (paint bucket)** | Only per-cell paint in MVP | Scanline/queue fill over `frame_buffer[][]` + direct blit of the filled region | `GAME-PAINT-FLOODFILL` |
| **Adjustable brush size** | Fixed 10×10 block | Parameterize the paint/restore block dimensions (already a single constant today) | `GAME-PAINT-BRUSHSIZE` |
| **True white eraser** | "Clear" wipes the whole screen; no local erase | An eraser pen colour = canvas white that paints like any other colour (trivial once paint is direct, item 1) | `GAME-PAINT-ERASER` |
| **Larger palette / colour picker** | Fixed 6 RGB565 colours, cycle-only | An expanded palette or an RGB565 picker UI; the cycle UI already exists as the seed | `GAME-PAINT-PALETTE` |

Rolled up on the board as **`GAME-PAINT-FEATURE-PARITY`**.

---

## Cross-cutting note — why most of these resolve "for free" at the console layer

The high-severity items (1, 2, 3) are not really *paint* bugs — they are **driver-and-clock**
bugs that exist because EmbeddedPaint is a standalone PoC cartridge that owns its own
firmware, its own copy-pasted ST7735 driver, and a 16 MHz no-PLL clock. The console's
always-planned spine fixes them structurally:

- **One shared ST7735 driver** with a single byte order, **DMA**, and **single-window block
  blits** → closes items 2 and 3 and gives item 1 a clean direct-paint path.
- **PLL → 100 MHz** clock in the shared platform init → multiplies the throughput headroom
  every game depends on.
- **A cartridge lifecycle** (`init / update(input, dt) / render / teardown`) + a shell that
  owns the hardware → the refactor pass (`GAME-PAINT-CARTRIDGE-REFACTOR`) that naturally
  sweeps items 4–9 while reshaping the file.

So the cleanest sequencing is: fix the **shared driver + clock** first (biggest, profiled
win), then the **cartridge refactor** (which absorbs the hygiene and API-clarity items),
then the **feature-parity** stories. That ordering is the "what I'd do next" — driver and
clock before features, because the driver is the bottleneck and the shared contract is what
turns three proof-of-concept cartridges into one console.
