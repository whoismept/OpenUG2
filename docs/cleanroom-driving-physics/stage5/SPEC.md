# Stage 5 — bounded suspension, contacts, surfaces and handbrake

Companions are Stage1 `../SPEC.md` SHA-256
`61f14ebb5c850fa8aa414ee20baae001fb64d4fff692e9a7d6698dbabce399bb`,
Stage2 `../stage2/SPEC.md`
`d3072359aa25bf0ef541504805f70b3579829bb9b1310cda8ba1a568286c6cb6`,
Stage3 `../stage3/SPEC.md`
`d4689a16861df810159760a6a668ef3714966fd6c20a30e3b2b54f355c41c6f7`,
and Stage4 `../stage4/SPEC.md`
`02144d187323c6d74a6fb21e6e0b3e2842097cd0e28c935eea97907c1cbc911a`.
They remain unchanged. This contract supplies their deferred effective load,
contact geometry, surface and nonzero-handbrake inputs within the domain below.

**Evidence status:** every executable requirement is **inferred** from static
examination cross-checked against the intake executable. Numeric parameters were
read from the image. Vectors are synthetic calculations, never original runtime
captures. No original executable was launched and no retail handling, stopping
distance or full 3D parity is claimed. A separate reviewer must approve this file's
exact hash before implementation.

## S5-0 Domain, distinctions and replacement rules

Use metres, seconds, tonnes, kN, kN·m, radians and rad/s. Outer frame dt is positive (zero only at creation); substep h is nonnegative.
Inputs/state and decimal parameters are binary32; `b32` rounds to nearest binary32, ties to even. Numeric
vectors permit intermediate-rounding tolerance, never alternative strict
predicates. All values are finite; mass, radius, inertia diagonal, wheelbase and
upper-minus-lower travel range are positive. Normals and wheel/body rotation
axes are unit orthonormal vectors. Invalid surfaces, normals, NaNs and malformed
injected records are outside this boundary.

Travel, radius, mount coordinates and surface bump amplitude are metres;
preload, S and N are kN. Spring stiffness k and paired coupling a are kN/m,
progression j is 1/m, and damping d+/d- is kN·s/m. Body/wheel inertia is
tonne·m²; bump frequency is 1/m. Shares, alignment, kA and grip/surface
multipliers are dimensionless. These units follow the stated equations.

Wheel indices 0/1 are front, 2/3 rear. Local x is forward, y lateral, z up.
`B` maps body vectors to world vectors; `P` is world centre position, `V` linear
velocity and `Omega` world angular velocity. Distinguish:

- `v`: previously exported signed forward speed `V dot B(1,0,0)`;
- `speed`: previously exported magnitude `|V|`;
- `ncontact`: number of exported wheel contact flags;
- `p`: supplied numeric driver-role selector. `p=1` selects the established
  player rear-speed factor; `p=0` selects the low-grip/non-player branch.
  This selector is **not** a contact count. Other values require explicit inputs.
- `state`: vehicle-state code; the ordinary bounded domain uses 0 or 2.

The ordinary domain excludes alternate race families, nitrous, wheel damage,
remote movement, launch pulses, shift-adjustment grip bonus, active wheel-lift
controls, scripted reset impulses and rest/stuck handling. They are explicit
external boundaries, not presumed absent in every retail frame. Dynamic world
query and collision response remain external. Body angular integration remains
subject to the companion unknowns even though this stage supplies forces,
points and stock inertia.

Composition requirements (each operation occurs once):

1. Replace static quarter-weight or supplied load placeholders by S5-2…5 for the
   bounded suspension path; alternatively retain the supplied-load component
   entry, clearly labelled, without also running a second load producer.
2. Supply S5-5's aligned N to **all** Stage1/3/4 tyre, lock and cap calculations.
   Supply S5-7's four distinct surface channels in their existing formulas.
3. Replace the minimal S3-8 handbrake coefficient input by S5-6 and its frame
   grip output. Preserve Stage3 sign, relief, lock, direction and torque smoothing.
   Replace any host handbrake hold/direct velocity scaling with these contributions.
