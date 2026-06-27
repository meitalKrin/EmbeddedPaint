# EmbeddedPaint — Architecture

> **Status: MVP / proof-of-concept cartridge.** This document describes the
> firmware *as it exists today* — a single, self-contained STM32F411RE image —
> and then scopes the always-planned Full-Release work that turns it into one
> **game module** inside a 3-game handheld console (alongside spaceShipGame and a
> greenfield Snake), eventually re-hosted on the Raspberry Pi **Pico 2 /
> RP2350**.
>
> Throughout, I separate **what the code does today** from **what Full-Release
> adds**. Where a design choice looks odd in isolation (indirect render,
> always-black cursor, big-endian byte order, a 40 KB framebuffer), I call it out
> honestly and tie it to the roadmap item that closes it. None of these gaps are
> hidden; each is a deliberate MVP boundary with a named successor.

---

## 1. Context & Overview

EmbeddedPaint is an **MS-Paint-style freehand drawing app** running bare-metal on
an STM32F411RE (Nucleo-F411RE class), driving a 128×160 ST7735 SPI TFT, taking
input from an analog 2-axis joystick and three GPIO buttons. The user moves a
10×10 cursor; holding the **pen** button paints with the current colour, and a
**colour** button cycles a 6-colour palette. A **clear** button wipes the canvas.

### What it is today

- A **single monolithic firmware image**. `main()` owns `HAL_Init`,
  `SystemClock_Config`, every peripheral init (`MX_GPIO_Init`, `MX_SPI1_Init`,
  `MX_ADC1_Init`, `MX_USART2_UART_Init`), the `while(1)` super-loop, and its
  **own copy-pasted ST7735 + ADC + GPIO driver** inlined into `Core/Src/main.c`
  (691 lines). There is no shared platform layer and no menu/shell.
- A **proof-of-concept "cartridge."** It validates the input → state → render
  loop for one game on this hardware. The integrating console shell that would
  let a user *select* this game and *exit* back to a menu is **deliberately out
  of MVP scope** — that is the always-planned console feature, not a defect of
  this image.

### Why a monolith today (and why it changes)

A single-image build is the fastest way to prove the hardware bring-up:
display init, the SPI byte path, ADC scan order, and button polling are all
exercised in one `while(1)`. The cost is that **the game *is* the firmware** —
there is no `main()` to return to, no game-select, no shared driver to reuse
across the three planned games. spaceShipGame is the same shape (its own
copy-pasted driver), which means today **two near-identical ST7735 drivers exist
and disagree on byte order** (see §6, §8). Reconciling them into one shared
driver behind a cartridge lifecycle is the central Full-Release refactor (§9).

### System context

```
        ┌──────────────────────────────────────────────┐
        │            STM32F411RE @ 16 MHz (HSI)          │
        │                                                │
  Joy ──┤ ADC1 (PA1/PB1, 12-bit, scan, polled)           │
 Btns ──┤ GPIO  (PC4 colour / PC5 pen / PA10 clear, PU)  │── SPI1 (8 MHz) ──▶ ST7735
        │                                                │   DC=PB13 CS=PB14   128×160
  PC  ──┤ USART2 (PA2/PA3 @115200, printf debug only)    │   RES=PB15          RGB565
        │                                                │
        │  frame_buffer[128][160] uint16_t  (40 KB RAM)  │
        └──────────────────────────────────────────────┘
```

---

## 2. Drivers & Non-Functional Requirements

The app is interactive and visual, so the NFRs that actually constrain the
design are **perceived stroke latency**, **memory footprint**, and
**persistence** of the drawing.

| NFR | Today (MVP) | Target (Full-Release) | Notes |
|-----|-------------|------------------------|-------|
| **Stroke latency** (move → pixel on screen) | Bounded by a **per-pixel windowed SPI restore** (up to ~100 single-pixel windowed writes per cursor move) over an **8 MHz** SPI clock with **no DMA**; a fast stroke can outrun the redraw (indirect render — §5, §7). | DMA block transfer + dirty-rect → one windowed burst per move; PLL → faster SPI. | The dominant latency cost is *windowing overhead*, not pixel volume. |
| **Memory** | `frame_buffer[128][160] uint16_t` = **40 KB** of a 128 KB device (~31% of RAM) held statically for the full session. | On RP2350 (520 KB RAM) multiple framebuffers / undo history become trivial. | Framebuffer is the price of persistence; see §4. |
| **Persistence (in-session)** | Full-screen RAM framebuffer survives cursor movement and redraw; **lost on power-off** (no flash save). | Persistence service (flash) → save/load drawings. | No save/load today. |
| **Determinism / input** | Joystick polled 8 samples/axis, averaged, deadzoned; buttons polled active-low with pull-ups. Adequate; not interrupt-driven. | Debounced input service shared across games. | |
| **Debuggability** | `printf` retargeted to USART2 @115200. | Unchanged. | Debug only — not a product surface. |

