# Timed-history questions

No implementation questions received yet — there is no implementation. This
draft raises questions for the analyst/reviewer before the spec gate:

| # | Question | Blocks |
| --- | --- | --- |
| Q-1 | Does expiry reset expired slots to the unused marker, or leave stale entries skipped by the `timestamp > 0` predicate? | TH-3 step 2, TH-4 |
| Q-2 | Is `live_count` an integer and `sum` strictly binary32, and is `average` published per append? | TH-2, TH-5 |
| Q-3 | Is `expiry_pos` a persistent ring pointer (this spec's assumption) or a per-call oldest-first scan? | TH-4 |
| Q-4 | Exact-age (`t − t_slot == WINDOW`) retention is normative; confirm the comparison is performed on binary32 values exactly as written, or specify the rounding. | TH-4, vectors |
| Q-5 | Clock source: confirm `t = global_tick_clock × b32(0.00025)` and whether the global tick clock advances per fixed frame or per substep. | TH-1, TH-7/U-4 |
| Q-6 | Confirm the producer-enforced vs caller-enforced ordering of TH-6 rules 1–3 (who appends input vs rate, and when S4-2 reads `h`/`e`). | TH-6 |
| Q-7 | Capacity formula: confirm `trunc(WINDOW × 120 + 0.5)` is evaluated on the binary32 window constants. | TH-1 |
| Q-8 | Scope of "ordinary boundary": confirm which of zero/backwards clock, reset, and count overflow the reviewer wants specified now versus deferred as U-1…U-5. | TH-0, TH-7 |

Reviewer instructions: answers land as an analyst amendment to SPEC.md,
followed by a fresh reviewer gate. Unknowns that remain unanswered stay
explicit unknowns; they must not become implementation defaults.