4. For the shared service exception in S3-2, supersede `vmph` by
   `b32(v*b32(2.236999988555908))`. It uses **signed forward speed**, not magnitude,
   and supersedes that paragraph's conversion constant. Use the same value for
   S5-6. Thus a reverse simultaneous-pedal fixture can produce negative rear
   service coefficients; it is outside S3's former nonnegative-coefficient
   fixture domain, but the signed coefficient follows this corrected distribution.
   For this reverse exception continue Stage3 torque addition and lock-test algebra
   with signed B=4*Kb*ai, rather than taking abs(B), clamping it or calling it a
   nonnegative magnitude. H remains nonnegative. No reverse-symmetry clamp is added.
   Ordinary S3 fixtures entered directly at
   their coefficient boundary retain their own supplied inputs.
5. S5-5 supplies the force transformation and contact point, replacing S4-6's
   level-only application adapter. Do not add N separately a second time. Body
   gravity, aero and stabilization still act once under Stage2. No direct yaw
   assignment, extra grip decay or algebraic load-transfer correction is added.
6. S5-8 refines lifecycle scheduling; frame distribution does not retroactively
   replace the already prepared forces consumed by the first derivative.

## S5-1 Effective data and initialization

Offsets below are within a 2,192-byte little-endian source car row. The effective
record starts at row +0x110. Stock ordinary composition preserves these numeric
fields; upgraded/tuned/variant composition is outside this stage.

| Row offset | Meaning |
| ---: | --- |
| +0x110,+0x114,+0x118 | Stock centre c.x,c.y,c.z |
| +0x120+i*0x30, three floats | Wheel mount m_i |
| +0x130+i*0x30 | Wheel radius R_i |
| +0x220 | Body mass M |
| +0x224,+0x228,+0x22C | Body dimensions dx,dy,dz |
| +0x270 | Yaw-inertia multiplier kI |
| +0x280 / +0x2A0 | Front/rear progression j |
| +0x284 / +0x2A4 | Front/rear stiffness k |
| +0x288 / +0x2A8 | Damping d+ for positive travel rate |
| +0x28C / +0x2AC | Damping d- for nonpositive travel rate |
| +0x290 / +0x2B0 | Paired-wheel coupling a |
| +0x294 / +0x2B4 | Upper travel U |
| +0x298 / +0x2B8 | Lower travel L |
| +0x29C / +0x2BC | Presentation travel reference, not a force rest length |
| +0x374,+0x378 | Aero coefficients cF,cR (Stage2) |
| +0x37C | Aero-to-preload factor kA |
| +0x384,+0x388,+0x38C | Brake strengths Kb,Kh and rear share beta (Stage3) |
| +0x390,+0x394 | Rear slip-bonus coefficients b0,b1 |

Before the established mode selection, the diagonal stock inertia is
`Ix=M*(dy²+dz²)*b32(1/12)`, `Iy=M*(dx²+dz²)*b32(1/12)`,
`Iz=M*(dx²+dy²)*b32(1/12)`. The second selected inertia differs by replacing
Iz with `Iz*kI`; retain the companion's supplied mode selector for choosing
which inverse is used. Do not add a parallel-axis correction or replace body
dimensions with wheelbase/track. Complete angular damping remains U5-6.

Ordinary centre c is the record centre. If a caller supplies a changed centre
c_new, preserving the geometric body origin requires
`P_new=P_old+B*(c_new-c_old)`. Mount arms must then use the changed centre.
The alternate state-1 centre producer is excluded (U5-6).

Creation initializes suspension travel and rate to zero, handbrake blend r=1,
base frame grip q=1 and wheel longitudinal/lateral/peak scales to 1. Wheel spin
inertia is assigned `b32(.01)` at frame load refresh, superseding an arbitrary
host wheel-inertia placeholder when this path is selected. Unknown lock-reset
behavior from Stage3 is still explicit. At an arbitrary restored-state entry,
supply old travel/rate, r, frame/export snapshots and prepared forces, rather
than reconstructing them from pedal state.

## S5-2 Frame preload, mount geometry and aero feedback