> **Honest framing:** the framebuffer is what *buys* persistence — every painted
> pixel is remembered so the cursor can move over it and restore the canvas
> underneath. That is a real engineering trade (40 KB RAM for a simple,
> correct persistence model), not waste. The lag risk it introduces (indirect
> render, §5/§7) is the MVP tax that DMA + dirty-rect pays off in Full-Release.

---

## 3. Component View

All components below live **inside a single image** today. The boxes are logical;
physically they are functions in `Core/Src/main.c`.

### 3.1 Clock — the headline performance fact

`SystemClock_Config()` runs the MCU from the **internal HSI oscillator at 16 MHz
with the PLL DISABLED** (`RCC_PLL_NONE`), `FLASH_LATENCY_0`, and AHB/APB1/APB2 all
÷1:

```
SYSCLK = HCLK = APB1 = APB2 = 16 MHz   (HSI, no PLL)
```

The STM32F411 is rated to **100 MHz**, so the firmware runs at **~16% of the
silicon's ceiling**. This is verified in code (`RCC_OscInitStruct.PLL.PLLState =
RCC_PLL_NONE`, `RCC_SYSCLKSOURCE_HSI`) and in the `.ioc`
(`RCC.APB1Freq_Value=16000000`). The `.ioc`'s `PLLCLKFreq_Value=96000000` is a
**dormant default** — it is not applied, because the code never enables the PLL.

> **This is the single biggest, cheapest performance win available** and the
> clearest "I profiled it and found the bottleneck" story: enabling the PLL to
> 100 MHz (≈6.25×) plus moving the display to DMA-SPI is the headline
> Full-Release optimisation (§7, §9). Today: **16 MHz, no PLL, no DMA — stated as
> fact, not aspiration.**

### 3.2 GPIO — buttons & display control

| Pin | Role | Config | Active |
|-----|------|--------|--------|
| **PC4** | Colour-cycle button | Input, **pull-up** | Active-low (RESET = pressed) |
| **PC5** | Pen button (held = paint, released = hover) | Input, **pull-up** | Active-low (held = `GPIO_PIN_SET`? see note) |
| **PA10** | Clear / reset canvas | Input, **pull-up** | Active-low (RESET = pressed) |
| PB13 | ST7735 **DC** (data/command) | Output PP | Bit-banged per byte |
| PB14 | ST7735 **CS** (chip select) | Output PP | Bit-banged (`LcdOpen`=low / `Lcdclose`=high) |
| PB15 | ST7735 **RES** (reset) | Output PP | Pulsed at init |

> **Pen-button polarity note (real code behaviour):** the pen branch in `main()`
> treats **`HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_5) == GPIO_PIN_SET`** as the
> hover/no-paint path and the `else` as paint. With a pull-up, `SET` = not
> pressed. So as written, **paint occurs when PC5 is pressed (pulled low)** and
> hover when released — consistent with "held = paint." I flag the polarity
> explicitly because PC4/PA10 use `== GPIO_PIN_RESET` for "pressed" while PC5
> uses `== GPIO_PIN_SET` for "released," and that asymmetry is exactly the kind
> of thing a shared, debounced input service should normalise (§9).

### 3.3 SPI1 + ST7735 driver (write-only panel)

- **SPI1 master**, `BaudRatePrescaler = /2` of the 16 MHz APB2 = **8 MHz SCK**,
  8-bit, MSB-first, **mode 0** (CPOL=0 `SPI_POLARITY_LOW`, CPHA=1edge
  `SPI_PHASE_1EDGE`).
- **Software NSS** (`SPI_NSS_SOFT`); **CS and DC are bit-banged GPIO** toggled
  around every transfer.
- Every byte goes out via **`HAL_SPI_Transmit(..., HAL_MAX_DELAY)` — blocking,
  byte-at-a-time. No DMA. No double-buffer. No dirty-rect.**
- Pins: SCK=PA5, MISO=PA6 (unused — panel is write-only), MOSI=PA7.
- **ST7735 init sequence** (`LCD_Init`): `0x01` reset → `0x11` wake → `0x3A`/`0x05`
  16-bit colour → **`0x21` INVON (display inversion ON)** → `0x29` display ON.
  Colours were tuned on-device with inversion on (§6).

### 3.4 ADC1 — analog joystick

- ADC1, **12-bit**, `ScanConvMode = ENABLE`, software-triggered
  (`ADC_SOFTWARE_START`), **polled**, **144-cycle** sample time,
  `NbrOfConversion = 2`.
- Channels: **IN1 = PA1 (rank 1)**, **IN9 = PB1 (rank 2)**.
- Read pattern in `main()`: per sample iteration, **one** `HAL_ADC_Start`, then
  **two** back-to-back `PollForConversion` + `GetValue` reads, relying on the
  scan-sequencer to deliver X then Y. 8 samples/axis are summed and averaged.
- **Hand-calibrated centre offsets** `x_center_offset = 2150`,
  `y_center_offset = 1900` (deliberately **not** the ideal 2048 — eyeballed on
  the bench), with a local `deadzone = 10`.

> **Two honest caveats, both real:**
> 1. **Brittle scan read.** "One Start, two reads" leans on the sequencer order;
>    a missed/extra EOC can **swap the axes**. A robust input service should read
>    each channel deterministically (or DMA the whole sequence).
> 2. **Axis-label contradiction (documented bug).** The `MX_ADC1_Init` comments
>    say *X on PA1 / Y on PB1*, but `pin_layout.txt` says **`A1 = Y` / `PB1 = X`**.
>    One label is wrong. This is a documentation/wiring ambiguity to resolve
>    against the physical joystick, not a code crash — but it is exactly the kind
>    of thing the shared input service must pin down (§8, §9).

### 3.5 USART2 — debug

USART2 on PA2 (TX) / PA3 (RX) @115200, with `printf` retargeted via
`__io_putchar` + `_write`. **Debug only** — not a user-facing surface.

### 3.6 Paint loop, framebuffer & restore-area (the heart of the app)

The `while(1)` super-loop each iteration:

1. **Sample** joystick (8×, average, deadzone) → map to a target `(new_x, new_y)`.
2. **Movement gate:** only act if the cursor moved more than `threshold = 5` px.
3. **Pen state:**
   - **Hover** (PC5 not pressed): `LCD_Restore_Area(old)` then redraw cursor.
   - **Paint** (PC5 pressed): `LCD_Restore_Area(old)`, then **`paint(new, ...)`**,
     then redraw cursor.
4. **Buttons:** PA10 → clear + reset to colour 0; PC4 → cycle colour.

The three render primitives:

- **`paint()` → `saveColorToArray()`** — writes a 10×10 block **into the
  framebuffer only**. *No SPI.* (See §5 — this is the indirect-render decision.)
- **`LCD_courser()`** — draws the 10×10 cursor as a single windowed block… but
  **always in black** (`0x0000`); the `color` argument is **read and ignored**
  (the function hard-codes `black`).
- **`LCD_Restore_Area()`** — repaints the 10×10 region under the *old* cursor
  from the framebuffer, **one pixel per windowed SPI write** (sets a 1-px address
  window, transmits 2 bytes, ~100 times). This is where painted pixels finally
  reach the glass.

---

## 4. Data Model

The app's state is small, static, and entirely in RAM:

| Symbol | Type | Size | Purpose |
|--------|------|------|---------|
| `frame_buffer` | `uint16_t[128][160]` | **40,960 B (40 KB)** | Full-screen RGB565 canvas — the persistence store |
| `paint_colors` | `uint16_t[6]` | 12 B | Palette: red `0xF800`, green `0x07E0`, blue `0x001F`, yellow `0xFFE0`, magenta `0xF81F`, cyan `0x07FF` |
| `color_index` | `uint8_t` | 1 B | Active palette slot (cycled by PC4) |
| `current_x/y`, `old_x/y` | `uint16_t` | 8 B | Cursor position (globals; **shadowed by locals in `main()`** — §8) |
| `num_colors` | `uint8_t` | 1 B | `sizeof(paint_colors)/sizeof(...)` = 6 |

**Memory budget.** `frame_buffer` is **40 KB of the device's 128 KB** SRAM (~31%)
— statically allocated, present for the whole session. That is the deliberate
cost of a simple, correct persistence model: one RGB565 word per pixel means
"what's on screen" and "what's in memory" can never disagree. It is comfortably
within budget today, and on RP2350's **520 KB** it becomes a rounding error
(§9) — making multi-framebuffer features (undo, layers) trivial later.

---

## 5. Rendering Pipeline

This is the most important section to read honestly, because the pipeline is
**indirect** by design and that has a real consequence.

### The three stages

```
joystick move ──▶ paint() ──▶ saveColorToArray()      [RAM only — no SPI]
                                     │
                                     ▼
                          frame_buffer[x..x+9][y..y+9] = color
                                     │
   next cursor move over that cell ──┤
                                     ▼
                 LCD_Restore_Area(old)  ──▶ 100× { 1-px window + 2-byte SPI }   [SPI]
                                     │
                                     ▼
                 LCD_courser(new) ──▶ 10×10 windowed block, ALWAYS BLACK        [SPI]
