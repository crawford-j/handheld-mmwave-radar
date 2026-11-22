# Development Log

A chronological record of how the firmware was developed: what was built,
what was tested on hardware, what went wrong and how it was resolved. The
technical detail behind each decision (calculations, datasheet references,
measurements) is in [DESIGN_NOTES.md](../DESIGN_NOTES.md). The commit
history on GitHub has the step-by-step changes.

## Process

- **Milestones:** the firmware was built in seven milestones. Each ended
  with a flash to the real board, a hardware test by the team, and the
  results reported back before moving on.
- **Datasheets first:** the radar, display, touch controller and MCU
  datasheets were collected at the start. Register settings, command bytes
  and timings in the code cite the section or table they came from.
- **Small commits:** each change was compiled with `-Wall -Wextra` (zero
  warnings), size-checked and committed separately, using
  `type: short message` messages.
- **Design notes:** decisions, measurements and dead ends were logged in
  DESIGN_NOTES.md as they happened.
- **Upload procedure:** the radar's TX wire shares D0 with the Uno's USB
  serial link, so it was unplugged for every upload and plugged back in for
  testing.

## 2026-11-21

**Setup and planning**
- Created the public GitHub repo and Makefile build, using the Microchip
  Studio avr-gcc toolchain and the Arduino IDE's avrdude.
- Downloaded the datasheets. The LD2450 protocol (V1.03) came from Hi-Link's
  shared folder.
- Checked the radar protocol against the brief. Everything matched except
  one point: the 4th field per target is the distance-gate size, not the
  distance, so distance is computed as √(x²+y²).
- Wrote the module plan, timer map and SRAM budget (docs/PLAN.md), and got
  it approved.

**Milestone 1: display**
- Wrote the ILI9341 driver from the datasheet, using a minimal init
  sequence.
- First flash: the image was upside down. Fixed MADCTL from 0x28 to 0xE8.
- Measured the full-screen fill at 537 ms. This confirmed the no-framebuffer,
  partial-redraw design.