Refresh before the current frame's mode refresh and pedal distribution. Let
c be the effective centre, `xf=m_0.x-c.x`, `xr=m_3.x-c.x`,
`sr=b32(.5*xf/(xf-xr))`, `sf=.5-sr`. A stock enabled centre switch uses c.x;
its disabled variant uses zero only for the share computation. Mount subtraction
still uses the actual c. The image switch starts enabled; later switch changes
are external. For valid ordinary records sf and sr are nonnegative.

Let e_prev be the aero enable value held **before** this frame's mode refresh.
The total preload scalar is

`W=b32(M*b32(9.8)+(cF+cR)*e_prev*max(v,0)*kA)`.

Each front wheel gets `preload=b32(W*sf)` and each rear `b32(W*sr)`.
This is not a per-axle sum: two wheels receive each share, so their total is W.
Its aero term is linear in the nonnegative signed-forward snapshot. It does not
use instantaneous substep magnitude, the Stage2 reported vertical aero load,
contact fraction, throttle/orientation factor, response scale or inverse speed
scale. It is an independently established feed-forward term, in addition to the
actual Stage2 aerodynamic force and its consequent body motion. Do not add the
reported Stage2 load again to preload or N.

Given an explicit per-wheel ride offset o_i, the fixed body mount arm for this
frame is `r0_i=m_i-c-(0,0,o_i)`. Zero ride offsets are a bounded ordinary fixture;
the active lift producer is external. Preload, r0 and spin inertia are held for
the frame. Braking/throttle/lateral acceleration do not directly modify W or the
shares: transfer emerges through body pose, suspension travel, rate and moments.

## S5-3 World query and geometric travel

For each wheel at its post-step, query at `Q=P+B*r0`. The external query returns
world height H at Q.x,Q.y, unit normal n, integer surface s and found flag.
The physics wrapper uses `H+height_offset` when found; otherwise it substitutes
-10000. The per-wheel height offset initializes to zero; other producers are
external. Then add the S5-7 bump displacement for the returned surface and Q.x,Q.y.
The query must return valid surface and normal even for found=false within this
boundary. Do not infer missing output initialization or invent a contact from
found alone.

Let `u=B(0,0,1)` and `d=u dot n`. If `d<b32(.001)`, return distance -100 and
set effective surface to 0. Equality at the threshold takes the ordinary path.
Otherwise return the signed distance along body up:

`distance=((Q.x,Q.y,H+bump)-Q) dot n / d`.

The ordinary domain has no global forced-surface override. An explicitly selected
override substitutes effective ID 1 **after** bump sampling and before the steep
normal check; it does not resample bump. Missing height is still processed through
this wrapper. No upper clamp on d or automatic normal normalization is added.

Candidate travel is `t*=distance+R`. With current axle limits L,U:

| Candidate | New travel t | Contact |
| --- | --- | --- |
| t*>U | U | true |
| t*<L | L | false |
| L<=t*<=U | t* | true |

Both exact endpoints are contacts; a clamped lower droop value can also be
noncontact, depending on the candidate. A contact can have zero load.
When current substep duration h>`b32(.0001)`, update rate
`tdot=b32((t-t_old)/h)`; otherwise retain the preceding rate. Suspension state
has zero Euler derivatives: this geometric update is its only established
travel/rate producer. Do not separately integrate a spring mass or travel velocity.

The wheel-centre arm is `r=r0+(0,0,t)`, world centre `C=P+B*r`. Wheel frame
uses the Stage4 steer geometry on the body orientation. Sample velocity
`V+Omega cross (B*r)`; no extra tdot term is included. The contact arm below is
distinct from both r and r0. Damage-induced additional wheel rotation is excluded.

## S5-4 Spring force, pairing and bottoming boundary

For a contacting wheel compute the raw scalar

`S=preload + (1+j*max(t,0))*k*t + (tdot>0 ? d+ : d-)*tdot
   + a*(t-t_pair)`.

Set scalar load `S=max(S,0)`; noncontact gives S=0. The presentation reference
field is not subtracted from t. No contact-normal cosine, quarter-weight,
mass/gravity or acceleration-transfer formula is inserted here. The aligned
load N is calculated subsequently in S5-5.

