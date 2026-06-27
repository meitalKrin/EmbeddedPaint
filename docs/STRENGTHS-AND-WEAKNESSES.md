# EmbeddedPaint — Strengths & Weaknesses (Interview Talking Points)

> **Frame first.** EmbeddedPaint is the **paint cartridge** of a three-game STM32F411 console (alongside a space-dodge game and a planned Snake). It is a deliberately scoped **MVP / proof-of-concept**: a bare-metal MS-Paint clone that proves I can drive an unbuffered SPI panel, hold a full-screen canvas in RAM, and composite a moving cursor over it — *on a 16 MHz core with no DMA*. The interesting weaknesses below are not accidents I'm hiding; most are the exact seams the always-planned **shared platform layer** and **console shell** are designed to close. I name every gap and pair it with the fix.
>
> Throughout, I separate **what the code does TODAY** from **what Full-Release ADDS** — because conflating the two is how embedded demos lie.

---

## TL;DR for a hiring manager

- I hand-wrote the ST7735 driver, the ADC joystick sampling, and a 40 KB full-screen framebuffer persistence model on a Cortex-M4F running at **16 MHz with the PLL disabled and no DMA** — and I can tell you *exactly* why that's the headline bottleneck and what the fix is.
- The painting model is a **framebuffer-of-record + cursor-compositing-via-restore** design: paint mutates RAM, and the display is patched lazily under the moving cursor. That's a real rendering-architecture decision with a real, known failure mode (fast strokes can outrun the redraw).
- The honest weaknesses — indirect render, per-pixel restore, an endianness disagreement with the sibling game, the ignored cursor-colour argument, a proto/def arg-name swap, dead code, no undo/save — are each **PoC seams** that a named roadmap item closes. I'll walk you through the diff I'd write.

---

## Part 1 — Strengths

Each is something I built and can defend, with *why it impresses* a reviewer.

### 1. Hand-written ST7735 driver (no vendor graphics library)
I bring up the panel from raw command bytes: hardware reset via `RES` (PB15), `0x01` software reset, `0x11` sleep-out, `0x3A`/`0x05` to select 16-bit RGB565, `0x21` display-inversion-on, `0x29` display-on — then every pixel is an address-window (`0x2A` column / `0x2B` row / `0x2C` RAM-write) followed by raw SPI byte pairs.

**Why it impresses:** it shows I understand the panel at the datasheet level — command vs. data framing, the column/row/RAM-write windowing protocol, and the init handshake — rather than `#include`-ing a library and hoping. I also chose to bit-bang `CS` (PB14) and `DC` (PB13) as plain GPIO with software NSS (`SPI_NSS_SOFT`), so I own the chip-select timing around every transfer. That is exactly the level of control you need before you can add DMA.

### 2. RGB565 colour pipeline and a 6-colour palette
The panel is configured for 16bpp (`0x3A` → `0x05`) and the app carries a packed palette: red `0xF800`, green `0x07E0`, blue `0x001F`, yellow `0xFFE0`, magenta `0xF81F`, cyan `0x07FF`. `PC4` cycles through it.

**Why it impresses:** I'm comfortable with the RGB565 bit layout (5-6-5) and can hand-pick colour constants for the format. The palette is a clean, fixed-size array with a `num_colors` computed from `sizeof`, so adding a colour is a one-line change — a small but real sign of writing for extension.

### 3. Full-screen 40 KB framebuffer persistence model
`uint16_t frame_buffer[128][160]` is a complete shadow copy of the panel — `128 × 160 × 2 = 40 KB` of RAM — and it is the **source of truth** for the canvas. `frame_buffer_init()` clears it to white; `saveColorToArray()` writes painted blocks into it.

**Why it impresses:** I made a deliberate **memory-vs-bandwidth trade**. On a panel with no read-back path over a write-only SPI link, you *cannot* ask the display "what colour is this pixel?" — so to restore what's under a moving cursor you must remember it yourself. Spending 40 KB to get O(1) "what was here before" is the right call, and I sized it against the budget: 40 KB of 128 KB SRAM (~31%) is affordable today, and on the RP2350 target (520 KB) it's a rounding error. This is the artifact that makes persistent drawing possible at all.