```

1. **`paint()` writes RAM, not the screen.** A painted 10×10 block lands in
   `frame_buffer` immediately, but **nothing is sent to the LCD at paint time.**
2. **Pixels reach the glass only when the cursor later moves off them.**
   `LCD_Restore_Area(old)` is what flushes the framebuffer region under the
   *previous* cursor position to the panel — so a painted pixel is only drawn
   once the cursor has moved away from it.
3. **The cursor is an always-black 10×10 overlay** (`LCD_courser` ignores its
   `color` arg), drawn on top so the user can see where they are.

### Quantifying the cost (this is the headache)

`LCD_Restore_Area` does **not** open one window for the 10×10 block. It opens a
**fresh 1-pixel address window for every pixel**:

```
per restored pixel:  CASET(0x2A) + 4 data + RASET(0x2B) + 4 data + RAMWR(0x2C) + 2 data
                     ≈ 13 byte-wide blocking SPI transactions, each wrapped in
                       bit-banged DC + CS toggles
per cursor move:     10 × 10 = 100 pixels  →  up to ~100 windowed writes
                     ≈ 1300 byte transfers + 200 CS edges + 300 DC edges per move
```

At **8 MHz SPI, byte-at-a-time, no DMA**, the address-window *overhead* (commands +
CS/DC bit-banging) **dwarfs** the 2 bytes of actual pixel payload. Compare: a
single windowed block write of the same 10×10 (as `LCD_courser` already does)
would cost **one** CASET/RASET/RAMWR and 200 payload bytes — roughly an order of
magnitude cheaper. The per-pixel windowing is the real cost, not the pixels.

### The consequence — render can be outrun

Because paint only writes RAM and the flush happens on the *next* move, a **fast
stroke can outrun the redraw**: the framebuffer is correct, but the screen lags
behind the hand. The canvas is never *wrong* (it's all in `frame_buffer`), it
just renders late. This is the headline visible quirk of the MVP and the direct
motivation for the DMA + dirty-rect Full-Release work (§9).

---

## 6. Key Decisions & Rationale

| Decision | Today | Rationale | Trade-off / successor |
|----------|-------|-----------|------------------------|
| **Framebuffer persistence vs. direct draw** | Keep a full `[128][160]` RGB565 canvas in RAM; paint writes RAM. | A single source of truth for "what's on screen" makes restore-under-cursor trivially correct and makes save/load a future memcpy. | 40 KB RAM + indirect-render lag. Pays off on RP2350 (undo/layers). |
| **Restore-area redraw** | Repaint the 10×10 under the old cursor from the framebuffer so the cursor leaves no black smear. | Correct, simple, and framebuffer-backed — no read-back from the (write-only) panel needed. | Implemented **per-pixel windowed**, which is the perf cost (§7). Fix: one windowed block + DMA. |
| **Swatch masking** | Cursor/paint **early-return** inside the colour-swatch rect (≈ x80–95, y10–25). | Stops the user painting over / erasing the on-screen palette indicator — a cheap UI-integrity guard. | Geometry is hand-tuned and slightly loose; a real UI layer would own widget bounds. |
| **INVON (`0x21`)** | Display inversion ON at init. | Colours were **tuned on-device** with inversion on; the RGB565 constants are correct *for this panel with INVON*. | Couples the palette constants to the init sequence — must travel together into the shared driver. |
| **Big-endian byte order here** | All transfers send `{color >> 8, color & 0xFF}` (high byte first). | Matches what *this* panel displayed correctly during bring-up. | **Disagrees with spaceShipGame**, which sends `{color & 0xFF, color >> 8}`. Two copy-pasted drivers, opposite endianness — must be reconciled into **one** shared driver (§8, §9). |
| **Always-black cursor** | `LCD_courser` ignores its `color` arg and draws black. | A black 10×10 is unambiguous against the white canvas; simplest possible cursor. | The unused `color` parameter is dead surface; a real cursor service would honour it (e.g. XOR/contrast cursor). |

---

## 7. Performance Analysis

Three compounding factors set the performance envelope today. All three are
**real and verified**, and all three have a clear, named fix.

### 7.1 Clock — 16 MHz, no PLL

The MCU runs at **16 MHz (HSI, PLL disabled)** — ~16% of its 100 MHz ceiling
(§3.1). Every cycle of HAL overhead, every SPI byte, every ADC sample is **~6×
slower than it needs to be.** Enabling the PLL is a *configuration* change
(`SystemClock_Config` + flash latency), not a redesign — the single cheapest
win on the board.

### 7.2 SPI — 8 MHz, blocking, byte-at-a-time, no DMA

SCK is **8 MHz** (APB2/2). Every pixel is two `HAL_SPI_Transmit` bytes with the
CPU **blocked** on each (`HAL_MAX_DELAY`), wrapped in bit-banged DC/CS toggles.
There is **no DMA**, so the Cortex-M4 spins on the SPI peripheral instead of
doing anything useful. (Note SPI clock is derived from APB2 — raising the core
clock with the PLL also raises the achievable SCK, compounding the win.)

### 7.3 Per-pixel windowed restore — the dominant cost

The killer is **`LCD_Restore_Area` opening a 1-pixel address window per pixel** —
**up to ~100 windowed writes per cursor move** (§5). The command/CS/DC overhead
per window is ~6× the 2-byte payload, so the restore path is *overhead-bound*,
not bandwidth-bound.

### 7.4 Net effect & the optimisation order

Because paint is indirect and the flush is heavy, **a fast stroke outruns the
render** (§5). The fixes, in descending value:

1. **Coalesce the restore** into one windowed block per dirty 10×10 (no code
   beyond what `LCD_courser` already does) — biggest algorithmic win.
2. **Enable the PLL** → ~6× core/SPI throughput — biggest config win.
3. **DMA the SPI block transfers** → free the CPU and stream pixels.
4. **Dirty-rect / double-buffer** → only push what changed.

> The point I'd make in review: **the bottleneck is the *algorithm* (per-pixel
> windowing), not the silicon.** The 16 MHz clock makes it worse, but even at
> 100 MHz, 100 windowed writes per move is the wrong shape. Fix the redraw first,
> then the clock, then DMA.

---

## 8. Risks & Known Issues

Surfaced honestly — every one is real in the current tree. They split into
**runtime risks** and **code-hygiene / drift risks**.

### Runtime / behavioural

| Risk | Detail | Severity | Disposition |
|------|--------|----------|-------------|
| **Indirect-render lag** | Paint writes RAM; pixels flush only on the *next* move via per-pixel restore → fast strokes outrun the screen (§5/§7). | High (visible) | Full-Release: coalesced restore + DMA + dirty-rect. |
| **Endianness disagreement** | This game sends RGB565 **big-endian** `{hi, lo}`; spaceShipGame sends **little-endian** `{lo, hi}`. Two copy-pasted drivers contradict each other. | High (blocks shared driver) | Pick one byte order in the **single shared ST7735 driver** (§9). |
| **Brittle ADC scan read** | "One Start, two reads" relies on sequencer order; a missed EOC can swap axes (§3.4). | Medium | Deterministic per-channel read or DMA the sequence in the shared input service. |
| **Axis-label contradiction** | ADC comments say X=PA1/Y=PB1; `pin_layout.txt` says A1=Y/PB1=X. One is wrong. | Medium (docs/wiring) | Resolve against the physical joystick; encode once in the input service. |

### Code hygiene / build drift

| Issue | Detail | Severity | Disposition |
|-------|--------|----------|-------------|
| **Prototype/def arg-name mismatch** | `paint(uint8_t y, uint8_t x, ...)` prototype vs `paint(uint8_t x, uint8_t y, ...)` definition; same for `saveColorToArray`. Compiles (same types) but is **misleading** about coordinate order. | Low (correctness), Medium (readability) | Normalise signatures. |
| **Stray root `main.c`** | Repo-root `/main.c` is just `//test` — dead. Real entry is `Core/Src/main.c`. | Low | Delete it. |
| **Double `#include "main.h"`** | `main.h` included twice at the top of `Core/Src/main.c`. | Low | Remove the duplicate. |
| **Dead `loadColorToArray` prototype** | Declared (line 83), **never defined**. Unused dead surface. | Low | Remove or implement. |
| **Shadowed globals** | `current_x/current_y/old_x/old_y` globals are shadowed by `int old_x/old_y` locals in `main()`; the globals aren't the loop's state. | Low | Remove globals or use them consistently. |
| **`.ioc` drift (real workflow risk)** | Code was hand-edited (`NbrOfConversion = 2`, `144CYCLES`) but the `.ioc` still says `NbrOfConversionFlag=1` and `SamplingTime = 3 cycles`. **A CubeMX regenerate would clobber the hand-fixes.** | Medium (silent regression risk) | Re-apply the changes in CubeMX so the `.ioc` is the source of truth, or freeze generation. |