Pairs are 0↔1 and 2↔3. Refresh order is wheel 0,1,2,3, each using its pair's
**currently stored** travel. Thus 0 uses the incoming travel of 1, whereas 1 uses
0's freshly updated travel; the same applies to 2/3. Do not replace this by a
simultaneous snapshot unless entering the component boundary with explicit
`t_pair` inputs. Spring force is stored until force preparation; the full set
of travel values is not retroactively substituted into already computed loads.

At exactly upper travel U, the moving/nonrest path tests a bottoming impulse.
Let `rworld=B*r`, `closing=-n dot (V+Omega cross rworld)`. For closing<0 return
zero. Otherwise requested impulse magnitude is
`J=b32(b32(.225)*closing*M)`, vector J*n at rworld. Each application caps vector
magnitude at the supplied body impulse limit; its angular change `rworld cross
Jvector` has a separate supplied angular-impulse cap. These caps are external
parameters here (U5-6); fixtures use limits above the request. A global impulse
suppression input can disable application while retaining the reported request.
After any upper-limit test the scheduler performs a zero-duration post-step sweep,
even when closing<0 and J=0. Rest-triggered sweeps are external U5-6.

## S5-5 Alignment, point and force application

At preparation, publish the contact flag. For contact=true set world point
`T=C-R*n`. For contact=false leave the previous point retained; no force is
applied there. Given positive S and world wheel lateral axis l,

`alignment=max(b32(.3),4*((l cross n) cross l) dot n-3)`;
`N=b32(alignment*S)`.

For nonpositive S, N=0. Under the unit-vector domain this is
`max(.3,1-4*(l dot n)^2)`; this identity explains the alignment without requiring
another normalizer. There is no upper clamp, no separate body-up cosine, and no
contact-flag test inside this positive-load alignment operation. The composed
producer nevertheless sets S=0 for noncontact in S5-4.

The stock force-basis selector starts at 1: world heading h, lateral l and wheel
up u_w are the wheel orientation axes, including body tilt and steering. Transform
the final Stage1/3/4 values as `F=Fx*h+Fy*l+N*u_w`. In particular N is along the
wheel up axis, not automatically along n. The normal controls geometric travel,
contact point and alignment. This extends the common level-plane result exactly.
For a contact, add F and `(T-P) cross F` once to body accumulators. No contact
means no body wheel force application even if a component fixture injected
positive N. Wheel preparation can still update spin/lock in that fixture.
The five provisional Stage1 force passes retain their local planar projection
and r0.x/r0.y yaw arms; they do not repeatedly add these final body forces.

Other explicit force selectors are characterized as follows, with h,l the wheel
axes: 2 uses `h-(h dot n)*n`, `l-(l dot n)*n`, n without normalization;
3 uses `normalize(h-(h dot n)*n)`, `n cross normalized_heading`, n;
4 uses h,l,n. Selector 1 is the bounded ordinary default. A degenerate projected
heading under selector 3 is outside this contract. Do not silently replace selector
1 by a terrain-tangent basis. Full retail selector mutation remains U5-6.

## S5-6 Handbrake and bounded frame grip

Use the pedals remaining after the once-only Stage1/3 exclusivity operation.
Let hb be handbrake in [0,1], `vm=b32(v*b32(2.236999988555908))`, dt the outer
frame duration, and sigma the wrapped body-heading-minus-velocity-heading angle
in radians using Stage4 quantization. When v<1 set sigma=0, even in reverse.
The following coefficients and grip are assigned once per frame, held through
substeps. Do not update the blend from every substep duration.

The persistent blend r initializes to 1. For hb<.5 set
`r_new=min(1,r+dt)`; for hb>=.5 set `r_new=max(.5,r-20*dt)`.
This asymmetry is intentional; r is a rear-grip blend, not brake torque smoothing.
There is no hb interpolation inside the state update apart from this threshold.

