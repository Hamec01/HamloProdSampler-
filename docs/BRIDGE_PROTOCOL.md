# HP Bridge — protocol (P1, live)

One connection between one HPSAMPLER and one HPDG instance. Started from either side:
HPSAMPLER `CONNECT HPDG` or HPDG `Connect to HPSAMPLER` (header, second row, right). Two message directions on it:

| direction | schema | when |
|---|---|---|
| HPSAMPLER → HPDG | `hp.sample-context.v1` with optional `features.version=1` capability | on connect, then 400 ms after MAIN edits (toggle "Auto-send"), or "Send now" |
| HPDG → HPSAMPLER | `hp.drum-guide.v1` (current drum pattern, bars, tempo, swing) | on connect, then when the HPDG pattern / parameters change |

In HPDG's Sample Analysis menu, select HPSAMPLER to use its musical context for Guide generation.
Received edits update the stored guidance even with the editor closed. Press Generate to use it.
Tempo / key following has its own toggle. Clear or loading an audio file ends the numeric Guide selection.
HPSAMPLER shows the guide as the DRUM GUIDE overlay above the 4 lanes (never a 5th lane); "Follow HPDG drum guide" off = freeze.

The live bridge sends no audio and creates no WAV files. Manual WAV export and loading that file into HPDG remain
available through the existing file workflow.

The optional feature capability uses compact rows on a sixteenth-note grid. Each row contains energy, onset,
low / mid / high energy, twelve chroma values, bass MIDI note (-1 for unknown), bass confidence, and projection
confidence. Floating-point cells are rounded to four decimal places. The method is `source_projection`.
Older readers ignore this optional field. Updated HPDG requires the capability when selecting HPSAMPLER as its
Guide source and continues accepting legacy tempo / key metadata. A legacy `audioReference` never triggers analysis.

HPSAMPLER projects cached source measurements through MAIN timing, pad speed / pitch / reverse, envelopes and cuts.
Band filtering and overlapping audio are approximations. Phase cancellation, nonlinear master clipping, very short
transients between grid samples, and stretch-induced spectral changes are not measured by this projection. Bass
observations beyond the source analyzer's measured window remain unknown. Chroma is sent as evidence and does not
assert a chord root. Treat projection confidence as a conservative influence score, without statistical calibration.

## Transport

`%LOCALAPPDATA%\HamloProd\Bridge\<hpdg|hpsampler>-<id16>.slot`, one memory-mapped file per instance (same mechanism as
HPDG Link). Local OS user only, no sockets, works across host processes, independent of the plug-in window.

Slot = `SlotHeader` (magic `HPBRIDGE`, version 1.0, seqlock counter, heartbeat, instance id, type, name, bpm,
`pairTarget`, `payloadRevision`, `ackRevision`, schema, payload size) + 480 KB UTF-8 JSON payload.

* Writer: seqlock (odd while writing), heartbeat every 500 ms. Reader: copy, retry if the counter moved.
* Handshake: A sets `pairTarget = B` (Connecting). A free B with `acceptIncoming` sets `pairTarget = A`. Mutual pairing with
  fresh heartbeats = **Connected** on both sides. A busy instance (paired with someone else) is shown as busy and refuses.
* Payload revision increases on each publish; the receiver validates (BridgeValidator) and writes the accepted revision to
  its `ackRevision`; HPSAMPLER shows "HPDG has rev N".
* No heartbeat for 3 s = **Stale**; 10 s more, or heartbeat 0 (closed) = **Disconnected**. `pairTarget = 0` = Disconnect.
* Major version mismatch = **Incompatible** (no data exchanged).
* Crashed instances' slots are removed after an hour; discovery lists only alive ones.

## Shared code

`HPBridgeCore/include/hpbridge/{BridgeTypes,BridgeValidator,BridgeSession}.h` and `src/{BridgeValidator,BridgeSession}.cpp`
are copied byte-for-byte into HPDG `Source/Services/HPBridge/`. `HPSAMPLER_Tests` fails if the copies differ.

## Tests

* `HPSAMPLER_Tests`: handshake from either side, both directions + ack, busy partner refused, Stale, Disconnect,
  closed partner never shown Connected, shared files in sync.
* `HPSAMPLER_Tests --e2e-hpdg` with a running HPDG Standalone: discovered, connected, a real drum guide received and
  validated (49 events / 4 bars on the default Boom Bap pattern), context acknowledged by HPDG.

## Fallbacks (no connection needed)

File snapshots (`Export MAIN context`, `Import drum guide file`) and the read-only HPDG Link guide for HPDG builds
without the bridge.
