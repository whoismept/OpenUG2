# Timed steering histories — behavioural specification

Subject: the two per-player steering histories used by the Stage 4 bounded
human input filter (S4-2): the **input history** and the **keyboard
change-rate history**.

Status: **proposal, not approved.** Derived exclusively from the bounded
static-evidence summary EFA-4 (see PROVENANCE.md). No retail runtime was
observed; conventions, equations and predicates carry `[inf]`. Nothing in this
document is an implementation authorization; it requires a fresh spec gate and
reviewer sign-off before any code is written. The current gameplay adapter
(fixed 33-input / 9-rate FIFOs) is **not** modified by this document and
remains an independent host policy until a reviewed implementation handoff.

Confidence tags: **[inf]** inferred from static executable/data evidence;
**[obs]** observed in runtime or retail data; **[gss]** guessed (none used
normatively). Untagged statements inherit the tag of their section heading.

## TH-0 Domain, state and composition

1. This document specifies the ordinary, positive, nondecreasing-clock
   boundary only. Time-zero samples, backwards/decreasing clocks, resets,
   count-overflow beyond the ring semantics below, device preferences,
   collision recovery, analog direct-angle specifics beyond the append-order
   rule in TH-6, and the complete call cadence of the producer are
   **outside this boundary** and remain explicit unknowns (TH-7, U-1…U-8).
2. Each history is an independent instance of the same bounded-ring state
   machine. The two instances differ only in window and capacity (TH-1) and in
   which filter consumes their published average (TH-6).
3. The histories are consumed by S4-2 as the supplied scalars `h` (input
   average, before appending the current mapped input) and `e` (rate average,
   including the current keyboard change-rate sample). S4-2 itself is
   unchanged by this document.

## TH-1 Parameters and binary32 constants [inf]

| symbol | value | meaning |
| --- | --- | --- |
| `WINDOW_IN` | b32 `0.550000011920929` (`0x3F0CCCCD`) | input history window, s |
| `WINDOW_RATE` | b32 `0.15000000596046448` (`0x3E19999A`) | keyboard rate history window, s |
| `TICK_TO_S` | b32 `0.0002500000118743628` (`0x3983126F`) | seconds per global tick |
| `CAP(WINDOW)` | `trunc(WINDOW × 120 + 0.5)` | slot capacity: **66** input, **18** rate |

1. `WINDOW_IN`/`WINDOW_RATE` are binary32 constants; comparisons and ages use
   the binary32 values above, not the decimal literals.
2. The factor 120 calibrates capacity only. It does **not** establish a 120 Hz
   simulation, poll cadence, or any fixed sampling interval (see TH-5).
3. Both capacity readings agree: `trunc(b32(.55)×120+.5) = 66` and
   `trunc(b32(.15)×120+.5) = 18`.
4. Clock seconds derive from the global tick clock:
   `t = global_tick_clock × TICK_TO_S`. They are **not** accumulated from a
   caller-supplied `dt`. The origin, reset and mode semantics of the global
   tick clock are unknown (U-4).

## TH-2 State and initialization [inf]

Each history instance holds:

- `slots[CAP]` — ring of `(timestamp f32, value f32)` entries;
- `sum` f32 — running sum of live sample values;
- `live_count` integer — number of live samples;
- `average` f32 — published mean (TH-5);
- `write_pos` integer — next ring slot to write;
- `expiry_pos` integer — ring slot to examine next for expiry.

Initialization: every slot is **unused**, `sum = 0`, `live_count = 0`,
`average = 0`, `write_pos = 0`, `expiry_pos = 0`.

**Unused marker [inf]:** a slot whose `timestamp == 0` **and** `value == 0`
(binary32 zeros) is unused. A recorded sample may itself carry value 0;
unused-ness is the pair, never the value alone. The ordinary boundary admits
no time-zero sample, so timestamp 0 unambiguously marks an unused slot here;
that equivalence breaks outside this boundary (TH-7).

## TH-3 Append operation [inf]

Preconditions (ordinary boundary): `t > 0` and `t` is nondecreasing across
calls to the same history instance. Violating either exits the specified
domain (TH-7).

`append(t, v)` performs, in order:

1. Let `p = write_pos`. Examine `slots[p]`.
2. **Overwrite removal.** If `slots[p].timestamp > 0`, subtract its value
   from `sum` and decrement `live_count`. (A zero/zero slot is unused and
   contributes nothing.)
3. Store `(t, v)` in `slots[p]`; add `v` to `sum`; increment `live_count`.
4. Set `write_pos = (p + 1) mod CAP`.
5. **Expiry** (TH-4) with the same `t`.
6. **Publish** (TH-5).

Equal timestamps are **not** coalesced: each call records a separate sample;
no intermediate samples are synthesized between calls.

## TH-4 Expiry operation [inf]

Expiry runs after every append, using the append's `t`:

1. Let `e = expiry_pos`. Inspect `slots[e]`.
2. If `slots[e].timestamp > 0` **and** `t − slots[e].timestamp > WINDOW`
   (**strictly greater**), delete the sample: subtract its value from `sum`,
   decrement `live_count`, and reset the slot to the unused marker
   (`timestamp = value = 0`). Advance `expiry_pos = (e + 1) mod CAP` and
   repeat from step 1.
3. Otherwise stop. `expiry_pos` is left at the first retained (or unused)
   slot; it never moves backwards within this boundary.

Rules that fix the deletion predicate exactly:

- **Age exactly `WINDOW` is retained.** Only strictly greater ages expire.
- Unused (`timestamp == 0`) slots are skipped without touching `sum` or
  `live_count`; the scan may pass over them.
- Resetting expired slots to the unused marker is what keeps a later wrap of
  `write_pos` over an expired slot from double-subtracting (TH-3 step 2).
- The scan terminates because the ring is finite and `expiry_pos` advances on
  every deletion.

## TH-5 Publication [inf]

After append (and expiry), publish:

- `sum`, `live_count`;
- `average = sum / live_count` when `live_count > 0`, otherwise `average = 0`
  (the initialized value). Empty allocated slots never dilute the average.

`h` and `e` consumed by S4-2 are read at the filter boundary; this document
does not define when S4-2 reads them relative to TH-3/TH-4 beyond the
append-order rules in TH-6.

## TH-6 Composition with the S4-2 filter [inf]

1. **Input history:** the current mapped input `j` is appended **after** the
   steering command angle is calculated (S4-2 stores `j` as the previous
   mapped input and appends it to the input history after the angle).
   Therefore `h` — the average consumed in the same frame — is the average
   **before** appending the current `j`.
2. **Keyboard rate history:** the current change-rate sample
   `E(abs((j − jold)/dt))` is included **before** the angle rate is
   calculated; `e` — the average consumed in the same frame — **includes**
   the current sample.
3. **Analog path:** the ordinary direct-angle analog path does **not** insert
   a keyboard rate sample. This document does not specify the analog history
   semantics beyond that exclusion.
4. `h ∈ [−1, 1]` and the supplied `e` is nonnegative, as required by S4-2;
   those bounds are effective filter constraints, not producer guarantees —
   nothing here licenses clamping in the producer.

## TH-7 Explicitly outside the ordinary boundary [inf]

Not specified by this document; each requires its own contract and tests:

- **U-1** time-zero (`t == 0`) samples and the zero/zero-versus-recorded-zero
  ambiguity they create;
- **U-2** backwards or nonmonotonic clocks;
- **U-3** history reset / vehicle recreation / mode transitions;
- **U-4** global tick clock origin, reset and wrap semantics;
- **U-5** count-overflow beyond the ring (e.g. wider counters,
  saturation behaviour at `live_count` extremes);
- **U-6** the producer's actual call cadence and any frame-rate dependence
  of the recorded stream (capacity calibration ≠ cadence);
- **U-7** analog-specific history behaviour beyond TH-6 rule 3;
- **U-8** race-state, device-preference and collision-recovery interaction.

Any implementation that needs one of these must stop and request an amendment;
it must not guess.

## TH-8 Worked cases (synthetic, [inf])

Arithmetic below uses exact binary32 values; `+` is real addition of the
stored binary32 sample values.

1. **Startup singleton.** Append `(t=1, v=1)` to an empty input history.
   No expiry applies (age 0 ≤ WINDOW). Published: `sum=1`, `live_count=1`,
   `average=1`. (A fixed-count FIFO that divides by capacity would publish
   `1/66`; that is not this contract.)
2. **Two samples.** Append `(1, 1)`, then `(1.25, −1)`.
   `sum=0`, `live_count=2`, `average=0`.
3. **Expiry on append.** Appending `(1.75, 1)` to the state in case 2:
   the sample at `t=1` has age `0.75 > 0.550000011920929` and expires;
   `(1.25, −1)` has age `0.5 ≤ WINDOW` and is retained. Published:
   `sum=0`, `live_count=2`, `average=0`. (A sample-count FIFO retaining the
   first sample would publish `1/3`; that is not this contract.)
4. **Below/above the window.** With a sample at `t=1.0`: at `t=1.54`
   (age `0.54 < WINDOW`) it is retained; at `t=1.56`
   (age `0.56 > WINDOW`) it expires. (Exact-age retention is normative in
   TH-4; it is not vector-tested, see test-vectors/README.md.)
5. **Overwrite versus expiry.** Fill all 66 input slots with `v=1` at
   `t = 1 + k×0.008`, `k = 0…65` (last `t = 1.52`, oldest age `0.52 < WINDOW`).
   `sum=66`, `live_count=66`. Append `(1.528, 10)`: slot 0
   (`t=1.0, v=1`) is overwritten (`sum = 66−1+10 = 75`), and no expiry
   applies (oldest is now `t=1.008`, age `0.52`). Published: `average = 75/66`.
6. **Equal timestamps.** Append `(1, 1)`, `(1, −1)`, `(1, 1)`.
   `sum=1`, `live_count=3`, `average=1/3`.
7. **Rate window.** Rate history, append `(1.0, 0.5)`, `(1.10, 0.5)`,
   `(1.20, 0.5)`. At the third append the sample at `t=1.0` has age `0.20`
   (`> 0.15000000596046448`) and expires; `t=1.10` has age `0.10` and is
   retained. Published: `sum=1.0`, `live_count=2`, `average=0.5`.
8. **Empty publication.** Initial state publishes `sum=0`, `live_count=0`,
   `average=0`.

## Cross-references

- Stage 4 spec: `docs/cleanroom-driving-physics/stage4/SPEC.md` (S4-2
  consumes `h`/`e`; U4-2 is the unknown this document closes),
  `stage4/QUESTIONS.md` (Q4-I1).
- Evidence summary: `docs/cleanroom-driving-physics/feel-audit/ORIGINAL_GAPS.md`,
  item 4 (EFA-4). See PROVENANCE.md.
- Implementation status: none. No code path implements this document; the
  current adapter is an independent host policy.