### 4. Dirty restore-area redraw concept
Instead of repainting the whole screen every frame (which at 16 MHz, no-DMA would be visibly slow), I only touch the **10×10 region the cursor just left**: `LCD_Restore_Area(old_x, old_y)` repaints that block from the framebuffer, then the cursor is drawn at the new position.

**Why it impresses:** this is dirty-rectangle rendering in miniature — I understood that the expensive resource is *SPI bandwidth*, not compute, and I localised redraws to the changed region. A full-frame blit is ~20 480 pixel writes; a cursor move is ~100. That's the correct instinct, and it's the conceptual seed of the dirty-rect/double-buffer engine the shared platform layer is slated to add.

### 5. Button state machine: paint / hover / clear / cycle
Three buttons drive distinct modes, all active-low with internal pull-ups (`GPIO_PULLUP`, pressed reads `RESET`):
- **`PC5` held** → paint with the current colour; **released** → hover (move the cursor without drawing).
- **`PC4`** → cycle to the next palette colour (`LCD_color_choice()`), with a 200 ms debounce delay.
- **`PA10`** → clear the canvas, reset to colour index 0, re-init the framebuffer, redraw the swatch (300 ms debounce).

**Why it impresses:** the **held-vs-released** distinction on `PC5` is the core UX insight — it's literally "pen down / pen up," which is what makes freehand drawing feel natural instead of toggle-to-draw. I read it as a *level*, not an edge, every loop, so holding continuously paints. The debounce is crude (`HAL_Delay`) but intentional and acknowledged below.

### 6. Cursor compositing over a persistent canvas
Every loop the cursor is composited *on top of* the canvas without destroying it: restore the 10×10 under the old position from the framebuffer, then draw the cursor at the new position. The canvas underneath is never lost because it lives in RAM, not on the (unreadable) panel.

**Why it impresses:** this is a real **two-layer composite** — a persistent background layer (the framebuffer) and a transient foreground sprite (the cursor) — implemented without a GPU, without page-flipping, and without a read path from the display. Separating "what's permanently drawn" from "what's just hovering" is the foundational idea behind every sprite-over-tilemap renderer, done here from first principles.

### 7. Swatch UI region masking
The current-colour swatch lives in a fixed screen region (`x` 80–95, `y` 10–25). Both `LCD_courser()` and `LCD_Restore_Area()` early-return if the cursor overlaps that rectangle, so the cursor never paints over or erases the UI chrome.

**Why it impresses:** I carved out a **reserved HUD region** and guarded it at the draw level — a clean separation of "interactive canvas" from "status UI." It's the same pattern as a game reserving the top bar for score: a single overlap test keeps the two worlds from corrupting each other.

### 8. ADC joystick sampling with averaging, deadzone, and calibration
The 2-axis analog stick is read on ADC1 (12-bit, scan mode, software-triggered, 144-cycle sample) across two channels (IN1/PA1 rank 1, IN9/PB1 rank 2). Per loop I take **8 samples per axis and average them**, subtract **hand-measured centre offsets** (`x=2150`, `y=1900` — *not* the textbook 2048, because I measured the actual rest position on the bench), and apply a **deadzone** before mapping to cursor delta.

**Why it impresses:** averaging to beat ADC noise, a deadzone to kill rest-jitter, and *empirically calibrated* centre offsets are exactly the three things a real analog input needs. The `2150/1900` constants are the tell that I calibrated against hardware rather than trusting the ideal midpoint — that's bench discipline, not theory.

---

## Part 2 — Weaknesses (honest, each with fix + what it shows + PoC frame)

I lead with the failure mode, not a euphemism. Every item is paired with the fix I'd ship and the roadmap feature it belongs to.

### A. Indirect render: fast strokes can outrun the redraw — *core-loop seam*
**What happens today:** `paint()` only mutates the framebuffer (`saveColorToArray`). A painted pixel reaches the **panel** only later, when the cursor moves *off* it and `LCD_Restore_Area` repaints that block from RAM. So the canvas-of-record is always correct, but a fast stroke can paint into RAM faster than the screen catches up — pixels appear a beat late, under the trailing edge of the cursor.

**Fix:** make `paint()` write-through — push the painted block to the panel immediately *and* to the framebuffer, decoupling "commit to canvas" from "uncover via cursor motion." With DMA (below) the immediate blit is nearly free.

**What it shows:** I can reason about the difference between a *model* (framebuffer) and its *view* (panel) and spot where they desynchronise — a model/view-coherence bug, the most interesting kind.