Front eta_0=eta_1=0. For each rear let eta=hb. Only if eta>`b32(.2)`:
when p=0 add .5; when p!=0 add .5 only if
`abs(sigma)>b32(.3)` and `vm<80`. At exact .2, .3 radians or 80 the corresponding
strict predicate fails. Result eta can reach 1.5. Nonzero hb at or below .2 still
produces its unboosted torque. The Stage3 magnitude is `H=10*Kh*eta`, with no
extra radius, rear-share or mass factor. With hb=0 all eta are zero.
The Stage3 combined-torque relief is suppressed whenever H!=0, not merely hb>=.5.
Its existing lock reevaluation/retention determines lock release; this blend is
not an unconditional unlock timer. Reverse p!=0 has sigma=0, so gets no .5 boost.

Let q be the supplied per-vehicle base grip (creation 1), X the supplied drafting
factor, and ordinary alternate-family gates false. Define
`g=q*(p==0 ? b32(.85) : 1)*(1-(1-b32(.975))*X)`.
The X producer and q's later writers remain U5-3; X=0 is an explicit fixture.
This refines the shorthand frame grip of Stage1 without adding another factor.
Front frame grip is g. Rear frame grip is `g*(1+(f-1)*r_new)`, where:

- let `z=abs(B(1,0,0).z)`; initially `f=1+z`;
- if effective eta>.5 and abs(sigma)<`b32(20)*b32(.01745329238474369)`, use that f;
- otherwise let `c=(p==0 ? b0+b1 : b0+b1*event)` and
  `s=s0+(1-s0)*(speed-v0)/(30-v0)`; use
  `f=1+z+min(b32(.35),c*s*abs(sigma))`.

There is no clamp on speed interpolation or lower clamp on the bonus. In the
image's zero-filled initial state v0=0 and s0=0, so s=speed/30. These two effective
calibrations are supplied state inputs if externally changed; v0 must differ
from 30. `event` is the supplied transient event-curve sample (U5-3), not contact
count or acceleration. Use the whole-body speed magnitude only in this rear
bonus interpolation. A steep pitch term z remains even when handbrake suppresses
the slip bonus. At sigma equal to the 20-degree radian threshold, the bonus path
is eligible. Compare against the product of the two stated binary32 constants
without first rounding that product to binary32. It lies strictly between
binary32 sigma values 0.3490658402442932 and 0.3490658700466156: the former
suppresses the bonus when eta>.5, the latter permits it. The role argument must
not be replaced by ncontact.

This is the bounded ordinary frame-grip output, before any separately supplied
shift/launch/nitrous multiplier. The shared response scale and Stage1 conditional
player rear-speed factor remain distinct and applied once at their stated sites.
Per-wheel longitudinal and lateral scales initialize 1 and have no new producer
here; rear peak-scale modification by alternate-family yaw assistance is excluded.
Complete other writers are U5-3. The wheel handbrake does not directly assign body
yaw speed or add an independent handbrake yaw impulse in this bounded branch.

## S5-7 Surface channels, mapping and bump displacement

`test-vectors/surfaces.json` is the normative bounded numeric mapping from surface
IDs 0…28 to peak, sliding, spin-drag, body-drag, bump frequency and amplitude.
Decimals designate binary32. These six channels are compatibility parameters;
no original names, visual assets or complete record dumps are supplied. IDs are
returned by the external collision query; S5-11 specifies the world attribute
that yields them (a per-polygon byte used directly as the ID). User-facing
labels remain unestablished; ID 1 is an explicit ordinary road fixture and is
also the query's default/override ID. Do not assign IDs based on unverified
semantic labels.

Peak multiplies the final Stage1 friction cap; sliding multiplies the sliding
coefficient and lock input. Spin-drag is rho in the Stage1/2 wheel angular drag
formula. Body-drag is Stage2 d's arithmetic mean over exported contacting wheels,
or 1 with no contacts. They are independent: e.g. ID 15 has body-drag 5 but
spin-drag 1.05. Do not reuse one scalar for all four channels. Surface inputs
for immediate wheel preparation are the just-refreshed query IDs; frame body-drag
uses the previously exported contact/surface snapshot.