> **`.ioc` drift is the one I'd flag loudest in a code review** — it is invisible
> until someone opens CubeMX, regenerates, and silently reverts the working ADC
> config. It's a classic STM32 brownfield trap and it argues for moving the
> hand-tuned config into version-controlled, regeneration-safe code.

---

## 9. MVP vs. Full-Release Scope & Console Integration

This is where the proof-of-concept framing matters most. **Everything in §1–§8
is the MVP cartridge.** The items below are the always-planned features that turn
it into a console game module — they were scoped *out* of the MVP on purpose, not
forgotten.

### 9.1 What the MVP proves today

- Hardware bring-up: ST7735 over SPI, ADC joystick, debounced-ish buttons,
  printf-debug — all working in one image.
- A correct, persistent **drawing model** (the framebuffer) and a working
  input → state → render loop.
- A real, measured **performance bottleneck** (per-pixel windowed restore at
  16 MHz / 8 MHz / no DMA) with a clear fix order.

### 9.2 What Full-Release adds (the app itself)

| Gap today | Full-Release feature |
|-----------|----------------------|
| Render can be outrun (indirect) | **DMA SPI + framebuffer dirty-rect**: one block transfer per dirty region, CPU free. |
| 16 MHz / no PLL | **PLL → 100 MHz** (and the higher SPI clock that comes with it). |
| No undo, no save/load | **Persistence service** (flash) for saved drawings; RAM undo history. |
| 6 fixed colours, fixed brush, no eraser | Larger palette / colour picker, **brush size**, true white **eraser**, **flood-fill**. |
| Always-black cursor (ignored colour arg) | Contrast/XOR cursor that honours its colour. |
| Two drivers disagree on endianness | **One shared ST7735 driver**, one byte order, settled once. |
| Brittle ADC read, axis ambiguity | Shared **input service**: deterministic channel read, debounce, one canonical axis mapping. |

