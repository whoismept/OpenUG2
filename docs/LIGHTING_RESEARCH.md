# Lighting in the original game: research notes

What public sources and this repository's own clean-room specs say about how the
retail game lights a night scene, compared with what OpenUG2 draws today. Collected
on 2026-11-13 from web searches; nothing here comes from the retail executable or
from reading another project's code. Each item is tagged:

- **[spec]** already established in `docs/cleanroom-settings/SPEC.md`;
- **[public]** stated in prose by a public source (linked);
- **[capture]** read by eye from one retail night screenshot (not in the repository);
- **[unknown]** no source found.

## 1. Headlights on the road

| Fact | Tag |
|---|---|
| One projector attached to the player's car lights the road. It is a single source, not one per lamp. | [public] 1 |
| Every world solid the projector can see is redrawn with the world-headlight effect, so walls, kerbs and props receive the light, not only the road. | [public] 1 |
| Result = baked vertex light + headlight texture x falloff ramp x headlight colour. The headlight is additive over the prelit world. | [public] 1 |
| The falloff ramp is computed per vertex. | [public] 1 |
| No shadow: the light passes through walls and cars. | [public] 1 |
| Opponent and traffic headlights light nothing in the world. | [public] 1, 3 |
| The stock game has no beam switch. Mods add one with low beam at 0.75 of full brightness and high beam at 1.0, which suggests the stock light runs at full strength. | [public] 4 |
| On screen: a cool, bluish-white patch about two and a half car widths wide, brightest beside and just ahead of the front wheels, gone about three car lengths ahead. | [capture] |
| Projector field of view, range, pitch, and where the colour value comes from. | [unknown] |
| What `HeadlightClipTex` clips. | [unknown] |

**OpenUG2 today:** the pool texture is laid on a 12 x 8 ground grid, so only the
ground receives light; walls and kerbs stay dark. Size and colour were matched by eye.
High beam exists (`L`), which the original lacks.

## 2. World (ambient) lighting

| Fact | Tag |
|---|---|
| World geometry carries baked vertex light, and the headlight shader uses that vertex light as its base term. | [public] 1 |
| A Car Lighting option exists in the registry block but is capped to 0 and absent from the menu: no dynamic car lighting option is ever active. | [spec] 4.3 |
| On screen: unlit asphalt is close to black, with strong contrast between lit pools and the rest. | [capture] |
| Whether the world also receives any directional light at night. | [unknown] |

**OpenUG2 today:** at night the world is texture x vertex prelight only. Before
this it also took (ambient 0.38 + diffuse 0.62 x N.L) x 1.35 from a fixed sun
direction, which lit sun-facing surfaces about 10% brighter and the rest down to
half. Removing it evens the surfaces out; it does not by itself produce the dark,
high-contrast look of the capture, which more likely comes from section 4.

## 3. Light glow, flares and trails

| Fact | Tag |
|---|---|
| Light Glow is an on/off option: whether light sources get a glow around them. | [spec], [public] 5 |
| Light Trails is a separate on/off option. A 2004 player report ties it to the visible glow of car and street lights and to lights showing through walls. | [spec], [public] 6 |
| Street-lamp flares fade out by 200 m in the stock game. | [public] 2 |
| Headlight flare textures are named `HEADLIGHTFLAREINNER` and `HEADLIGHTFLAREOUTER` and live in the GLOBALB bundle. | [public] 7 |
| Nitrous draws light trails behind the tail lights. | [public] 3, 4 |
| On screen: lamp flares are large, pink to magenta, with star-shaped rays; tail lights throw red light on the road under the car. | [capture] |
| Flare size rule, occlusion rule, how trails are built. | [unknown] |

**OpenUG2 today:** authored `0x135003` lights draw as small camera-facing quads in
their authored colour, fading out by 200 m. A red pool under the rear bumper stands
in for the tail lights on the road; its size and strength are by eye. No trails.

## 4. Whole-frame effects

