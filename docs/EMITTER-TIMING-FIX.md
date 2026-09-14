# App/emitter timing correction, 2026-09-13

The NVIDIA backend had two errors in translating phase to firmware commands.
These are software errors; this investigation used source, USB packet arithmetic,
session logs and the local RAM firmware. It did not investigate the TVs.

1. The FX2 firmware subtracts 214, 264 or 364 ticks from the negative X reload.
   Because that timer counts up to zero, this **adds** 53.5, 66 or 91 microseconds
   to the delay. The application previously subtracted that delay. Its phase
   estimate was therefore wrong by 107 to 182 microseconds. The shutter guard
   and conversion of old register-based profiles had the same sign error.
2. The boundary/X split sometimes schedules a token in the following refresh.
   The old code wrapped its time modulo one refresh but retained the original
   eye. The firmware assigns the commanded eye at the next boundary, so the
   backend must invert the command when that window belongs to the following
   refresh. Otherwise a phase sweep crosses into an inverted stereo pair.
   Independent durations now follow the physical eye in that command as well.

At 120 Hz and phase zero, the schedule's next token starts about 8333 us after
the command. That boundary must command the following image's lens. The matching
current-image token belongs to the preceding firmware boundary. This describes
steady-state timing; acquisition and live timing changes still have transients.

Two additional control defects were corrected. Leaving independent eye timing
now rewrites the common timing block, clearing the last per-eye registers. Eye
swap also forces acquisition instead of waiting for the firmware's mismatch
counter. A missed USB deadline disables free-running output and forces acquisition;
the presenter blanks/reacquires on late USB commands and insufficient prediction
lead instead of silently continuing to display stereo as locked.

The UI no longer presents calculated register intervals as measured lens windows.
IR pulse completion, lens response and USB command-receipt latency are not measured
by these calculations. Shutter Y is a register countdown, not an optical exposure
measurement: the firmware adds 784 ticks to its negative reload after the first
token, shortening that countdown by 196 us.

## Evidence and checks

Local firmware SHA-256:
`7348cfd799c0842be7e67dd8e72bab3cbbdbac7b00531c5d4088368afcddd643`.
Relevant instructions: phase correction at 0x164C/0x1689/0x16A9; next-boundary eye
selection at 0x03AB–0x03DC; X loading at 0x03E5; Y adjustment at 0x01D4–0x01F9;
configuration acquisition reset at 0x140D. The USB register block is based at
0x200F in this firmware; protocol offsets remain compatible with the upstream
[libnvstusb packet layout](https://github.com/eruffaldi/libnvstusb/blob/master/src/nvstusb.c).

New regression tests decode the actual outgoing packet bytes, apply the firmware's
signed timer arithmetic, and check time **and eye** across the stereo cycle.
They failed first on the old delay sign, then on lost eye parity after correcting
the sign. They also check both eye commands, shutter independence and sequence
guards across rates including 240 Hz. Build/test results and the pre-edit source
backup are in `reports/crosstalk-fix/`.

These checks establish the corrected software behavior. They do not establish
that all visible crosstalk is eliminated. No driver installation or firmware
binary modification was needed for these fixes.

## Expanded phase range

The NVIDIA phase control now spans the full stereo cycle (two image refreshes
for Left/Right, four for black-insertion or repeated sequences). Slider edits,
keyboard changes and profile reloads preserve that range. Timer reloads stay
bounded to one emitter period; command eye parity carries the additional period.
Regression tests decode the outgoing packets across both halves of the cycle,
verify that adding one emitter period changes eye phase, and verify that adding
a complete cycle repeats the timing within one 12 MHz timer tick.