**PoC frame:** today's lazy, cursor-driven redraw was the *minimum* needed to prove the persistence-and-composite model on a 16 MHz, no-DMA core. Write-through rendering is a named Full-Release item that lands with the shared display driver — not a defect, a deferred optimisation.

### B. Per-pixel windowed restore is slow — no DMA, no block blit — *the bandwidth bottleneck*
**What happens today:** `LCD_Restore_Area` restores its 10×10 block by, for **each of the 100 pixels**, sending a full `0x2A`/`0x2B` 1-pixel address window and a single 2-byte `HAL_SPI_Transmit` (blocking, `HAL_MAX_DELAY`). That's ~100 command/data round-trips with bit-banged CS/DC toggling per pixel — the most expensive possible way to move a rectangle.

**Fix:** set the address window **once** for the whole 10×10 block and stream all 200 bytes in a single transfer; then move to **DMA** (`HAL_SPI_Transmit_DMA`) so the CPU isn't babysitting each byte. Block-window + DMA together turn 100 round-trips into one.

**What it shows:** I can spot that the cost is in the *per-pixel windowing overhead*, not the pixel count — and that the fix is a protocol change (window once, stream many) before it's a hardware feature (DMA).

**PoC frame:** per-pixel restore was the simplest correct implementation to prove the dirty-rect concept. Block-streaming and DMA are explicitly the shared-driver's job — *I do not claim DMA exists today; it does not.*

### C. Endianness disagreement with the sibling game — copy-paste divergence
**What happens today:** EmbeddedPaint writes RGB565 **big-endian** — `{color >> 8, color & 0xFF}` (high byte first) — in fill, paint, swatch, and restore. The sibling space-dodge game writes the **opposite** byte order in its copy-pasted driver. Each game was tuned on-device to look right *with its own ordering* (both also set `0x21` inversion-on), so each works in isolation — but the two drivers are not interchangeable.

**Fix:** extract **one** ST7735 driver with a single, documented byte order and a named colour type, and delete both copies. Tune inversion/endianness *once*, centrally.

**What it shows:** I recognise copy-paste driver duplication as a latent integration bug, and I know the cure is a shared abstraction, not careful parallel editing.

**PoC frame:** the two games being independent firmware images with their own drivers is *the* central PoC limitation of the console — and exactly what the always-planned **shared platform/HAL layer** exists to fix. The endianness split is the concrete evidence that the consolidation is needed, not a mystery.

### D. 16 MHz core, PLL disabled, no DMA — *the headline performance story*
**What happens today (verified in `SystemClock_Config`):** the clock tree runs on **HSI 16 MHz with `RCC_PLL_NONE` and `FLASH_LATENCY_0`**, so `SYSCLK = HCLK = APB1 = APB2 = 16 MHz` — about **16% of the F411's 100 MHz ceiling**. SPI1 runs `/2` of the 16 MHz APB2 = **8 MHz SCK**, and every transfer is a **blocking** byte-pair with bit-banged CS/DC. There is **no DMA and no double-buffering anywhere in the codebase today.**

