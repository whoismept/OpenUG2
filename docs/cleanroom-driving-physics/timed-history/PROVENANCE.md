# Timed-history specification — provenance

Date: 2026-11-04. Author: contribution draft; not yet analyst-reviewed.

## Sources

1. **Primary evidence summary:** `docs/cleanroom-driving-physics/feel-audit/ORIGINAL_GAPS.md`,
   item 4, "Steering history producer — high priority for response" (EFA-4).
   Every normative statement in `SPEC.md` traces to that summary or to the
   Stage 4 documents it references. The Stage 4 spec already cites U4-2 and
   Q4-I1 as the unknown this document proposes to close.
2. **Stage 4 spec:** `docs/cleanroom-driving-physics/stage4/SPEC.md` (S4-2
   consumes `h`/`e`; the 0.55 s / 0.15 s windows are stated there), and
   `stage4/QUESTIONS.md` (Q4-I1: complete history startup/expiry/reset
   semantics unknown to the implementer).
3. **Filter composition rules** (TH-6): S4-2 text in the same Stage 4 spec
   (input appended after the angle is calculated; rate sample included before
   the angle rate is calculated; `h` read before the append; `e` includes the
   current sample).

## What was NOT used

- No retail runtime capture exists; none was attempted for this document.
- No access to the analyst-only evidence register (EFA-4 interrogations);
  only the published summary above was used. Statements beyond it are marked
  unknown (TH-7 / U-1…U-8), not filled in by analogy.
- No disassembly, decompiler output, executable bytes or assets were opened.
- No unrelated game engine or third-party physics code was consulted.

## Verification performed

- The arithmetic in `test-vectors/timed-history.json` was reproduced with an
  independent Python simulation of TH-3/TH-4 (written from this spec, not from
  any implementation); all published vectors match.
- binary32 constants (`0.55`, `0.15`, `0.00025`) were converted exactly;
  both capacity readings agree (66 / 18).
- The exact-age retention case was deliberately excluded from vectors; see
  `test-vectors/README.md`.

## Open provenance questions for the analyst

1. Is the expiry slot reset to the unused marker, or is the slot left stale
   and only skipped by `timestamp > 0` checks? (TH-4 assumes reset; this keeps
   TH-3 step 2 consistent.)
2. Is `live_count` an integer or a floating accumulator in the original?
   (Assumed integer here.)
3. Is `sum` binary32 or higher intermediate precision, and is `average`
   published as `sum/live_count` per frame or only on demand?
4. Does `expiry_pos` exist as a persistent scan pointer in the original, or
   does deletion scan from the oldest slot each call? (Both satisfy the
   summary; this spec fixes the pointer because the initial state names an
   expiry position.)
5. Does the input history's caller ever observe `h` before the current frame's
   append in the same call sequence (i.e. is the TH-6 ordering
   caller-enforced or producer-enforced)?

Exact answers belong in an analyst amendment; this draft must not be read as
resolving them.
