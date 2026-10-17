# Stage5 synthetic vectors

`components.json` contains 77 input/output cases of the named operations in
SPEC.md; `surfaces.json` contains only the six functional numeric channels for
29 surface IDs; `surface_mapping.json` fixes how a world contact yields one of
those IDs. All expected outputs are calculated from the statically inferred
requirements, not captured by running original instructions. Surface parameters
were read from the intake image and narrowed to numeric interoperability facts.
The small bump sequence is a numeric compatibility parameter, not an asset.
No original code, executable bytes, recovered identifiers or complete data records
are included.

`surface_mapping.json` is a static-derived compatibility artifact (S5-11). It
records the identity mapping from the one-byte surface attribute carried by a
world collision polygon to the S5-7 surface ID, the 0..28 ID domain, the
per-record channel offsets, and the fixed fallback IDs. Its `shipped_*` fields
and `used_by_shipped_data` flags come from a static walk of the shipped world
collision geometry, not from a running game; the `vectors` field lists the
boundary and representative attribute→ID cases an implementation must satisfy.
No semantic material or surface names are asserted.

For scalar/vector arithmetic use absolute tolerance 0.00002 plus relative
0.00002; exact IDs, contact/lock predicates, update counts and Boolean results
must match. Decimal constants and persistent inputs designate nearest binary32;
all strict branch boundaries must follow the SPEC, even when numeric output
tolerance would otherwise hide a branch difference. Boundary values adjacent to
a binary32 threshold were selected by adjacent representable bits. Expected
outputs sometimes show decimal real-number calculations to avoid asserting
unestablished x87 intermediate rounding. These tolerances do not permit changing
a threshold or adding a clamp.

Each case's `operation` identifies its entry boundary. For example, tests with
`vm` enter after the signed speed conversion; tests with `candidate` enter after
the external query/radius sum. Scalar alignment cases supply the lateral-normal
dot product within the unit-vector domain. The pair sequence uses the specified
ordered refresh. The four-contact cases prescribe body pitch/roll, then solve
geometry and paired loads. Their pedal labels identify braking/throttle scenarios;
they do not assert that those pedals instantly create that prescribed pose.
They test front/rear and lateral transfer consequences without inventing an
unresolved 3D body trajectory. Service reverse cases intentionally have negative
rear coefficients, as specified by the Stage5 supersession.

For every four-contact case, `body_axes` explicitly supplies B's world-space
columns (forward, lateral, up). The pitch convention is forward=(cosine,0,sine),
lateral=(0,1,0), up=(-sine,0,cosine); roll is forward=(1,0,0),
lateral=(0,cosine,sine), up=(0,-sine,cosine). Zero steering, ride offsets and
bump are fixture choices. Compute Q=P+B*(mount-centre_offset), distance from
S5-3, then candidate=distance+radius and the sequential S5-4 loads. In these
level-height fixtures the travel before clamping is
`radius-(P.z+sine*mount_axis)/cosine`; omitting the cosine division or the
radius/P.z term changes the required result. Damping and progression are zero
in these cases. The brake/throttle labels prescribe no body-state producer.

The two rear-grip threshold cases bracket the exact product of the two
binary32 angle constants with adjacent binary32 sigma inputs. The product
itself is not a representable binary32 input; treating its decimal value as an
exact supplied sigma would choose the wrong strict branch after input rounding.

Every component case records its synthetic static-inference provenance.
For each individual surface row, the provenance is direct numeric extraction
from the same intake image, narrowed to the six channels identified by E5-7.
All 29 IDs are retained because the external query may supply any of them and
each selected channel affects a specified output. Repeated values do not imply
an evidenced generative formula; no such formula for the channel mapping was
established. The bump index has a formula in S5-7; its 16 scalar samples are
necessary numeric compatibility parameters. Neither file contains surface
names, original presentation data, assets or complete source records.

Scheduling cases test retained prepared forces, once-per-frame blend assignment,
ordered pair sampling and a zero-duration extra sweep. No end-to-end runtime
trace or stopping distance is supplied. Bump inputs include negative coordinates.
Surface composition demonstrates that a body-drag channel of 5 is separate from
spin-drag 1.05 and the two tyre channels.

The analyst-only evidence register associates these cases with E5-1…E5-8. The
analyst generator and binary inspection commands remain outside this directory.