For bump, scale world Q.x,Q.y by frequency, take floor cells i,j and fractions
u,w. Define H(i,j) as the numeric sample at index
`(-5*i*j-3*(i+1)*(j+1)) mod 16`, with nonnegative remainder. Samples in index order:
`[-.5,-.2,.5,.2,-.4,-.1,.2,.5,-.2,-.5,.4,-.3,.3,0,-.2,.1]`.
Bump equals amplitude times the bilinear interpolation of H(i,j), H(i+1,j),
H(i,j+1), H(i+1,j+1) with weights `(1-u)*(1-w),u*(1-w),(1-u)*w,u*w`.
Negative coordinates use floor, not truncation. World z does not enter the noise.
This describes the scalar surface-height perturbation; it does not perturb n.
Fraction/cell precision at extreme coordinates is U5-5.

## S5-8 Scheduling and bounded body integration interface

At frame entry: centre/mass/preload/mount refresh, mode/surface/response refresh,
shared pedal exclusivity and controls, then brake/handbrake/grip distribution and
any explicitly selected external launch contribution. The preload reads the
preceding aero-enable state; mode refresh then updates that state for force
preparation. All speed/contact values used by these frame operations are the
previously exported snapshot. Current input does not recompute that snapshot.

Use the companion frame subdivision and derivative-then-Euler lifecycle. During
each post-step, body publication/force clearing/gravity precedes wheel0/arm0,
wheel1/arm1, wheel2/arm2, wheel3/arm3 and other established block callbacks.
The arms perform S5-3/4 in that order. Then the external rest/bottoming handling
may request the zero-duration extra post-step sweep described in S5-4. It has
no additional Euler update and retains suspension rate because h=0. Aero and
contact alignment run after these post-steps. All four contacts are aligned
before coupling. Then initial drive, signed brakes/lock/lateral preparation,
five longitudinal passes, final cap/basis transform, finishing and once-only
body wheel-force accumulation run in the companion order. Other explicitly
supplied body torques follow at their established sites.

The first derivative consumes preceding prepared forces. Subsequent preparation
uses current geometry and current frame coefficients; a changed pedal, surface
or airborne flag does not retroactively rewrite the preceding derivative.
Frame export publishes contact flags, count, surfaces, aligned loads, signed
forward speed and magnitude only after all substeps. Creation performs its
established zero-duration preparation starting with travel/rate zero. Geometric
preparation can then change travel; the zero-duration update retains the rate.

A 3D host supplies a consistent P,B,V,Omega trajectory and accumulates the full
force/moment here with Stage2 aero and gravity. A planar host may use explicit
prescribed pose/travel fixtures but cannot claim dynamic pitch/roll transfer
without the outstanding angular/body solver. There is no evidenced direct
`M*a*height/wheelbase` load-transfer operation to substitute. An airborne
transition sets S=N=0 through geometry, applies no wheel body force, and retains
body aero, spin/lock state and existing angular dynamics. Touchdown rate uses
the previous clamped travel, not an invented free-fall suspension state.

## S5-9 Unknowns / ambiguities

- **U5-1** No isolated original runtime, trajectory or differential capture;
  complete handling and visual slope/landing parity are unmeasured.
- **U5-2** External world-height query validity, search/triangle selection,
  collision impulses and missing-output policy.
  The wrapper and numeric ID channels are specified, not a collision engine.
  **Resolved part:** the attribute returned as a contact's surface ID is a
  one-byte per-polygon surface attribute used directly as the ID (identity
  mapping over 0…28); see S5-11 and `test-vectors/surface_mapping.json`.
  **Residual:** validity and triangle selection of the height search, behaviour
  for an out-of-domain attribute (>28), collision impulses, and the
  missing-output policy beyond the fixed ID fallbacks specified in S5-3/S5-11.
- **U5-3** Complete q/X/event producers, externally changed interpolation
  calibrations, other wheel multiplier writers, shift/launch/nitrous composition.
  Supply them explicitly; fixture values do not establish general retail defaults.
- **U5-4** Upgrades, tuning, race variants, driver/device pedal mapping and
  reverse pedal swapping before the simulation-facing boundary.
- **U5-5** Bit-exact x87 precision, trig approximation and extreme-coordinate
  cell quantization. Static vectors have stated tolerances; strict tests remain.
- **U5-6** Complete angular damping/momentum solver and mode selection, alternate
  centre producer, rest/stuck/reset and active lift behavior, impulse cap values,
  later force-basis/global-switch changes. These remain explicit body/external
  inputs; numeric tyre/contact forces do not establish the whole retail chassis.
