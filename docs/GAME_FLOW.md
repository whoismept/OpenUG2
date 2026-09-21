# Bayview: career, shops and progression

Research reference for OpenUG2, 15 September 2026. This is the requested gameplay
reference, not a claim that career mode has been implemented. The desktop/console
Underground 2 campaign is the baseline; handheld versions are outside this scope.
North American starter availability is used below, with regional differences
identified explicitly. Sources describe observable gameplay, not implementation
code. No original dialogue, artwork or binary data is reproduced here.

## Current OpenUG2 boundary

The debug build has **Modification → Body / Specialties / Graphics / Performance /
Safe House**, with the matching green/yellow/red/blue/purple labels. Available
geometry is discovered per vehicle. Bumpers, skirts, hoods, lights, spoilers,
exhaust tips, scoops and trunk layouts have independent selectors; rims use the
existing replacement path. Body/rim paint and underglow are live previews.
Engine-cover geometry is a diagnostic, not a purchased engine package.

These controls do not spend money, unlock content, maintain owned inventory, or
require entering a shop. The Safe House currently shows installed part choices
without claiming ownership. Performance packages, vinyl/decal editing, advanced
paint finishes and most specialty mechanics are still open. Selecting another
car or track now stays in the running SDL/GL session; per-car garage ownership
and installed configurations are still not persisted.

## Story and opening

