# HP Bridge — protocol (P1, live)

One connection between one HPSAMPLER and one HPDG instance. Started from either side:
HPSAMPLER `CONNECT HPDG` or HPDG `Connect to HPSAMPLER` (header, second row, right). Two message directions on it:

| direction | schema | when |
|---|---|---|
| HPSAMPLER → HPDG | `hp.sample-context.v1` (MAIN: tempo, key, pattern events, beat grid, accents, swing) | on connect, then 400 ms after MAIN edits (toggle "Auto-send"), or "Send now" |
| HPDG → HPSAMPLER | `hp.drum-guide.v1` (current drum pattern, bars, tempo, swing) | on connect, then when the HPDG pattern / parameters change |

Neither side regenerates anything because of a message. HPDG keeps the received context; "Use HPSAMPLER tempo and key"
is an explicit action. Feeding accents / harmony into the genre engines is a later stage, to be measured first (HPDG docs/RULES.md).
HPSAMPLER shows the guide as the DRUM GUIDE overlay above the 4 lanes (never a 5th lane); "Follow HPDG drum guide" off = freeze.

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