- **U5-7** Full upstream frame cadence remains the Stage1 timing unknown. This
  stage specifies only the outer-frame/substep/post-step boundary after dt entry.

## S5-10 Traceability and acceptance

Evidence IDs E5-1 data/inertia, E5-2 preload/centre, E5-3 ground/travel,
E5-4 spring/pair/bottoming, E5-5 alignment/basis/point, E5-6 handbrake/grip,
E5-7 surfaces/noise, E5-8 scheduling and E5-9…E5-12 world attribute/ID mapping
resolve in the analyst-only evidence register. Test vectors in `test-vectors/`
are synthetic component, sequence and composition fixtures, with README
provenance and tolerances. They do not claim retail runtime execution. Implement
each covered producer or expose its explicit supplied-input boundary; do not fill
unknowns with silent retail assumptions.

## S5-11 World surface attribute and surface-ID derivation

This refines S5-7 and resolves the attribute-to-ID part of U5-2. A contact's
surface ID is not chosen by the host from semantic labels; the world geometry
carries it. All statements here are testable; `test-vectors/surface_mapping.json`
supplies the identity table, the fallback IDs, the channel record layout and
boundary/representative vectors.

Every driving-collision polygon in the shipped world geometry stores a one-byte
**surface attribute** at offset 2 of its 32-byte polygon record. (Offset 0 is a
barrier/group byte, offset 1 a vertex-extent selector and offset 3 a flags byte;
the attribute is a distinct field, not any of those.)

**S5-11.1 (attribute to ID is identity).** When the ground query selects a
polygon, the returned surface ID equals that polygon's attribute byte widened to
an integer, with no arithmetic remap, table lookup, offset addition or clamp.
The attribute byte is returned unchanged on the found path. Vector: attribute 20
gives ID 20; attribute 1 gives ID 1.

**S5-11.2 (ID domain).** The surface ID domain is 0…28 inclusive, matching the 29
rows of `surfaces.json`. A host reproducing a ground contact maps attribute A to
surface ID A for A in 0…28 and then applies the S5-7 channel row with that id.
An attribute above 28 selects no defined row and is invalid (U5-2 residual).

**S5-11.3 (no material/name table).** For every integer A with 0 <= A <= 28,
`surface_id(A) = A`. There is no material-name table and no material-to-surface
translation step between the polygon attribute and the channel row. Do not invent
semantic material or surface names for any value.

**S5-11.4 (no transform at load or query).** Loading and querying the world do
not transform the attribute. A polygon's byte is returned exactly as stored,
independent of which pack, section or load path supplied it.

**S5-11.5 (fallback IDs do not consult the attribute).** With no collision pack,
or with a pack but no polygon selected, the query output is ID 1; an explicitly
selected forced-surface override sets the effective ID to 1; a failed steep-normal
test sets the effective ID to 0. These restate the S5-3 rules for the mapping; all
of these fallbacks use fixed constants, not geometry.

**S5-11.6 (shipped reachability).** A static walk of the eight shipped world
stream bundles yields 417572 collision polygons whose attribute takes exactly the
values {0, 1, 3, 8, 9, 10, 11, 14, 15, 16, 17, 19, 20, 23, 25, 26, 28}; none
exceeds 28 and value 1 dominates (259255 polygons). IDs 2, 4, 5, 6, 7, 12, 13,
18, 21, 22, 24 and 27 do not occur in these bundles but remain valid rows because
the query may return any value in 0…28. Reproducing shipped-world contacts
therefore requires only the identity mapping over 0…28.

**S5-11.7 (channel record layout).** `surfaces.json` fixes the channel values. The
ID indexes fixed-size records of 152 bytes, 29 in number, with frequency at record
offset 0x18, amplitude at 0x1C, peak at 0x24, slide at 0x28, spin at 0x2C and
body-drag at 0x30. These offsets are compatibility data; a host must not reorder
or share the channels.

Subsequent writers of a contact's ID (for example a race-specific forced surface,
U5-6) are external and must be supplied explicitly; this section fixes only the
world-attribute derivation.
