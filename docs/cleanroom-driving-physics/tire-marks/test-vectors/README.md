# Synthetic tire-mark vectors

`tire_marks.json` is normative after review. Every expected value is an arithmetic or
state consequence of SPEC.md TM-2…TM-11, computed by an analyst-only reference calculation
written from the spec text. None is an original runtime capture; the original executable
was not run. Car tyre widths used (.205, .225, .235) are the stock values of TM-1.1 [obs].

Sections:

- `lay_predicate` — TM-3 inputs (contact, suppression bit, horizontal U, sliding flag, u,
  N in kN) → lay/end. Booleans exact; includes the strict equality cases `u·N = 8` and
  horizontal speed exactly 1.
- `vehicle_gate` — TM-2 vehicle gate `G`, evaluated per vehicle. Inputs: `clause1` (wheel
  effects enabled for this vehicle), `clause2` (not the special mover mode), `clause3`
  (venue not in the reserved block) and the raw `venue` id, with the invariant
  `clause3 = (venue ∉ [4400, 4499])`; output `lays` = `clause1 ∧ clause2 ∧ clause3`. Full
  2×2×4 truth table over venue {4399, 4400, 4499, 4500}, plus free roam (4000), an ordinary
  circuit (1200), the pre-race countdown (clause 1 false), the special mover mode (clause 2
  false) and a non-player vehicle. Arithmetic consequence of the TM-2 text.
- `sample_geometry` — TM-4/5/6: wheel centre C, sampled velocity U, lateral axis l
  (level, l = (0,1,0) so heading is +x), width W, factor k_s, sliding flag, surface, and a
  ground function (flat z = 0 or z = 0.1·y) → alignment, half-width w, sample point,
  half-width vector, intensity, quantised across bytes and intensity byte. `d_i = 0`.
- `strip_sequences` — TM-7/TM-8 sample sequences for one wheel. Origin O is placed 1 m
  from each sample (inside the 4 m gate) except in the rejection case. Output: event list
  (`new`, `commit`, `replace`, `end`, `full`, `new-continued`, `new-seeded`, `far`) and the
  resulting strips with stored points. `attached` = still the trail's open strip.
- `render_alpha` — TM-11.2 vertex alpha from intensity byte and fade byte.
- `view_metric` — TM-11.3 with an example `K` for a 71.5° camera FOV (illustrative only;
  FOV units/values are TM-U6). `fade: null` means not drawn.
- `cell_keys` — TM-10.2 cell indices.

Tolerances: integers, bytes, Booleans and event lists exact. Floats compare with absolute
1e-6 (intermediate precision is TM-U11). Inputs avoid decision boundaries except the
deliberately exact boundary cases, whose operands are exactly representable.

## render_state.json (TM-15, added 2026-10-16)

Synthetic arithmetic consequences of SPEC.md TM-15.2/15.6, computed by an analyst-only
reference calculation written from the amendment text; not runtime captures (the original
was not executed). Texel alpha/colour inputs are the [obs] statistics of TM-15.5 (numbers
only; no pixels are included).

- `constants` — the state values of TM-15.3 in neutral form.
- `effective_alpha` — intensity byte `Ib`, fade byte `F`, texel alpha (0–255) → vertex alpha
  byte (exact integer) and per-pixel opacity `a = (tA/255)·(vA/255)`.
- `source_colour` — texel RGB (0–255) → source colour `min(1, 2·t/255·128/255)`.
- `composite` — `a` and `src` blended over a 0.5 grey destination, 1, 2 or 4 overlapping
  layers (`result = src·a + dst·(1−a)` applied repeatedly).

Tolerances: `vertex_alpha` exact; floats absolute 1e-6 against the formulas, and ±1/255 when
compared with any real rasterised output (the original pixel pipeline is fixed-point).