### 9.3 The monolith → cartridge refactor (the console spine)

The central console problem is that **each game owns the whole firmware and there
is no way to exit**. The fix is a **game-module / cartridge contract** plus a
**shell**:

```c
// Cartridge lifecycle — each game implements this; the shell owns the hardware.
typedef struct {
    void (*init)(Platform* p);                 // claim resources, build initial state
    GameResult (*update)(const Input* in, uint32_t dt_ms);  // one tick; may signal EXIT
    void (*render)(Platform* p);               // draw via shared driver
    void (*teardown)(Platform* p);             // release; return control to the shell
} Cartridge;
```

- **EmbeddedPaint becomes one `Cartridge`**: `paint()`/`saveColorToArray()` move
  behind `update`; `LCD_*` calls move behind the shared driver invoked from
  `render`; `LCD_Init`/`MX_*` move into the **shell/platform** so games never own
  hardware init again.
- A **shell / menu** (game-select dashboard) owns the hardware, runs shared
  **input / render / timing** services, dispatches to the selected game, and —
  critically — **gets control back** when a game's `update` returns `EXIT`
  (the game-exit path that does **not** exist today).
- **Shared platform/HAL** to design once: **one** ST7735 driver (DMA +
  framebuffer/dirty-rect), **one** input service (joystick ADC + debounced
  buttons), **one** timing/scheduler, **one** seeded RNG, **one** persistence
  service (flash for high scores / saved drawings).
