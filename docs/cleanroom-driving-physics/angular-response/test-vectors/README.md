# Angular-response component vectors

`angular-response.json` contains **synthetic derived** fixtures for the
ordinary angular damping and slip-dependent vertical torque restriction
(EFA-2 / EFA-3). None was obtained by running the retail executable.
Expectations were calculated from `../SPEC.md`; the curve knots and the
`+0x398`/`+0x39C` scalars come from the reviewed static evidence summary
and local aggregate checks (see `../PROVENANCE.md`).

There is **no implementation** of this contract; the vectors exist for
review and for the future component tests.

Real-valued outputs accept `abs(actual − expected) ≤ absolute_tolerance +
relative_tolerance × abs(expected)`. `angular_k_select` outputs are exact.

Coverage:

- `angular_curve_D` — knots, interpolation midpoint, high clamp;
- `angular_ldot` — curve only, stabilize addition, both-gates-off (state 1),
  code-4 boundary narrowing (stabilize off, curve on);
- `angular_slip_override` — raw torque above / inside K, negative branch,
  raw-in-range-even-if-damped-out-of-range distinction;
- `angular_k_select` — p==0 indirect selection, p≠0 sigma trigger,
  same-sign no-trigger, exact-threshold no-trigger (strictly greater),
  low-speed sigma zeroing.

Not covered, by design: race-state/throttle/contacts gate producers
(AR-2, U-1…U-3), remote-movement override (U-5), adjusted-inertia transition
(U-7, owned by the body-response contract), binary32 rounding of the
exact-threshold comparisons beyond the single b32(0.1) fixture.