| Option | What it does | Tag |
|---|---|---|
| Over Bright | brightens only the already-bright parts of the image | [public] 5 |
| Enhanced Contrast (stored as bleach bypass) | widens the gap between shadows and lights | [spec], [public] 5 |
| Tinting | on/off; effect not described anywhere | [spec], [unknown] |
| Motion Blur, Depth Of Field | on/off | [spec] |
| Fog, Horizon Fog | on/off | [spec] |

Enhanced Contrast may be hidden on older graphics cards (one user report) [public] 8.

**OpenUG2 today:** none of these is drawn. The contrast in the capture is probably
partly Enhanced Contrast and Over Bright, both on at Level Of Detail 4 and 5.

## 5. Weather, reflections, time of day

| Fact | Tag |
|---|---|
| The sky has three stock states (sunrise, night, sunset) that rotate; there is never full daylight. | [public] 9 |
| Rain is its own option; rain tunables seen in mods: intensity 0.7, a road-reflection amount for rain of 1.0. | [spec], [public] 4 |
| Stock render sizes: road reflection 320 x 240, car environment map 128, mirror 256. | [public] 2 |
| Stock traffic despawns at 250 m; scenery under 17 game pixels is not drawn. | [public] 2 |

## 6. Per-level presets [spec 4.2]

| Value | L1 | L2 | L3 | L4 | L5 |
|---|---|---|---|---|---|
| Car Headlight, Light Glow, Tinting | 0 | 1 | 1 | 1 | 1 |
| Light Trails, Over Bright, Fog, Horizon Fog, Motion Blur, Depth Of Field | 0 | 0 | 1 | 1 | 1 |
| Road Reflection | 0 | 0 | 2 | 3 | 3 |
| Rain, Enhanced Contrast | 0 | 0 | 0 | 1 | 1 |

## 7. What to implement, in order

1. **World brightness.** Done: the night world uses its baked vertex light only,
   with a brightness slider. Its default of 1.0 is unverified.
2. **Projected headlight on all nearby geometry.** Replace the ground grid with a
   real projection so walls and kerbs receive light, with a per-vertex falloff ramp
   and no shadow. An earlier attempt at redrawing nearby world batches cost more
   than the per-pixel beams, so this needs a tight receiver list.
3. **Flares.** The 200 m fade is done. Larger flares with rays and the headlight
   inner and outer flares from the named textures remain.
4. **Tail-light road glow** under the car. Done, by eye.
5. **Enhanced Contrast and Over Bright** as a final full-screen pass, driven by the
   two existing settings rows.
6. **Light Trails.**

Items 1, 3 and 4 can be tuned against the capture. Items 2, 5 and 6 need the
numbers marked [unknown] above: either a clean-room spec or more captures (lights
on and off at the same spot, facing a wall, with each option toggled).

## Sources

1. NFSU2Recomp pull request 36, description only: https://github.com/TalhaMudo/NFSU2Recomp/pull/36
2. NFSU2 ForceLOD2 README: https://github.com/Lucent-Mods/NFSU2_ForceLOD2
3. nfsu2-web player guide: https://github.com/th0mk/nfsu2-web/blob/main/docs/web/PLAYING.md
4. NFSU2 Extra Options settings file: https://github.com/ExOptsTeam/NFSU2ExOpts
5. Retail readme as reposted: https://www.ingamers.com/forum/weitere-spiele/need-speed-94303.html
6. Overclockers forum thread: https://www.overclockers.com/forums/threads/need-for-speed-underground-2-problem-no-graphics.343959/
7. NFSAddons headlight colour mod page: https://www.nfsaddons.com/downloads/nfsug/tools/7561/new-ultra-intense-headlight-colors.html
8. Metacritic user review mirror: https://pages.cs.wisc.edu/~linzuo/deliverables/stage1/metacritic/metacritic611.html
9. NFSMods "Sunrise, Night, Sunset": https://nfsmods.xyz/mod/5081

Sources 1 and 3 belong to recompilation projects built from the retail executable.
Only their prose descriptions were read; their source code was not opened.