- **Snake** (greenfield) is built natively to this contract so it doubles as the
  **reference implementation** that proves the model — and fixes spaceShip's
  unbounded-heap bug by giving the body linked-list a proper node lifecycle.

### 9.4 Pico 2 / RP2350 — a HAL-swap, not a rewrite

Because the cartridge contract sits on a **platform abstraction**, moving to the
**Pico 2 / RP2350** is a **HAL swap**: replace the STM32 `HAL_*` / `MX_*` calls
behind the platform with **pico-sdk** equivalents.

| STM32 (today) | RP2350 (Full-Release) |
|---------------|------------------------|
| `HAL_SPI_Transmit` + bit-banged CS/DC | `hardware/spi` (+ `hardware/dma`; **PIO** can offload the display/SPI entirely) |
| `HAL_ADC_*` polled scan | `hardware/adc` |
| `HAL_GPIO_*` | `hardware/gpio` |
| `HAL_Delay` / `HAL_GetTick` | `pico/time` |

**Why RP2350 strengthens the product story:**

- **Dual Cortex-M33 @150 MHz** — one core can run the render/SPI pump while the
  other runs game logic.
- **PIO** — offload the ST7735 serial stream to a programmable I/O state machine,
  freeing the CPU from byte-banging entirely.
- **520 KB RAM** — the **40 KB framebuffer is a rounding error**, so **multiple
  framebuffers, undo history, and layers become trivial** — the exact features
  the 128 KB STM32 made expensive.

