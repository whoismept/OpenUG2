# Fixed-input baseline — HUMMER / MIATA / 240SX (OpenUG2, not retail)

Date: 2026-11-04. Branch `docs/timed-history-spec`; build `make debug`,
commit at capture: `47a4556` (docs-only branch; engine identical to
`development` at capture time). Command lines:

```
./nfsu2.exe --path <data root> --vehicle-compare HUMMER MIATA
./nfsu2.exe --path <data root> --vehicle-compare 240SX MIATA
```

These are **OpenUG2 model outputs under identical synthetic inputs**, not
retail measurements and not a parity claim. They are a reproducible
baseline for detecting unintended handling changes; rerun after any
physics-adjacent change and diff the table. No assets or dumps are
committed; commands need a local legally owned data root.

| car | 0–100 km/h (s) | top km/h | steady full-lock radius (m) | yaw swept in 2 s (deg) | slide (m/tick) |
| --- | ---: | ---: | ---: | ---: | ---: |
| HUMMER | 4.98 | 212.75 | 15.6 | 132.8 | 0.026 |
| MIATA | 4.43 | 220.0 | 16.7 | 117.0 | 0.088 |
| 240SX | 4.27 | 220.0 | 17.7 | 115.9 | 0.070 |

Source data (local `GLOBAL/GLOBALB.BUN`, aggregate-verifiable):

| car | mass (t) | GLOBALB power stock/L1/L2/L3 (kW) | gearbox |
| --- | ---: | --- | --- |
| HUMMER | 2.90 | 242.0 / 248.2 / 254.4 / 260.2 | 4/4.100, 4/4.202, 4/4.301, 5/4.400 |
| MIATA | 1.04 | 95.6 / 110.2 / 124.8 / 138.5 | 5/4.300, 5/4.096, 5/3.898, 6/3.700 |
| 240SX | 1.22 | 115.9 / 125.7 / 135.6 / 144.8 | 5/4.083, 5/4.021, 5/3.960, 6/3.900 |

Notes

- The `--vehicle-compare` harness (milestone M121) is the project's
  fixed-input comparison facility; it computes both cars from the source
  records and geometry in one run, GL-free.
- The drag/curve composition and units remain `[inf]` (see docs/FORMATS.md
  and the cleanroom-driving-physics Stage-1 spec); the table is a
  self-consistency baseline, not a calibration target.
- `kI` (yaw-inertia multiplier, +0x270) is 2.0–2.28 across the fleet and
  is currently **not** consumed by the playable yaw equation (see
  feel-audit/INTEGRATION_GAPS.md finding 2; body-response BR-1 defines the
  selected-inertia law but gameplay still supplies the unscaled value).
