# EmbeddedPaint — Product Requirements Document

> **An MS-Paint freehand drawing app running bare-metal on an STM32F411RE driving a 128×160 ST7735 TFT, painted with an analog joystick.** This document follows the `gds-prd` / `gds-gdd` shape, adapted for an embedded-firmware deliverable.

---

## 1. Header

| Field | Value |
|---|---|
| **Product** | EmbeddedPaint — MS-Paint-style freehand drawing app |
| **Status** | **MVP — shipping today** on STM32F411RE + ST7735. Builds, flashes, runs. |
| **Form factor today** | Standalone monolithic firmware image (owns `main()`, `while(1)`, all peripheral init, and its own ST7735/ADC/GPIO driver) |
| **Form factor planned** | **Console cartridge** — one of three games (with `spaceShipGame` and a greenfield `Snake`) on a shared shell with a game-select menu |
| **Target MCU** | STM32F411RE — Cortex-M4F, FPU, 512 KB flash / 128 KB RAM, rated to 100 MHz (Nucleo-F411RE class) |
| **Display** | ST7735, 128×160, RGB565 (16 bpp), SPI1 master |
| **Input** | 2-axis analog joystick (ADC1) + 3 push-buttons (GPIO, active-low) |
| **Toolchain** | STM32CubeIDE / CubeMX (`MxCube.Version=6.16.1`, `FW_F4 V1.28.3`), HAL drivers |
| **Source of truth** | `Core/Src/main.c` (691 lines), `EmbeddedPaint.ioc` |
| **Doc owner** | Firmware engineer (author) |
| **Date** | 2026-06-27 |

**PoC framing (read this first).** EmbeddedPaint is a deliberately scoped **proof-of-concept cartridge**. It proves the hard parts — driving an ST7735 over hand-rolled SPI, reading and calibrating an analog joystick, and maintaining a **persistent 40 KB framebuffer** as the canvas model — on a clock running at **16 MHz, ~16% of the part's 100 MHz ceiling**. Several core-loop simplifications (indirect render, no exit-to-shell, no shared driver, palette of six) are **MVP scope choices**, each closed by a **named, already-planned Full-Release story** in §8. This PRD states every such gap explicitly and never hides one; that is the honesty bar this document is held to.

---

## 2. Vision & Pillars

**Vision.** Pick up a joystick, hold a button, and *draw immediately* — no menus, no boot screen, no mode dialog. EmbeddedPaint is the smallest honest expression of "a paint program on a microcontroller": the moment the LCD lights up white, you are already drawing.

### Pillars

| Pillar | What it means | How the MVP delivers it today |
|---|---|---|
| **Immediate freehand drawing** | Zero ceremony between power-on and a stroke on screen | On reset the screen fills white, the swatch shows the active colour, and the cursor is live. Hold **PC5** and move — you are painting. |
| **Persistent canvas** | What you draw *stays drawn*; the canvas is real state, not a transient blit | A full-screen `frame_buffer[128][160]` of `uint16_t` (**40 KB**) is the authoritative canvas. Every painted pixel is committed to it and survives the cursor moving over it. |
| **Tactile colour switching** | Changing colour is a physical click, not a screen the user navigates | **PC4** cycles the 6-colour palette; the on-screen swatch updates instantly so the active colour is always visible. |

**Non-pillars (explicitly).** Photographic fidelity, large palettes, file management, and image processing are **not** pillars of the MVP — they are Full-Release concerns (§8). The MVP optimises for *immediacy and persistence on constrained hardware*, not feature breadth.

---

## 3. Target User & Context

| Dimension | Detail |
|---|---|
| **Primary user** | A person holding the device — a maker, a kid, a demo-booth visitor — who wants instant doodling with no instructions. |
| **Secondary user** | **Me, the firmware engineer, and reviewers** — this PRD doubles as the design rationale and the contract for the console refactor. The audience for the *document* is an embedded hiring manager evaluating engineering judgement. |
| **Usage context** | Handheld, single-session, no save. Sessions are short ("draw something, clear it, draw again"). Power-cycling is the reset. |
| **Hardware context** | Single ST7735 panel, single joystick, three buttons, 3V3 backlight always on. No network, no storage, no OS. UART (USART2 @115200) is **debug-only** (retargeted `printf`), not a user surface. |
| **Skill assumed** | None. The control scheme is discoverable: move the stick, hold a button to draw, click to change colour. |

---

## 4. Core Loop