**Fix (and it's the single biggest, easiest win):** enable the PLL to take SYSCLK to ~100 MHz (with the matching flash wait-states and voltage scaling), which immediately allows a faster SPI prescaler, then add DMA-driven SPI so the CPU stops spinning on each byte. PLL + DMA together would multiply effective draw throughput by a large factor with almost no architectural change.

**What it shows:** I profiled the clock tree, found the bottleneck, quantified it (16 of 100 MHz; 8 MHz SPI; blocking transfers), and can name the exact fix and its ordering. "I measured where the time goes" beats "I think it's slow."

**PoC frame:** running at HSI with the PLL off is a *legitimate* MVP choice — it's the simplest clock config that boots and works, with no PLL-lock or flash-latency tuning to get wrong. **100 MHz operation and DMA are Full-Release / roadmap items; neither exists today.** I'm framing the gap, not denying it.

### E. Cursor colour argument is ignored — dead parameter
**What happens today:** `LCD_courser(uint8_t x, uint8_t y, uint16_t color)` takes a `color` argument but **ignores it** — the body hard-codes `black = 0x0000`. The cursor is always black regardless of what the caller passes (and callers do pass `0`).

**Fix:** either honour the argument (draw the cursor in `color`, e.g. the current paint colour or its inverse for contrast) or remove the parameter so the signature stops lying. I'd make the cursor draw in the active paint colour so it previews what a click would lay down.

**What it shows:** attention to the gap between an API's *signature* and its *behaviour* — a dead parameter is a small honesty bug, and I'd rather fix the contract than leave it misleading.

**PoC frame:** a fixed black cursor was enough to prove compositing; a colour-aware cursor is a UX polish item, cheap and deferred — not load-bearing for the MVP.

### F. Prototype/definition argument-name swap — `paint(y,x)` vs `paint(x,y)`
**What happens today:** the prototype declares `void paint(uint8_t y, uint8_t x, ...)` while the definition is `void paint(uint8_t x, uint8_t y, ...)` (and `saveColorToArray` has the same swap). It **compiles and runs correctly** — the types match and arguments bind by position — but the *names* are transposed, so a reader can't trust which is which.

**Fix:** make the prototype and definition agree on `(x, y)` everywhere; it's a pure-clarity, zero-behaviour edit. Compile with `-Wall -Wextra` and treat such mismatches as review blockers.

**What it shows:** I read signatures critically and care that names tell the truth even when the compiler doesn't complain — exactly the kind of latent confusion that bites the *next* engineer.

**PoC frame:** harmless today, but it's the sort of thing the shared-driver refactor cleans up by construction when the function is rewritten once against a clear contract.

### G. Stray root `main.c` + double `#include` + dead prototype — cleanup debt
**What happens today:** three small hygiene issues coexist with working code:
- A root-level `/main.c` containing only `//test` — **dead**; the real entry point is `Core/Src/main.c`. It should be deleted before it confuses a build or a reader.
- `#include "main.h"` appears **twice** at the top of `Core/Src/main.c` (harmless thanks to the header guard, but noise).
- `loadColorToArray()` is **declared but never defined** — a dead prototype. (Several globals, e.g. `current_x/current_y/old_x/old_y`, are also shadowed by locals in `main()`.)

**Fix:** delete the stray file and the dead prototype, drop the duplicate include, and remove the shadowing globals (or use them). A linter / `-Wshadow` pass plus a pre-commit check would catch all of these.

**What it shows:** I can inventory cleanup debt precisely and distinguish *harmless-but-untidy* from *actually broken* — and I'd rather call it out than pretend the tree is pristine.

**PoC frame:** prototype clutter is expected in a fast PoC; it's swept up in the consolidation pass, and none of it affects the running demo.

### H. No undo, no save/load — missing canvas features
**What happens today:** the canvas is a single mutable framebuffer. `PA10` clears it; there is **no undo, no save/load, no flood-fill, no brush-size control, no true white-eraser, and no larger palette/colour-picker.** A clear is irreversible and nothing survives a power cycle.

**Fix:** undo needs a small ring of framebuffer snapshots or a stroke log; save/load needs a **persistence service** writing the framebuffer to on-chip flash. Both belong to the shared platform's persistence layer (the same one that would store the other games' high scores).

**What it shows:** I can scope a feature set honestly and map each missing capability to the shared service that would provide it, rather than bolting one-off storage into the app.

**PoC frame:** these are explicitly **Full-Release** features. The MVP proves *drawing*; persistence and undo are named roadmap items riding on the console's shared persistence service — not silently absent, deliberately deferred.

### I. `.ioc` drift — generated config no longer matches hand-edited code
**What happens today:** the running code was **hand-edited** after CubeMX generation — ADC `NbrOfConversion = 2` and `ADC_SAMPLETIME_144CYCLES` — but the `.ioc` still says `NbrOfConversionFlag = 1` and `SamplingTime = 3CYCLES`. The code is correct; the *generator's source of truth* is stale. A naive CubeMX regenerate would **clobber the hand-fixes** and silently break the second ADC channel and the sample timing.

**Fix:** reconcile the `.ioc` to match the working code (set the conversion count and sample time in CubeMX, regenerate once, diff), so generate-from-`.ioc` is safe again. Longer term, keep peripheral init in `USER CODE` regions or a hand-owned module that regeneration won't touch.

**What it shows:** I understand the **generated-vs-handwritten drift trap** that bites every CubeMX project, can spot it by diffing the `.ioc` against the code, and know the regenerate would be destructive *before* running it. That's real embedded-workflow awareness.

