# Emitter recovery changes, 2026-10-01

Update 2026-10-06: the one/two-frame presentation policy described below was
reverted after the captured-game session recorded severe missed image/black
slots, including while capture was idle. Both emitters again use the
refresh-scaled 25 ms presentation reserve (six frames at 240 Hz). The short
one-frame checks below did not establish sustained captured-game stability.

The preceding RP2040 session recorded presentation misses and black frames on
image refreshes despite zero USB errors and zero rejected device commands. The
preserved evidence is `reports/session-before-stability-20261001.log`. A USB
success counter does not establish stable stereo.

Changes in the application:

- Reduce the RP2040 stereo queue to one pending frame. The first two-frame queue
  gameplay check still recorded 54 misses and 28 black-on-image frames. A
  subsequent one-frame OLED diagnostic recorded 10,746 presents with zero
  detected misses, but its source stopped updating and Dolphin closed before
  the comparison completed. This is evidence about presentation, not a
  sustained gameplay pass. Keep two frames for the official emitter: its
  one-frame 60-second hardware test produced nine presentation misses and six
  late commands, so it needs the additional host USB lead.
- Choose the next future physical refresh after a producer stall before
  selecting the image eye and IR deadline. Record that same prediction for
  presentation feedback.
- On the official NVIDIA emitter, reacquire its firmware phase/eye lock after
  a repeated eye or missing command interval. Rewrite the timing block without
  disabling the emitter. Previously, locked firmware could need twenty
  mismatched commands to correct eye order.
- Serialize suspend with command submission, queue clearing and generation
  changes so a concurrent submission cannot undo suspension halfway through.
- Treat RP2040 queue-full and scheduling-horizon rejections as missed events,
  preserving the active session and already accepted opens/closes.
- Restart the uninterrupted ten-minute timing measurement on every detected
  miss. Remaining connected for ten minutes with intermittent misses no longer
  earns a timing pass.
- Give USB workers critical MMCSS priority and the presenter high MMCSS
  priority. Wake the official emitter worker 2 ms before its deadline, rather
  than 0.7 ms; the prior reserve still missed deadlines in a two-minute run.
- Keep the preview's three-frame reserve separate from optical output. Its
  one-frame queue froze after a resize in the windowed-preview regression;
  restoring three frames made that check pass.

Regression coverage includes ordinary timing jitter, repeated eyes, skipped
intervals, producer stalls at 60/119.88/120/240 Hz, and RP2040 transient
rejections. All 17 CTest cases passed; Windows capture integration required
execution outside the sandbox.

The connected board reports `VRP1/0.4.1/3ee935a2684af844`, serial
`53032847387B8B9C`. Its read-only clock check passed with 197/200 usable samples
and 145.81 us final estimated uncertainty. This change does not require a new
UF2. The official emitter was connected later and its existing RAM firmware
loaded successfully, reaching the supported `0955:0007` runtime identity.

With the final priority/wake settings, the official emitter's 60-second live
test recorded 14,315 presents, 7,175 commands, zero detected misses, zero late
commands, zero USB errors and zero resynchronizations. It used the saved
239.991 Hz OLED BFI/HDR profile, phase 2380.354 us and duration 4980.146 us.
See `reports/emitter-stability-official-live-20261001.txt`. The preceding
two-minute run, before the priority/wake correction, recorded 21 detected
misses and seven late commands; it is retained as
`reports/emitter-stability-official-before-priority-20261001.txt`.

The updated executable completed a 60-second live hardware run on the Odyssey
G80SD at 239.991 Hz, HDR, Left/Black/Right/Black, phase 3770 us and duration
3000 us. It recorded 14,320 presents, 7,175 eye commands, zero detected misses,
zero late commands, zero USB errors and zero resynchronizations. Post-run
diagnostics reported zero rejected device commands and no device fault. The
report is `reports/emitter-stability-live-20261001.txt`. That first RP2040 test
used two pending frames. This short test uses
the built-in target and does not establish sustained game or optical stability.

The final RP2040 one-frame queue and priority check also completed 60 seconds
with zero detected misses, late commands, USB errors or resynchronizations:
14,366 presents and 7,175 commands. After clean shutdown the device reported
7,174 opens and 7,174 closes, zero rejected commands, no device fault and no
active session. Its clock check remained usable for 197/200 samples. See
`reports/emitter-stability-rp2040-final-20261001.txt`. Both emitter families
therefore passed their final short hardware checks; sustained gameplay and
actual lens separation remain unverified.

The updated application is installed at `build/bin/Release/VisionRestoration.exe`;
the prior executable is retained as `VisionRestoration.pre-stability-20261001.exe`.
Final executable SHA-256:
`46B204A1C0FAF438D51BA41510AAD45F732EBC9910263EAE8B4BCB0E692F817B`.
Optical behavior and sustained gameplay must still be checked through the
lenses. Recovery reduces the consequences of missed predictions; it cannot
undo a shutter event or image that already reached the hardware.
