# Angular-response specification — provenance

Date: 2026-11-04. Author: contribution draft; not yet analyst-reviewed.

## Sources

1. **Primary evidence (static):** `docs/cleanroom-driving-physics/feel-audit/ORIGINAL_GAPS.md`,
   item 2 (EFA-2 ordinary angular damping) and item 3 (EFA-3
   slip-dependent vertical torque restriction). The curve pairs, the
   independent gates, the state-code behaviour, the raw-torque
   replacement rule, the K selection offsets and the synthetic checks all
   come from there.
2. **Local aggregate checks:** `docs/FORMATS.md`, "Stock drivetrain and
   axle-force fields" — `+0x398 = 17.0` and `+0x39C = 0.0` across all 46
   local records (aggregate only; nothing committed per car). These are
   local observations, not retail-wide claims; they lie below `+0x3E0`
   and are therefore identical to retail in the local image.
3. **Stage 2 (stabilization state):** `docs/cleanroom-driving-physics/stage2/SPEC.md` —
   pre-Euler `z`, and the `-8*z*P` translational term referenced only for
   gate placement.
4. **Stage 4 (sigma producer):** `docs/cleanroom-driving-physics/stage4/SPEC.md` —
   sigma definition (body heading minus world velocity heading, wrapped,
   quantized) and the below-1 m/s zero rule.

## What was NOT used

- No retail runtime capture; the retail executable was not run.
- No access to the analyst-only evidence register; only the published
  summaries above were used. Anything beyond them is marked unknown
  (AR-7 / U-1…U-8).
- No disassembly, decompiler output, or executable bytes were opened.
- No third-party physics code consulted.

## Verification performed

- All vector expectations were reproduced with an independent Python
  evaluation of AR-1/AR-3/AR-4 (see test-vectors/README.md); all match.
- The `b32(0.1)` threshold case uses the exact binary32 value
  `0.10000000149011612`.
- Local aggregates were computed read-only from the user-owned
  `GLOBAL/GLOBALB.BUN`; nothing was written or committed except the
  aggregate ranges in docs/FORMATS.md.

## Open provenance questions for the analyst

1. Is `z` in the angular terms exactly the Stage 2 stabilization state
   (pre-Euler), and is it shared with the translational `-8*z*P`?
2. Gate narrowing trigger "all four exported contacts plus
   `abs(body_forward.z) > b32(0.15)`" — confirm both conjuncts are
   required, and confirm `body_forward.z` is the world-frame Z of the
   body forward axis.
3. `p` identity and reachability: is `p` the same "numeric selector" as
   any previously named race/mode selector? Which producer sets it?
4. K=20 trigger uses `sigma*L.z < 0` — is `L.z` the world-Z component of
   world angular momentum (not body-axis), as this draft assumes?
5. Does the frame-refresh K selection happen before or after Stage 2's
   own per-frame refresh, and can K change within a frame (substeps)?
6. Are +0x398/+0x39C per-car constants or shared template values? (Local
   data: identical across all 46 records; distribution may differ by
   edition/revision.)

Exact answers belong in an analyst amendment; this draft must not be read
as resolving them.