**PoC frame:** hand-patching past the generator to get the demo working is normal PoC velocity; the reconciliation is a known, named cleanup step — flagged, not hidden.

> **Acknowledged-but-out-of-scope (sibling game / shared spine):** the unbounded-difficulty and unseeded-`rand()` issues, the score-as-survival-tick design, and the monotonic heap growth are in the *space-dodge* cartridge, not EmbeddedPaint. They share the diagnosis here — they're the reason the shared spine adds **one seeded RNG** and a **correct node lifecycle** (which Snake will implement as the reference). I mention them only to show the console-wide pattern; none of them are EmbeddedPaint bugs.

---

## Part 3 — What I'd do next (the roadmap, ordered)

Ordered by **value ÷ effort** — the cheap, high-leverage wins first:

| # | Change | Effort | Payoff |
|---|--------|--------|--------|
| 1 | **Enable PLL → ~100 MHz** (+ flash wait-states, voltage scaling) and raise the SPI prescaler | Low | Largest single throughput win; unlocks everything below |
| 2 | **Block-window the restore** (set window once, stream the 10×10) | Low | Kills the per-pixel round-trip overhead immediately, no DMA needed |
| 3 | **DMA-driven SPI** (`HAL_SPI_Transmit_DMA`) | Medium | CPU stops spinning on each byte; frees the core for game logic |
| 4 | **Write-through `paint()`** (commit to panel + framebuffer together) | Low | Fixes the fast-stroke lag (weakness A) |
| 5 | **Reconcile the `.ioc`** to the hand-edited ADC config | Low | Makes regenerate safe; removes a latent footgun |
| 6 | **Hygiene sweep** (delete root `main.c`, dead prototype, dup include; fix `paint` arg names; honour or drop the cursor `color` arg) | Low | Removes the misleading-contract debt; `-Wall -Wextra -Wshadow` clean |
| 7 | **Extract one shared ST7735 driver** with a single documented endianness + a colour type | Medium | Resolves the copy-paste divergence (C); the keystone of the console |
| 8 | **Cartridge lifecycle contract** (`init / update(input, dt) / render / teardown`) + a **shell/menu** that owns the hardware and dispatches to a game and back | High | Turns three standalone firmware images into a real 3-game console — *and the missing game-exit path; there is none today* |
| 9 | **Persistence service** (flash) → save/load drawings + high scores; then **undo**, brush size, eraser, colour picker | Medium | Closes the missing-feature set (H) on shared infrastructure |
| 10 | **HAL-swap to RP2350 / Pico 2** behind the shared platform abstraction (pico-sdk `spi`/`adc`/`gpio`/`dma`/`time`, PIO for display offload) | High | Dual M33 @150 MHz, PIO, 520 KB RAM — the 40 KB framebuffer fits trivially; the platform layer is what makes the swap a port, not a rewrite |

---

## How to present this in an interview

- **Lead with the architecture decision, not the demo.** "I built a framebuffer-of-record and composite a cursor over it via lazy dirty-rect restore" is a stronger opener than "it's a paint app." It signals you think in models and views.
- **Volunteer the bottleneck before you're asked.** Say up front: "It runs at 16 MHz with the PLL off and no DMA — that's deliberate for the MVP, and PLL + DMA is my number-one next change." Naming your own headline weakness, with the fix and a rough magnitude, reads as senior, not defensive.
- **Use the PoC frame deliberately, never as an excuse.** Each gap is "MVP-minimal here → named Full-Release item closes it." The indirect render, per-pixel restore, endianness split, and missing shell are *seams the shared platform layer is designed for*, not things you forgot. Honesty about the gap + a concrete fix beats a flawless-looking demo with no roadmap.
- **Separate today from tomorrow out loud.** Be explicit: "Today the code has no DMA, no double-buffer, no 100 MHz, and no game-exit path. Those are roadmap." Interviewers trust the engineer who won't claim a capability the code lacks.
- **Have the `.ioc` drift story ready** — it's a great "I understand the real toolchain" anecdote: hand-edited config diverged from CubeMX, a naive regenerate would clobber the fix, here's how I'd reconcile it.
- **Land on the console vision.** The three cartridges + shell + shared HAL, and the RP2350 port as a HAL-swap behind that abstraction, shows you see past one app to a platform — and that the abstraction layer was *always* the plan, with EmbeddedPaint as the cartridge that proves persistence and compositing.
