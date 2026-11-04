# Angular-response questions

No implementation questions received yet — there is no implementation.
Questions for the analyst/reviewer before the spec gate:

| # | Question | Blocks |
| --- | --- | --- |
| Q-1 | Confirm `z` is the Stage 2 stabilization state (pre-Euler) and shared with the translational term. | AR-3 |
| Q-2 | Confirm the code-4 / throttle / contacts∨|fz| trigger semantics: independent alternatives; both conjuncts required for the contacts alternative; `body_forward.z` axis. | AR-2 |
| Q-3 | Identity and producer of selector `p`; does `p != 0` branch ever occur in ordinary (non-race) play, or only under a specific mode? | AR-4 |
| Q-4 | Confirm `L.z` in the sigma trigger is world-frame Z of world angular momentum. | AR-4 |
| Q-5 | Frame-refresh ordering vs Stage 2 refresh; K stability across a frame's substeps. | AR-4/AR-5 |
| Q-6 | Is +0x398/+0x39C an engine-wide constant (17.0/0.0 locally) or per-car? Any known exceptions in other editions? | AR-4 |
| Q-7 | Exact endpoint behaviour for `w` exactly on knot values (e.g. `w == 4`): interpolation endpoint or inclusive knot value — numerically identical here, confirm no half-open interval semantics intended. | AR-1 |
| Q-8 | Should the `K == 0` remote-movement tiny override be split into its own document (U-5) now, or deferred? | AR-4/AR-7 |

Reviewer instructions: answers land as an analyst amendment to SPEC.md,
followed by a fresh reviewer gate. Unknowns that remain unanswered stay
explicit unknowns; they must not become implementation defaults.