The MVP core loop is a single bare-metal `while(1)` in `main()` — no scheduler, no interrupts in the hot path, fully polled.

```
   ┌─────────────────────────────────────────────────────────────┐
   │  1. SAMPLE  joystick: 8 ADC samples/axis, average, deadzone  │
   │  2. MAP     raw → cursor position (centre-offset calibrated)  │
   │  3. MOVED?  |Δx|>5 or |Δy|>5  (movement threshold)            │
   │       ├─ NO  → idle                                          │
   │       └─ YES →                                               │
   │             PC5 held?                                        │
   │               ├─ YES → restore old area, PAINT new cell,     │
   │               │        redraw cursor  (DRAW mode)            │
   │               └─ NO  → restore old area, redraw cursor       │
   │                        (HOVER mode — move only)              │
   │  4. PA10 pressed? → CLEAR canvas (fill white, reset colour)  │
   │  5. PC4 pressed?  → CYCLE colour, repaint swatch             │
   └─────────────────────────────────────────────────────────────┘
                        (loop forever)
```

**Plain-language loop:**

1. **Move** the 10×10 cursor with the joystick.
2. **Hold PC5** to paint with the active colour as you move; **release PC5** to hover (move without leaving a trail).
3. **Press PC4** to cycle to the next colour (swatch updates).
4. **Press PA10** to clear the canvas to white and reset to the first colour.

There is **no win state, no score, no end** — by design. EmbeddedPaint is a *tool*, not a game with a loss condition. (Its sibling cartridge `spaceShipGame` carries the win/lose machinery; EmbeddedPaint deliberately does not.)

---

## 5. Mechanics

### 5.1 Palette — 6 fixed RGB565 colours

Defined in `paint_colors[]` (`main.c:48–55`). Verified against source:

| Index | Colour | RGB565 |
|---|---|---|
| 0 | Red | `0xF800` |
| 1 | Green | `0x07E0` |
| 2 | Blue | `0x001F` |
| 3 | Yellow | `0xFFE0` |
| 4 | Magenta | `0xF81F` |
| 5 | Cyan | `0x07FF` |

`PC4` increments `color_index` and wraps via `num_colors` (computed with `sizeof`, so the palette is a single-array edit to extend). The canvas background and the "eraser" are **white** (`0xFFFF`) — there is no distinct eraser tool; clearing or painting white is how you remove marks. *(A true white-eraser-vs-background distinction is a Full-Release item, §8.)*

### 5.2 Movement threshold

The cursor only repaints when `|Δx| > 5` **or** `|Δy| > 5` (`main.c:159–160`). This **debounces joystick jitter** so a stationary stick does not redraw or smear, and it bounds redraw work — a real concern given the per-pixel SPI cost (§7). Tradeoff: very fine sub-5-pixel positioning is not expressible; for a 128×160 canvas with a 10×10 cursor, that is an acceptable MVP resolution.

### 5.3 Cursor & paint cell

| Property | Value | Source |
|---|---|---|
| Cursor size | 10×10 px | `LCD_courser`, `size=10` |
| Cursor colour | **Always black (`0x0000`)** — the `color` argument is **ignored** | `LCD_courser` hard-codes black |
| Paint cell | 10×10 block written to the framebuffer | `saveColorToArray` double loop |
| Start position | centre, `(64, 80)` | globals `current_x/current_y` |

> **MVP note (PoC framing):** the cursor is intentionally a fixed black 10×10 reticle. A coloured/shaped cursor and variable brush size are Full-Release polish, not MVP correctness. The cursor reads as a clear "you are here" marker against any of the six saturated palette colours and white.

### 5.4 Swatch UI

The active-colour swatch lives at the address window **x = 80–95, y = 10–25** (`LCD_OnSetup_color_choice`). The cursor and paint are **guarded out of that rectangle** (`LCD_courser` / `LCD_Restore_Area` early-return when overlapping `x∈[80,95), y∈[10,30)`), so you cannot paint over the swatch and corrupt the UI.

> **Known minor inconsistency (documented, not hidden):** the swatch *fill loop* writes a **20×20** region (`for i < 20*20`) into a **16×16** address window (95−80=15, 25−10=15 → 16×16). The 16-bit ST7735 wrap clips the overspill to the panel; the swatch renders, but the loop count and window disagree. **Full-Release cleanup:** make the fill loop derive its count from `(x_end−x_start+1)×(y_end−y_start+1)`. This is exactly the class of bug a shared, parameterised display driver eliminates (§8).

### 5.5 Persistence — the 40 KB framebuffer