**Milestone 2: radar data**
- First test: no data arrived at all. Cause: radar wiring. Rewired.
- At 250000 baud (−2.34 % from the radar's 256000): 1000 frames with zero
  errors, at 11 Hz.
- Wrote a one-time setup tool and switched the radar to 38400, then 57600.
  The link stayed error-free, but the frame rate dropped to ~5 Hz at both
  speeds. Switching back to 256000 restored 11 Hz. Finding: the LD2450 only
  sends at its full rate at the factory baud; the protocol document doesn't
  mention this.
- Decision: run at 250000 for 11 Hz, relying on the measured zero-error link
  and the parser rejecting bad frames.

**Milestone 3: radar view**
- Built the fan view, rings, sweep line and pings. The background is computed
  per pixel, so moving objects can be erased without a framebuffer.
- Sideways tracking looked wrong: the dot stayed near the center. A raw-vs-
  filtered readout showed RAW X hovering around −300 mm and not following the
  walker.
  - Root cause: the radar was mounted landscape. The manual (Fig. 6) shows the
    module vertical; it measures azimuth across its short side.
  - Fix: the module was rotated 90°. Sideways tracking then worked.
- Initial tests on the family dogs gave poor results. Small, low, erratic
  targets are a hard case, so later tests used a person.

**Milestone 4: smoothing and arrows**
- First filter: an adaptive EMA, plus confirm/hold ghost rejection, a jump
  reset and gliding pings.
- Testing showed the filtered position lagging the raw one during slow turns.
  Replaced the EMA with an alpha-beta (position + velocity) filter. In
  simulation the lag on a ramp fell from 90 mm to 2 mm.
- Added direction arrows from the last ~0.36 s of motion.

**Milestone 5: alert zone and buzzer**
- Timer2 generates the 2 kHz tone. The alert has a radius, hysteresis, and a
  beep rate that speeds up with distance.
- Tuning with the team: the zone moved 1.5 m → 2 m → 1.5 m. A solid tone was
  added under 1 m.
- UI redesigned after a reference picture: a filled dome, the ±60° coverage
  cone and an expanding pulse.
  - The pulse was choppy near the edge. Fixes: draw only inside the cone, run
    the pulse speed off the clock, inline SPI writes, batch ring pixels into
    runs, draw the new ring before erasing the old one, and a 30 ms
    animation tick.
- Added a demo build with synthetic targets, so the UI could be tuned with
  the radar disconnected.
- Tried software screen dimming. It didn't help, because the backlight is
  hard-wired to 5 V. Removed.
- False triggers while setting the device up. Fixes: an arming delay, an
  alarm entry debounce, and a stricter confirmation for targets under 1 m.

**Handheld mode (attempted)**
- Holding the device produced ghosts all over: the radar can't separate its
  own motion from the room's.
- Discussed adding an MPU-6050 gyro. Instead, a firmware-only handheld mode
  was tried: stricter filtering, a "device moving" check, and a 0.8 m minimum
  range.
- It worked partially, but each test found new ghost cases.

## 2026-11-22

**Reorientation**
- Removed handheld mode to keep the design reliable and explainable. It's
  documented as a limitation, with the gyro as future work.
- The repo was renamed to `mmwave-presence-radar` to match the proposal's
  "presence radar" use case.
- Button fixed: fast presses were being missed. It's now debounced in the
  1 ms timer interrupt.
- The button re-arms the alarm with an 8 s countdown. The status line shows
  ARMING n / ARMED / ALERT.
- The team decided to add complexity through the touch screen (stylus menu,
  themes) instead of more sensor heuristics. This responds to the proposal
  feedback asking for more functionality.

**Milestone 6: touch**
- Wrote the XPT2046 driver from the datasheet.
- Wrote a self-contained calibration tool. Result on this panel: axes
  swapped, X 320–3906, Y 234–3825. The remaining small offset is likely the
  screen protector.
- Built the settings menu: units, zone radius, mute, scan style (pulse or
  sweep line, for the brief's "rotating sweep line"), debug, and themes, with
  3 presets plus a custom primary/secondary/tertiary palette.
- Feedback: added a mute icon on the radar screen.

**Milestone 7: EEPROM**
- Register-level EEPROM driver. Settings are stored with a magic byte,
  version and checksum, and fall back to defaults when invalid.
- Verified on hardware: DEFAULTS on the first boot, SAVED after changing
  settings, LOADED with all settings restored after a power cycle.

**Ongoing hardware observation**
- Uploads often fail verification on the first pass, always at byte 47 of a
  128-byte flash page, and succeed on retry. The suspect is the level
  shifter loading D1. `make flash` retries automatically. Still to test:
  unplugging the D1 wire during uploads.

**State at end of day:** all milestones complete. 21 KB flash, 716 B SRAM,
no warnings. Next: a stable stand for the device (team), a Microchip Studio
project, and the design report.

**Stand and usage changes**
- The team built a stand, so the device can be placed where it's needed
  instead of propped up.
- Feedback from using it on the stand:
  - Pings during the arming countdown could be misread, so pings are now
    hidden while ARMING. The tracker keeps running underneath.
  - A power button was requested. A true power-off isn't possible from
    firmware, because the radar and backlight are wired straight to the
    supply. Added standby instead: hold the button 1.5 s to blank the
    screen and disable the alarm, then press it (or tap the screen) to wake
    and arm. A switch on the battery lead is the real off switch.
  - Tracking through glass and thin walls is noisier and less accurate
    than in open air. This is expected: walls and glass weaken and reflect
    24 GHz, and coated windows block it almost completely. It's documented
    as a limitation, not fixed in firmware.
- Hardware test: pings hidden while arming, and standby / wake by
  button and by tap, all worked as intended.
- Follow-up requests from that test:
  - Boot into standby instead of a 5 s arming countdown, so the device is
    armed only once it's placed and the button is pressed.
  - Mute moved out of the menu: tapping the speaker icon on the radar view
    toggles it, and it's saved right away.
- Hardware test: standby boot and the mute icon both work.
- The debug line then showed BAD 2 and OVF 1486. OVF counts bytes the
  USART interrupt had to drop because the 128-byte ring buffer was full.
  1486 bytes is about 50 frames, or 4.5 s of radar data, in total.
  - Cause: full-screen redraws (standby screen, radar view on wake, menu
    pages) take 0.5 to 1.5 s, and the main loop doesn't read the radar
    while drawing. The buffer holds only ~0.4 s, so each redraw drops
    data. BAD goes up only when a gap happens to cut a frame between its
    header and tail.
  - Before standby, the first redraw happened before the radar UART was
    enabled, so the counters stayed at 0 until the menu was used.
  - Impact: none visible. Data is only lost while the screen is changing
    and nothing is shown, the parser resyncs on the next header, and
    tracking is back within ~0.2 s (2 confirm frames). Judged harmless
    and left as is.
- Team request: a GIF on the standby screen. The AVR can't decode GIF
  (LZW needs ~12 KB of RAM), so a PC-side tool (`tools/gen_anim.py`)
  converts a GIF into 1-bit 64 x 64 frames in flash, drawn at 2x. A
  default radar-scope animation is built in. Flash use went from 21.6 KB
  to 28.1 KB (86 %).
- Bug found by the team: with the radar TX unplugged and the device
  facing a wall, a ping stayed on screen at 13.6 ft (HZ 0, frame count
  frozen, RAW = FLT). It was the last target from before the wire came
  out. Confirm/hold count frames, so nothing aged out without frames, and
  an active alarm would never have stopped. Fixed with a 1 s radar-lost
  timeout: targets are dropped, the alarm clears and NO RADAR shows until
  frames come back.
- Standby GIF: the team picked two NBA clips. 1-bit previews of crops,
  thresholds and dithering were compared; heat.gif's close-up (9 frames,
  80 x 62 px) was chosen. The clips are copyrighted, so they and the frames
  made from them stay out of the public repo (`--local`, git-ignored); the
  published build keeps the radar-scope animation.