The platform-abstraction / HAL layer is itself an **always-planned feature**: it
is what makes the MVP's STM32-specific code portable, and it's the reason the
console can target a more capable chip without rewriting any game's logic.

---

## Appendix — Verified facts (against `Core/Src/main.c`, `pin_layout.txt`, `EmbeddedPaint.ioc`)

- **Clock:** `RCC_PLL_NONE`, `RCC_SYSCLKSOURCE_HSI`, `FLASH_LATENCY_0`, APB ÷1 →
  16 MHz everywhere. `.ioc` `APB1Freq_Value=16000000`. (PLL **not** enabled today.)
- **Framebuffer:** `uint16_t frame_buffer[128][160]` = 40 KB (`LCD_WIDTH 128`,
  `LCD_HEIGHT 160`).
- **Cursor:** `LCD_courser` hard-codes `black = 0x0000`; the `color` arg is unused.
- **Byte order:** `{color >> 8, color & 0xFF}` (big-endian) in fill / restore /
  swatch — opposite of spaceShipGame.
- **Restore:** `LCD_Restore_Area` opens a **1-pixel** CASET/RASET window per
  pixel, 10×10 = 100 windowed writes per move.
- **ADC:** `NbrOfConversion = 2`, `ADC_SAMPLETIME_144CYCLES`, IN1=PA1 rank1 /
  IN9=PB1 rank2; one `HAL_ADC_Start` then two reads per sample; offsets 2150/1900.
- **`.ioc` drift:** `.ioc` still says `NbrOfConversionFlag=1` and
  `SamplingTime = ADC_SAMPLETIME_3CYCLES` — diverged from the hand-edited code.
- **Hygiene:** double `#include "main.h"`; `paint`/`saveColorToArray`
  prototype↔def arg-name swap; `loadColorToArray` declared, never defined; root
  `/main.c` = `//test`; shadowed `current_x/old_x` globals.