The canvas is `uint16_t frame_buffer[128][160]` = **40,960 bytes ≈ 40 KB** of the 128 KB RAM. This is the **defining mechanic**: the framebuffer, not the LCD, is the source of truth for the drawing.

- `paint()` → `saveColorToArray()` commits a 10×10 block of the active colour into the framebuffer.
- `frame_buffer_init()` resets every cell to white (`0xFFFF`) on boot and on clear.
- `LCD_Restore_Area()` reads the framebuffer to repaint the 10×10 patch *under the old cursor position* when the cursor moves — this is what makes painted pixels persistent and the cursor non-destructive.

**Engineering rationale & tradeoff.** A full off-MCU framebuffer is the textbook approach to a persistent canvas, and at 40 KB it fits comfortably in 128 KB RAM — but the **render path that consumes it is the MVP's biggest simplification** (§7). On the RP2350's 520 KB RAM, this 40 KB buffer is trivial, which is one reason the platform port (§8) is attractive.

---

## 6. Controls

All inputs verified against `MX_GPIO_Init` / `MX_ADC1_Init` and the loop body.

| Control | Pin | Type | Action |
|---|---|---|---|
| **Joystick X** | PA1 — ADC1_IN1 (rank 1) | Analog, 12-bit | Move cursor horizontally |
| **Joystick Y** | PB1 — ADC1_IN9 (rank 2) | Analog, 12-bit | Move cursor vertically |
| **Paint / Hover** | PC5 | GPIO in, pull-up, active-low | **Held** → paint; **released** → hover (move only) |
| **Cycle colour** | PC4 | GPIO in, pull-up, active-low | Press → next palette colour + swatch update (`HAL_Delay(200)` debounce) |
| **Clear / reset** | PA10 | GPIO in, pull-up, active-low | Fill white, reset to colour 0 (`HAL_Delay(300)` debounce) |

