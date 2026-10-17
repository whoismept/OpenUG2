# Stage5 questions and bounded answers

All answers below are normative only through the reviewed SPEC.md hash.

- **Q5-1: How does nonzero handbrake enter?** S5-6 supplies the per-wheel
  coefficient and frame rear-grip blend. S3 torque/lock/direction remain shared.
- **Q5-2: What replaces quarter-weight?** S5-2 frame preload and S5-3/4 geometric
  travel, damping and sequential paired coupling, then S5-5 alignment. No direct
  acceleration-transfer formula was established.
- **Q5-3: How do slopes and airborne contacts compose?** S5-3 query wrapper,
  endpoint contact semantics, S5-5 actual point/basis and S5-8 scheduling. Full
  world query/collision and 3D body/angular state producers remain U5-2/U5-6.
- **Q5-4: Are surface factors interchangeable?** No; S5-7 and surfaces.json
  identify independent peak/sliding/spin/body-drag channels and bump parameters.
- **Q5-5: Are prior braking reverse semantics authoritative?** S5-0 explicitly
  supersedes the Stage3 speed snapshot and conversion at distribution, including
  the signed reverse exception; component entry inputs remain explicit.
- **Q5-6: Is complete frame grip solved?** The ordinary rear slip bonus and
  handbrake blend are specified in S5-6. Complete base/drafting/event and excluded
  shift/launch/nitrous/wheel multiplier producers remain U5-3.
- **Q5-7: Does this prove retail handling?** No. Static inference and synthetic
  vector agreement are separate from runtime/differential validation (U5-1).
- **Q5-8: How does a world contact get its numeric surface ID?** S5-11 and
  `test-vectors/surface_mapping.json`: the hit driving-collision polygon carries a
  one-byte surface attribute at offset 2 of its 32-byte record, returned directly
  as the ID with no remap (identity over 0…28). Fallbacks are ID 1 (no pack, no
  polygon, forced override) and ID 0 (failed steep test). Shipped world data uses
  only values 0…28. Search/triangle-selection validity, out-of-domain attribute
  behaviour and collision impulses remain U5-2.

Implementer questions must be added here for analyst amendment and fresh review;
do not seek original material or invent missing defaults.

## Implementation boundary follow-up — 2026-10-15

- **I5-1 (open, U5-2/U5-6):** What reviewed body/angular/world producers should
  replace the independent host's prescribed ride trajectory and numeric-ID-1
  world fixture? The implementation exposes all three contact force/moment
  components and both inertia diagonals, but the playable host integrates XY/yaw
  only. Its independent heave/pitch/roll supplies geometry; no dynamic brake-pitch
  or textbook acceleration-load correction was invented. Complete vertical/
  pitch/roll feedback, angular damping/mode selection and bottoming impulse caps
  require future approved input contracts. **Updated 2026-10-17:** the
  world-material-to-surface-ID part of this item is now resolved by S5-11 and
  `test-vectors/surface_mapping.json` (a host should return the hit polygon's
  one-byte attribute, identity over 0…28, instead of a fixed ID 1). Residual
  U5-2/U5-6 open items are the world height-search/triangle-selection validity,
  out-of-domain attribute behaviour, collision impulse caps and complete
  body/angular producers. Bottoming impulses are explicitly suppressed in this
  host while requests and required zero sweeps remain available. This is a
  limitation, not an amendment to SPEC.
