# Timed-history component vectors

`timed-history.json` contains **synthetic derived** input/output examples for
the two bounded steering histories (input 0.55 s / 66 slots; keyboard rate
0.15 s / 18 slots). None was obtained by running the retail executable or by
differential comparison. Every expectation was calculated from
`docs/cleanroom-driving-physics/timed-history/SPEC.md` using invented inputs;
the binary32 window constants were cross-checked against the identified
executable via the reviewed evidence summary (see `../PROVENANCE.md`).

There is **no implementation** of this contract yet: no source file, test
binary or engine path executes these vectors. They are a specification
artifact for review and for the future implementation's component tests.

Each case names its entry boundary; it is not an end-to-end vehicle run.
Real-valued outputs accept `abs(actual − expected) ≤ absolute_tolerance +
relative_tolerance × abs(expected)`; integer `live_count`/`capacity` must
match exactly. `appends` is the ordered list of `(t, v)` calls made on a
freshly initialized history instance.

Coverage:

- initialization and empty publication;
- startup singleton (average equals the sample, not sample/capacity);
- two-sample cancellation and expiry on append;
- below/above-window retention boundary (strictly-greater expiry);
- overwrite removal versus expiry (ring full inside the window);
- equal timestamps retained, recorded zero-value sample stays live;
- rate-history window expiry and both capacities.

Not covered, by design:

- **Exact-age retention** (`t − t_slot == WINDOW`): normative in TH-4 but not
  vector-tested, because the comparison involves binary32 subtraction rounding
  of a non-representable constant; it needs a dedicated rounding analysis
  before a fixture can assert it.
- Time-zero, backwards-clock, reset and count-overflow behaviour
  (TH-7 / U-1…U-5).
- Producer call cadence, device and analog paths (TH-6, TH-7 / U-6…U-8).
- Any original-runtime or differential trajectory.