**Joystick acquisition (verified `main.c:126–151`).** Per loop: 8 iterations, each `HAL_ADC_Start` then **two** back-to-back `PollForConversion`+`GetValue` (relying on the scan sequencer ordering X then Y); sum and average. Apply a **deadzone of 10** around hand-calibrated centre offsets **x=2150, y=1900** (note: *not* the ideal 2048 — these were eyeballed on the bench to cancel this specific stick's bias). Map the deadzoned delta to a cursor coordinate in `[0,128)×[0,160)`.

> **Honest hardware caveats (documented, scheduled for fix):**
> - **Axis-label contradiction.** The ADC config comments say *X-axis on PA1 / Y-axis on PB1*; `pin_layout.txt` says *A1 = Y, PB1 = X*. **One is wrong.** The next-step fix is a single verified mapping pinned in a shared input service (§8) with a calibration routine, retiring the eyeballed offsets.
> - **Brittle two-reads-per-start pattern.** Doing two `GetValue`s after one `Start` leans on undocumented sequencer ordering; under timing skew the axes can swap. The robust fix is ADC-DMA scan into a 2-element buffer — a Full-Release item.
> - **`pin_layout.txt` is internally inconsistent and mislabels the bus.** It tags **PC5 as both `btnA` and `btnB`**, and labels the SPI bus as **I²C** (`sda = pa7`, `scl = pa5`) — but **PA7 is SPI1_MOSI and PA5 is SPI1_SCK**. The *code* is correct (SPI1); the note is stale. Pin documentation is regenerated as part of the shared-platform extraction.

---

## 7. Canvas / Rendering Model — **indirect render** (be explicit)

This is the most important section to read honestly, because it explains a behaviour that *looks* like a bug and is actually a scoped MVP design.

### How a stroke reaches the screen today

`paint()` **does not draw to the LCD.** It only writes the framebuffer (`paint → saveColorToArray`). Painted pixels become visible **only later**, when the cursor *moves off* that location and `LCD_Restore_Area()` repaints the vacated 10×10 patch from the framebuffer:

```
  Hold PC5 + move
        │
        ▼
  paint(new) ──► writes frame_buffer[][]   (NOT the LCD)
        │
        ▼
  LCD_courser(new) ──► draws BLACK cursor at new position (LCD)
        │
   ... next move ...
        │
        ▼
  LCD_Restore_Area(old) ──► reads frame_buffer[old], writes those
                            10×10 pixels to the LCD  ← stroke appears here
```

So the model is **indirect**: the framebuffer is updated immediately and correctly, but the *display* of a painted cell is deferred until the cursor leaves it.

### The consequence — and why it's framed, not denied

**A fast stroke can outrun the redraw.** If the cursor moves several cells before `LCD_Restore_Area` repaints the trail, the painted pixels are *in the framebuffer* (correct, persistent) but **not yet on screen**. Clearing and the next pass would reveal them; the canvas state is never wrong, only its on-screen presentation lags.

**Why it costs.** Both the cursor draw and the restore push pixels **one ST7735 SPI write at a time** via `HAL_SPI_Transmit(..., HAL_MAX_DELAY)` — and `LCD_Restore_Area` is worse: it sets a **fresh 1-pixel address window (`0x2A/0x2B/0x2C`) per pixel**, 100 times for a 10×10 patch, each with bit-banged CS/DC GPIO toggles. At **8 MHz SCK on a 16 MHz core with no DMA**, this is the throughput wall.

### The headline performance story (I profiled it)

| Lever today | Setting | Headroom |
|---|---|---|
| **Core clock** | HSI 16 MHz, **PLL disabled** (`RCC_PLL_NONE`), `FLASH_LATENCY_0` | Part is rated to **100 MHz** → running at **~16%** |
| **SPI SCK** | APB2 16 MHz ÷ 2 = **8 MHz** | Faster SCK + a faster APB2 once PLL is on |
| **Transfer mode** | Byte-at-a-time blocking `HAL_SPI_Transmit` | **No DMA** today |
| **Blit unit** | 1-pixel address window per pixel in restore | Window once, stream the patch |

> **This is the single biggest, cheapest win and a genuine "I found the bottleneck" story:** enable the PLL (→ up to 100 MHz core, faster APB2/SPI) and convert the pixel pushes to **DMA SPI** with a per-patch (not per-pixel) address window. The framebuffer already exists, so a DMA blit of dirty rectangles is a natural fit. **None of this is implemented today** — it is the top Full-Release performance item (§8). Stated plainly: today the design is *correct but slow and indirect*; the roadmap makes it *direct and fast* without changing the canvas model.

### Endianness note (cross-cutting, for the shared driver)

EmbeddedPaint writes pixel bytes **big-endian** (`{color>>8, color&0xFF}`) throughout. Its sibling `spaceShipGame` writes **little-endian** (`{color&0xFF, color>>8}`). The two copy-pasted drivers **disagree on byte order** — harmless within each game, but a landmine the moment they share a driver. Reconciling this into **one** ST7735 driver with a single defined byte order is a precondition of the console (§8).

---

## 8. Scope — MVP (today) vs Full-Release (adds)

### 8.1 MVP scope — what the code does **today** (verified)

- ✅ Boots, fills the 128×160 ST7735 white, shows the colour swatch.
- ✅ Reads a 2-axis joystick (averaged, deadzoned, centre-calibrated) and moves a 10×10 cursor.
- ✅ Hold **PC5** to paint with the active colour; release to hover.
- ✅ **PC4** cycles a fixed 6-colour RGB565 palette; swatch updates.
- ✅ **PA10** clears to white and resets to colour 0.
- ✅ Persistent **40 KB full-screen framebuffer** is the canvas source of truth.
- ✅ UI region (swatch) protected from paint/cursor overwrite.
- ✅ UART debug `printf` retarget (developer surface only).

### 8.2 Full-Release scope — what it **adds** (none of this exists today)

| # | Feature | What it adds | Closes which MVP gap |
|---|---|---|---|
| FR-1 | **Exit-to-shell cartridge contract** | Refactor from a monolithic firmware image into a **game module** with an `init()/update(input,dt)/render()/teardown()` lifecycle that returns control to a shell | No way to exit a game today; this is the central console enabler |
| FR-2 | **Shared ST7735 driver (DMA + dirty-rect)** | One driver, **one byte order**, DMA blits, per-patch address windows, framebuffer-aware dirty-rectangle flush | Indirect/per-pixel render, byte-at-a-time SPI, big-vs-little-endian split (§7) |
| FR-3 | **PLL → up to 100 MHz** | Enable PLL + correct flash latency; faster APB2/SPI | 16 MHz / 8 MHz today (~16% of ceiling) |
| FR-4 | **Direct paint render** | Write painted pixels straight to the LCD (not only the framebuffer) | "Fast stroke outruns the redraw" (§7) |
| FR-5 | **Undo / redo** | Stroke-history stack over the framebuffer | No undo today |
| FR-6 | **Save / load to flash** | Persist drawings to on-chip flash; reload after power-cycle | Power-cycle is the only reset; nothing survives it |
| FR-7 | **Flood-fill** | Bucket-fill a bounded region | Freehand-only today |
| FR-8 | **Variable brush size** | Selectable brush widths beyond the fixed 10×10 | Single fixed brush |
| FR-9 | **True white eraser** | A dedicated eraser distinct from "paint white" / background | No eraser tool today |
| FR-10 | **Larger palette / colour picker** | Beyond 6 colours; an RGB565 picker | 6 fixed colours |
| FR-11 | **Shared input service** | Debounced buttons + ADC-DMA joystick + one verified axis map + calibration | Eyeballed offsets, two-reads-per-start brittleness, axis-label contradiction (§6) |
| FR-12 | **CubeMX-drift guard** | Reconcile `.ioc` with hand-edits or move acquisition behind code the generator won't clobber | See §10 risk R-1 |
| FR-13 | **RP2350 / Pico 2 port** | HAL-swap the STM32 `HAL_*`/`MX_*` for pico-sdk (`hardware/spi`, `hardware/adc`, `hardware/gpio`, `hardware/dma`, `pico/time`) behind the shared platform layer | Single-vendor lock-in; unlocks PIO offload, dual M33 @150 MHz, 520 KB RAM |

### 8.3 Roadmap (recommended order)

```
 Phase 1 — De-risk the render path (no console yet)
   FR-3 PLL → 100 MHz        ┐ biggest cheapest win; profile before/after
   FR-2 shared DMA driver    ┘ unify byte order, per-patch windows
   FR-4 direct paint render    (removes the "outrun the redraw" surprise)
   FR-1 cartridge contract     extract lifecycle; EmbeddedPaint = first module

 Phase 2 — Make it a console
   Shell + game-select menu owns the HW; shared input/timing/RNG/persistence
   Snake (greenfield) built natively to the contract = reference cartridge
   spaceShipGame refactored to the same contract

 Phase 3 — Paint feature depth
   FR-5 undo · FR-6 save/load to flash · FR-9 eraser · FR-7 flood-fill
   FR-8 brush size · FR-10 larger palette / picker

 Phase 4 — Platform portability
   FR-13 RP2350 HAL-swap behind the platform abstraction (validates the layer)
```

**Decision-with-rationale:** Phase 1 is deliberately *before* the console refactor. The render path is the riskiest, most-measurable code; fixing throughput and unifying the driver **first** means the cartridge contract is extracted from clean, fast code rather than carrying the indirect/per-pixel design into three games. Profiling the PLL+DMA win is also the strongest portfolio artifact.

---

## 9. Success Metrics

| Metric | MVP target (today) | Full-Release target |
|---|---|---|
| **Time-to-first-stroke** | < 1 s from reset to a drawable cursor | unchanged |
| **Perceived draw latency** | Stroke visible within one cursor move (indirect) | **Immediate** — pixel-accurate live stroke (FR-4) |
| **Stroke throughput** | Limited by 8 MHz SPI + per-pixel windowing | ≥ 5–10× via PLL + DMA + per-patch blit (FR-2/3); **measured before/after** |
| **Canvas correctness** | Framebuffer never wrong; on-screen may lag | On-screen == framebuffer at all times |
| **Colour switch latency** | < 250 ms incl. swatch repaint | unchanged |
| **RAM headroom** | 40 KB / 128 KB used (~31%); fits | trivial on RP2350 (40 / 520 KB) |
| **Clock utilisation** | 16 MHz (~16% of 100 MHz) | up to 100 MHz (PLL) |
| **Console readiness** | N/A (monolith) | EmbeddedPaint runs as a cartridge and **exits cleanly to the shell** |

The headline engineering metric is the **profiled before/after of the PLL+DMA work** — a concrete "I found and removed the bottleneck" number.

---

## 10. Risks

| ID | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R-1** | **CubeMX `.ioc` drift clobbers hand-fixes.** The code was hand-edited (ADC `NbrOfConversion=2`, 144-cycle sample) but the `.ioc` still says `NbrOfConversionFlag=1` and 3-cycle sampling. A CubeMX regenerate would silently overwrite the working acquisition. | High (if anyone regens) | High (breaks joystick) | FR-12: reconcile `.ioc`, or keep ADC config in a `USER CODE` block / hand-owned file. **Do not regenerate without re-applying the deltas.** |
| **R-2** | **Axis mapping ambiguity** (code comment vs `pin_layout.txt` contradict). | Medium | Medium | FR-11 shared input service with one verified axis map + calibration; bench-confirm before locking. |
| **R-3** | **Indirect render confuses users/reviewers** ("my fast stroke didn't show"). | Medium | Low–Med | Documented here (§7); FR-4 makes render direct. Canvas state is never lost. |
| **R-4** | **Render throughput wall** (16 MHz / 8 MHz / no DMA / per-pixel windows) caps stroke speed. | High (it's real today) | Medium | FR-2/3/4; profile the win. This is the flagship perf item, not a surprise. |
| **R-5** | **Swatch fill loop / window size mismatch** (20×20 loop into 16×16 window). | Certain (cosmetic) | Low | Derive loop count from window bounds in the shared driver (§5.4). |
| **R-6** | **Two-reads-per-`Start` ADC pattern** can swap axes under timing skew. | Low–Med | Medium | FR-11: ADC-DMA scan into a 2-element buffer. |
| **R-7** | **Copy-pasted driver / endianness divergence** across games blocks the console. | Certain (architectural) | High (for console) | FR-1/FR-2: one shared driver with a single defined byte order before sharing code. |
| **R-8** | **Repo hygiene** — stray root `//test` `main.c`, duplicate `#include "main.h"`, declared-never-defined `loadColorToArray`, `paint(y,x)` prototype vs `paint(x,y)` definition, shadowed globals. | Certain (cosmetic) | Low | Delete the dead root `main.c`; de-dupe includes; drop the dead prototype; align proto/def arg names; remove shadowing during the FR-1 extraction. |

---

## 11. Out of Scope

Explicitly **not** in EmbeddedPaint, MVP **or** Full-Release, unless re-scoped:

- **Image import/export over UART or USB**, photo loading, or any host-side tooling. (USART2 stays debug-only.)
- **Anti-aliasing, alpha blending, gradients, layers** — RGB565 flat fills only.
- **Networking / wireless / multiplayer drawing.**
- **An on-screen keyboard or text tool.**
- **Touch input** — the only inputs are the joystick and three buttons.
- **Pressure / tilt** — the joystick is position-only.
- **A general windowing/GUI framework** — the "UI" is the single colour swatch.
- **3D or animation.**

Console-shell concerns (menu, dispatch, shared timing/RNG/persistence services, the `Snake` cartridge) are **owned by the console PRD**, not this one — EmbeddedPaint's responsibility is to become a well-behaved **cartridge** (FR-1) and to hand back control cleanly. Everything else about the shell lives upstream of this document.

---

### Appendix A — Hardware quick reference (verified against `main.c` / `.ioc`)

| Block | Detail |
|---|---|
| **MCU** | STM32F411RET6, Cortex-M4F, 512 KB flash / 128 KB RAM |
| **Clock** | HSI 16 MHz, **PLL OFF** (`RCC_PLL_NONE`), `FLASH_LATENCY_0` → SYSCLK = HCLK = APB1 = APB2 = **16 MHz** |
| **Display** | ST7735 128×160 RGB565; init `0x01`→`0x11`→`0x3A/0x05`→`0x21` (INVON)→`0x29` |
| **SPI1** | Master, 8-bit, MSB-first, mode 0 (CPOL=0 / CPHA=1edge), prescaler /2 → **8 MHz SCK**, software NSS, **no DMA**, byte-at-a-time |
| **SPI pins** | SCK=PA5, MISO=PA6 (unused, write-only panel), MOSI=PA7 |
| **Control pins** | DC=PB13, CS=PB14, RES=PB15 (bit-banged); backlight tied to 3V3 |
| **ADC1** | 12-bit, scan mode, software-triggered, polled, 144-cycle sample; IN1=PA1 (rank 1), IN9=PB1 (rank 2), `NbrOfConversion=2` |
| **Buttons** | PC5 = paint/hover, PC4 = cycle colour, PA10 = clear/reset (all GPIO pull-up, active-low) |
| **UART** | USART2 PA2/PA3 @115200, retargeted `printf` — **debug only** |
| **RAM use** | `frame_buffer[128][160]` `uint16_t` = **40,960 B ≈ 40 KB** of 128 KB |

*All hardware and code claims in this document were verified by reading `Core/Src/main.c` and `EmbeddedPaint.ioc` on 2026-06-27. Capabilities not present in the source — DMA, double-buffering, PLL/100 MHz operation, a game-exit path — are described **only** as Full-Release / roadmap items, never as current behaviour.*