The returning Olympic City racer is forced to rebuild after Caleb wrecks their
car. Samantha connects them with Rachel Teller in Bayview. Rachel introduces the
local scene and sponsorship opportunities. Success attracts Caleb's opposition:
he tries to control sponsors and uses Nikki Morris against the player in URL.
After losing and falling out with Caleb, Nikki changes sides. Caleb's remaining
racers fail to stop the player, leading to a final head-to-head victory and a
celebration with Rachel and Nikki. This is the story spine; exact cutscene trigger
IDs still need verification against the user's game version.
[Story reference](https://en.wikipedia.org/wiki/Need_for_Speed:_Underground_2#Plot).

The playable introduction starts at **Bayview International Airport in Rachel's
350Z**, temporarily borrowed. The destination is the first car lot, followed by
Rachel's garage and Tommy's introduction. Optional opening events are Outer Ring
(circuit, two laps), Freemont (hidden circuit, two laps), and Palomino & 16th
(sprint). These are not compulsory prerequisites for returning the borrowed car.
The first owned car is a choice, not a mandatory Miata.
[Opening walkthrough](https://gamefaqs.gamespot.com/ps2/920467-need-for-speed-underground-2/faqs/34283).

North American starter choices: **Focus, Civic, Miata MX-5, 240SX, 206 and Corolla**.
[Starter selection](https://gamefaqs.gamespot.com/ps2/920467-need-for-speed-underground-2/faqs/34283).
The regional catalog substitutes **106 and Corsa** for the North American
**Civic and RSX**. Starter-Miata reports differ for PAL releases: verify the first
lot in the target edition before fixing a European starter list. Asset folders
alone do not establish dealer availability.
[Regional car catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Cars),
[PAL starter discrepancy](https://gamefaqs.gamespot.com/boards/920467-need-for-speed-underground-2/42080180).

Stage 1 introduces five world events. Four documented examples are Industrial
Park Track 2 (Street X, three laps), Bayview Bridge (drag), Stadium Drift 1
(drift, three laps), and Scenic Ride (circuit, three laps). The walkthrough's
stage summary includes a sprint but omits its detailed entry: **its name and
internal event ID remain unverified**. The guide's suggested visiting order is
not evidence of an enforced race sequence.
[Opening event details](https://gamefaqs.gamespot.com/ps2/920467-need-for-speed-underground-2/faqs/34283).

## Race loop and rules

Explore → discover an event or receive an invitation → travel to its marker →
accept → load the event's route, opponents and barriers → countdown → race →
results/rewards → update invitations and unlocks → return to exploration.
This is the intended OpenUG2 flow; exact retail event IDs must come from verified
event records, not the order of files in a STREAM bundle.

| Type | Completion rule |
| --- | --- |
| Circuit | Finish the configured laps first. |
| Sprint | Reach the destination first along the route. |
| Drag | Win a short acceleration race with timed gear changes. |
| Drift | Accumulate the highest drift score; finishing first is not the objective. |
| Street X | Win laps on a small, technical closed course. |
| URL | Race on dedicated circuits; a tournament can contain multiple rounds. |
| Free roam | Exploration without a race clock or finish condition. |

The manual distinguishes these modes and provides map/GPS and SMS navigation.
It also describes visual rating as a progression requirement and vinyls as four
ordered, individually recolourable layers.
[EA PC manual, printed pp. 4–7](https://oldgamesdownload.com/wp-content/uploads/manuals/need-for-speed-underground-2_win_manual_en_5rt.pdf).

URL tournament standings use accumulated finishing points. Street X and drift
disable nitrous. Drag uses lane changes and manual shifts; major impacts or
engine overheating can end a run. Outrun is a free-roam challenge: establish a
roughly 300 m lead; it does not follow a prescribed lap course. Photo opportunities
use a timed drive to the photographer. SUV events are vehicle-restricted variants,
not an additional mandatory district.
[Race behavior reference](https://en.wikipedia.org/wiki/Need_for_Speed:_Underground_2#Gameplay).

OpenUG2 must keep first-time event completion separate from retries and replay
rewards. A failed attempt must not advance unlock counters. Hidden events, sponsor
races, URL rounds/tournaments, outruns and photo events need distinct completion
records; one generic `races_won` value cannot safely implement all these gates.
The exact retail points table, tie resolution, timing limits and reward amounts
remain to be verified before implementing each mode.

## District and career progression

| Stage | Newly opened district | World wins | Sponsor wins | URL requirements | DVD requirements |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | City Core | 5 | — | — | — |
| 2 | Beacon Hill | 10 | 3 | 3 | One: 1-star car |
| 3 | Jackson Heights | 20 | 3 | 5 | Two: 2- and 3-star car |
| 4 | Coal Harbor East | 30 | 3 | 7 | Three: 4-, 5-, 6-star car |
| 5 | Coal Harbor West | 35 | 3 | 9 | Four: 7-, 8-, 9-, 10-star car |

Counts are **per stage**, not cumulative. Completing the current stage opens the
next area; earlier areas remain part of the playable city. Winning the final URL
leads to Caleb's concluding circuit challenge. The table summarizes the campaign
requirements, not all optional events on the map.
[Prima guide, career progression](https://www.scribd.com/document/343113449/Need-for-Speed-Underground-2-Prima-Official-Guide),
[district sequence](https://gamefaqs.gamespot.com/ps2/920467-need-for-speed-underground-2/faqs/34283).

After the first event in stages 2–5, sponsor offers depend on previous reputation.
Choose one contract for that stage; it provides bonuses and another vehicle slot.
The contract then asks for 3 sponsored races plus 9/19/29/34 additional races,
respectively, alongside URL and DVD requirements.
[Sponsor reference](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Sponsors).

**Counting caution:** the contract's additional-race counts are one below Prima's
world-win totals. Counting the initial sponsor-triggering win separately explains
that difference, but the actual save counters and which event classes contribute
still need direct verification. Never add both totals as separate obligations.
URL requirements likewise need tournament-versus-round verification before code
uses them as counters. A ten-star appearance and photo visits are progression
work, not optional cosmetics in an otherwise race-only campaign.

## Shops and permitted work

| Marker | Shop | Retail category inventory |
| --- | --- | --- |
| Green | Body Shop | Front/rear bumpers, skirts, hoods, headlight/taillight assemblies, spoilers, exhaust tips, roof scoops, mirrors, rims, carbon-fibre parts, wide-body kits. [Body catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop) |
| Yellow | Car Specialties | Trunk audio, neon, window tint, gauges, doors, split hoods, hydraulics, light colours, nitrous purge, spinners. [Specialties catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop) |
| Red | Graphics Shop | Body/eligible part paint, vinyls and decals. [Graphics catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Graphics_Shop) |
| Blue | Performance Shop | Engine, ECU, transmission, turbo, nitrous, suspension, brakes, tyres and weight reduction. Street → Pro → Extreme packages; dyno tuning depends on installed upgrades. [Performance catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop) |
| Purple | Safe House / Garage | OpenUG2 requirement: swap, remove and reinstall owned compatible parts; choose an owned vehicle and retain its setup. This inventory behavior is a requested design contract, not a claim of completed retail parity. |
| Cyan | Car Lot | Acquire an unlocked vehicle in an available garage slot. Keep vehicle acquisition separate from parts purchases. [Shop/lot reference](https://gamefaqs.gamespot.com/ps2/920467-need-for-speed-underground-2/faqs/33703) |

An exhaust **tip** is a green visual part; exhaust-system **performance** belongs
to blue engine upgrades. Green headlights replace assemblies, while yellow light
customization changes their appearance. Green selects rims; red paints rims.
Yellow nitrous purge is an effect and requires blue nitrous installation.
[Purge prerequisite](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Nitrous_Purge).

Megalow Parts is a performance discount outlet: the reference lists a 20% package
discount and no Extreme packages there. A shop's stock therefore depends on its
identity as well as its colour.
[Megalow restrictions](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop).

### Intended in-world access contract

1. Enter a discovered, reachable shop while stopped; activate that shop's menu.
2. Offer only that category's compatible, unlocked inventory. Show the reason for
   locked items. A preview must not create ownership or debit money.
3. Confirm a purchase only after the candidate loads and fits successfully. Money,
   ownership and installed configuration change together; failure keeps all three.
4. At a safe house, offer previously purchased compatible items and stock parts.
   Removal returns an item to that car's owned inventory; refitting it is free.
   Do not charge again or make an essential assembly disappear when choosing stock.
5. Leaving without buying restores the installed setup. Save ownership separately
   from fitted slots, colours and finish. Preserve both across vehicle selection
   and application restarts.

These are **requested OpenUG2 rules for later implementation**. Start with ownership
per vehicle; a universal spoiler mesh is not proof that purchases transfer freely
between cars. Retail parts were car-bound according to the guide.
[Ownership reference](https://www.scribd.com/document/343113449/Need-for-Speed-Underground-2-Prima-Official-Guide).
Debug access must remain explicitly separate from this career permission path.

## Performance unlock order

`S3/15` means 15 qualifying race wins during stage 3. `Discover BH/CE/CW` means
finding the performance shop in Beacon Hill / Coal Harbor East / Coal Harbor West.
"Start" means initially available stock once the shop is accessible, not already
installed or free. The exact race-counter membership is still unverified.

| System | Street | Pro | Extreme | Source |
| --- | --- | --- | --- | --- |
| Engine | Start | S3/15 | Finish stage 4 | [Engine](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Engine) |
| ECU | Start | S3/7 | S4/16 | [ECU](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/ECU) |
| Suspension | Start | S3/21 | S4/29 | [Suspension](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Suspension) |
| Transmission | Start | S4/9 | Discover CW | [Transmission](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Transmission) |
| Turbo | Discover BH | S4/13 | Discover CW | [Turbo](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Turbo) |
| Brakes | Discover BH | Discover CE | Discover CW | [Brakes](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Brakes) |
| Tyres | Discover BH | Discover CE | Discover CW | [Tyres](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Tyres) |
| Nitrous | Discover BH | Discover CE | Discover CW | [Nitrous](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Nitrous_Oxide) |
| Weight reduction | Discover BH | Discover CE | Discover CW | [Weight reduction](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Weight_Reduction) |

Packages do not simply multiply every statistic: replacement components can be
incompatible with the previous tier's component. Engine changes also affect sound.
Do not map the current acceleration/braking sliders to retail packages without
measuring the vehicle, component and tuning data.
[Engine replacements](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Engine),
[transmission replacements](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Performance_Shop/Transmission).

## Visual and specialty unlock order

The table records families of choices, not a mapping from retail marketing names
to OpenUG2's `KITnn`/`STYLEnn` IDs. Those mappings must be validated separately.

| Family | First group → later groups | Source |
| --- | --- | --- |
| Front bumpers | Start → discover Jackson Heights body shop → discover Coal Harbor West body shop | [Front bumpers](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Front_Bumpers) |
| Hood | Discover Beacon Hill body shop → S2/3 → S3/1 | [Hoods](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Hoods) |
| Headlights | Start → S3/8 → S4/3 | [Headlights](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Headlights) |
| Taillights | Start → S3/33 → S4/47 | [Taillights](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Taillights) |
| Spoiler | Discover Beacon Hill body shop → finish stage 3 → S4/4 | [Spoilers](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Spoilers) |
| Exhaust tip | S1/4 → S4/5 → S4/23 | [Exhaust tips](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Exhaust_Tips) |
| Mirrors | S2/4 → S3/13 → S4/15 | [Mirrors](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Side_Mirrors) |
| Roof scoop | Groups at S2/10 → S3/19 → S4/25 (single, dual and offset variants) | [Scoops](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Roof_Scoops) |
| Rims | Start → S3/11 → S4/27; some styles are SUV-only | [Rims](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Rims) |
| Trunk layout | Standard initially → Tuned at Beacon Hill specialty shop → Custom at Coal Harbor West specialty shop | [Trunk audio](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Trunk_Audio) |
| Paint finish | Gloss initially → Metallic at Jackson Heights graphics shop → Pearlescent at Coal Harbor East graphics shop | [Paint](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Graphics_Shop/Paint) |
| Rim/scoop/spinner paint | Discover Beacon Hill East graphics shop | [Part paint](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Graphics_Shop/Paint) |
| Brake / exhaust-tip paint | S3/12 / S3/23 | [Part paint](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Graphics_Shop/Paint) |
| Rear bumpers / skirts | Start → Jackson Heights body shop → Coal Harbor body shop (source does not specify East/West) | [Rear](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Rear_Bumpers), [skirts](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Side_Skirts) |
| Window tint | Basic at S1/2 → dark colours at S3/4 → pearlescent tint at S4/33 | [Tint](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Window_Tint) |
| Hydraulics | Level 1 initially → Level 2 at Beacon Hill specialty shop → Level 3 at Coal Harbor West specialty shop | [Hydraulics](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Hydraulics) |
| Doors / split hoods | Discover Coal Harbor West specialty shop; unique split-hood timing is disputed by the sources | [Doors](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Doors), [split hoods](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Split_Hoods) |
| Spinners | Discover Beacon Hill specialty shop → discover Coal Harbor East specialty shop | [Spinners](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Spinners) |
| Light colours | Basic initially → more colours S3/6 → advanced variants S4/22 | [Lights](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Lights) |
| Gauges | Initial group → S3/31 → final group S5/7 | [Gauges](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Custom_Gauges) |
| Nitrous purge | Basic with installed nitrous → Type 2 at Beacon Hill specialty shop → Type 3 / coloured effects S4/7 | [Purge](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Nitrous_Purge) |
| Decal slots | Windshield, rear window, six per door and two per rear quarter initially available | [Decals](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Graphics_Shop/Decals) |

Trunk audio uses layouts with differently sized slots for speakers, amplifiers,
subwoofers and screens. Custom trunks support trunk neon; engine neon requires
Extreme engine, ECU and turbo packages. Engine/trunk neon availability is listed
after stage 4. Do not equate a trunk-layout mesh with the complete audio editor.
[Trunk assembly](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Trunk_Audio),
[Neon prerequisites](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Car_Specialties_Shop/Neons).

**Paint direction for OpenUG2:** Gloss is the slightly shinier starting look.
Metallic and angle-dependent Pearlescent belong in the red shop when supported.
Matte is a requested extension, not one of the three documented retail finishes.
A shader clear-coat slider does not reproduce metallic flakes or pearlescent colour
shifts. Store colour and finish independently when purchase/save support lands.

### Unique rewards from outruns

Win counts are within the indicated stage. Reaching the count unlocks a timed
pickup challenge; win it to claim a choice for the active vehicle.

| Stage | Outrun wins | Reward selection |
| --- | ---: | --- |
| 2 | 4 | Unique hood |
| 3 | 3 | Engine, tyres or transmission |
| 3 | 6 | Unique rims |
| 4 | 4 | Unique spoiler |
| 4 | 6 | Unique vinyl |
| 4 | 9 | Brakes, ECU or turbo |
| 5 | 6 | Nitrous, suspension or weight reduction |
| 5 | 11 | Wide-body kit |

[Unique reward reference](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Unique_Upgrades).
The individual Turbo and Split Hoods pages conflict with this aggregate table
about the unique rewards' stage; use the table as research guidance pending a retail check.
Wide-body kits exclude SUVs. Completing the story makes their styles available
in the separate Customise mode; this is not evidence that every career car owns
all kits for free.
[Wide-body restrictions](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Body_Shop/Wide_Body_Kits).

## Dealer unlock sequence

The numbers below are the guide's **global URL unlock sequence**, not "race N
inside the current stage" and not a raw event identifier. Regional variation and
round/tournament counting must be resolved before converting them into code.

| URL ordinal | Newly available cars |
| ---: | --- |
| 1 | Escalade, Navigator |
| 2 | Hummer H2 |
| 3 | Tiburon, Sentra |
| 4 | Celica |
| 5 | IS 300 |
| 6 | Supra; RSX in North America |
| 7 | Golf GTI, A3 |
| 8 | Eclipse, TT |
| 9 | RX-8, 350Z |
| 10 | RX-7 |
| 11 | G35 |
| 12 | 3000GT |
| 13 | GTO |
| 14 | Mustang GT |
| 15 | Lancer Evolution VIII |
| 16 | Skyline GT-R |
| 17 | Impreza WRX STi |

[Unlock sequence](https://gamefaqs.gamespot.com/pc/920469-need-for-speed-underground-2/faqs/35133),
[RSX and regional catalog](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Cars).
Up to five career garage slots are offered. An unlocked model becomes a dealer
choice; that is distinct from acquiring it, installing parts on it, or unlocking
its appearance in Quick Race. Sponsor/cheat promotional cars and traffic models
are not normal dealer rewards.
[Garage/catalog behavior](https://nfs.fandom.com/wiki/Need_for_Speed%3A_Underground_2/Cars).

## Open verification items

- Exact first owned-car sprint, first dealer/garage coordinates, and source event
  IDs for all opening steps. The retail route must replace today's technical
  safe-spawn selection only when this mapping is proven.
- Region/version-specific starter lot; the car wiki's Celica stage label also
  conflicts with the stage URL totals. Preserve the global ordering above without
  treating that label as a verified stage gate.
- Race counters behind visual unlocks, optional/hidden-event eligibility,
  tournament round counting, sponsor availability and final invitation timing.
- Remaining visual gates: carbon fibre, individual trunk components and every
  vinyl group. Resolve the ambiguous Coal Harbor body-shop label and conflicting
  unique-part stages. Do not extrapolate one family's schedule to another.
- Retail component IDs, prices/refunds, visual-rating contributions, dyno curves,
  effect/audio dependencies, photo-event timers and exact save semantics.
- Career district barriers versus ordinary race barriers: current scenery group
  filtering is not a decoded career lock system. Do not unlock districts by merely
  loading another STREAM archive.

## Vehicle presentation and world-placement follow-up

The current default requests **14 cm** of body lowering (was 6 cm), limited by
available body clearance; wheels retain their contact height. The clearance
budget is measured against the **body shell only** and keeps 2 cm under it. The
earlier limit used the whole car AABB, which also covers the rim and brake
slices at hub height: that reported roughly zero clearance on every car and
silently cancelled the entire requested drop, so the cars rode visibly tall. The
shared body pose feeds rendering and collision; collision still uses the full
AABB. `Vehicle Diagnostics -> body lowering` tunes it per car, and the boot log
prints both the requested and the applied drop. This is an adjustable
presentation trim, not a decoded performance suspension package. Clear coat is
slightly stronger by default; Graphics owns its preview controls.

Reported misplaced water/river and greenhouse-like scenery remain a separate
investigation. First identify the location, source model and placement; then
compare its authored transform, world bounds, selected scenery group and collision
classification. A water-looking surface may also be a texture/material issue.
Do not blanket-disable scenery collision or move a district based on appearance.

Read-only placement census on 15 September: eight local STREAM bundles, 174
water-related instance placements, no unresolved models and at most 0.001 m of
transformed geometry outside the authored bounds. This name-based sample did not
establish the reported object's identity, and no world runtime change was made
from it.

RESOLVED on 18 September, and the two halves of the report were one defect. The
"river running into the conservatory" was never water: it is `OBJECT01`, an
unnamed backdrop impostor textured `TRN_COASTROADLOD_A_DM` +
`ARC_PANARAMABUILDINGSC_` + `TRN_TREELINEA_DM`, whose 1016 x 365 m sheet reaches
z 35.5 exactly where the park lawn sits at z 29.7-32.2. Because it is 1016 m
wide it also stayed far under the 3000 m measured impostor test, and it has no
name a rule can spell. Meanwhile the real water, `PAN_OCEAN`, was culled by the
`PAN_` prefix, which is why the canals and the bay rendered as empty void. Both
are now classified by material/measurement -- see `docs/FORMATS.md` -- so the
park is clean terrain and the water is back under the bridges. The impostor's
own collision was never in the ground scene (it is category OTHER), so no
collision behaviour changed for it; `PAN_OCEAN` does now answer ground queries
over open water at z ~= 0.
