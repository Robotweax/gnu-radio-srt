# Examples

All examples use AES-CTR and read the passphrase from the environment
variable `RWSRT_PASSPHRASE` (10 to 79 bytes) when they start. Set it in the
shell of both ends before running. Start the receiver first, or use strict
mode on the sender, which waits for its peer.

| File | What it is |
|---|---|
| `live_receiver.grc` | Companion flowgraph: SRT Source (Listener, port 9000) into a null sink. Enable the Message Debug block to print status |
| `live_sender.grc` | Companion flowgraph: 3 seconds of a paced test tone into the SRT Sink (Caller) |
| `headless_receiver.py` | Command-line receiver with a tag-aware consumer; JSON status lines |
| `headless_sender.py` | Command-line sender; JSON status lines |
| `gap_aware_spectrum.py` | Receiver with an FFT that never spans a gap; JSON lines, or a live plot with `--gui` |

Both flowgraphs are strict: any loss ends them with a failure in the status.
The Companion receiver ends in a null sink on purpose. A sink that ignores
tags is only correct in strict mode; in segmented live mode put a gap-aware
block after the Source, as `headless_receiver.py` does.

`gap_aware_spectrum.py` contains `gap_aware_fft`, a small block that shows
how downstream processing must treat `rwsrt.discontinuity`: it discards its
unfinished window at the tag and restarts at the tagged sample. Its headless
mode and the block are covered by the automated tests. The `--gui` plot needs
GNU Radio's Qt GUI and a display and has not been run in the test
environment.
