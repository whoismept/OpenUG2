# Underground 2 frontend asset notes

This is a clean-room inventory of the supplied game-data root. It records file
names, container headers, logical identifiers and measured sizes only; no retail
asset, texture, font or disassembly output is copied into the repository.

## Runtime boundary

The temporary OpenUG2 frontend that was previously wired into `main.c` exposed
only `FREE ROAM`, `RACE SELECT` and `QUIT`. It was not a retail Underground 2
menu. The game now boots directly into the authored free-roam pose (`race_state
== 1`); the standalone files under `src/frontend/` remain available for a
later, data-backed menu implementation but are no longer called by the game
loop.

## Retail data inventory

The current data root contains these frontend families:

| Path | Measured contents | Working interpretation |
| --- | --- | --- |
| `FRONTEND/FrontB.lzc` | 1,750,887 bytes; JDLZ; decoded size 5,969,488 bytes | Main frontend memory/resource package |
| `FRONTEND/FRONTA.BUN` | 0 bytes in this dump | Empty placeholder here; do not treat as a universal retail rule |
| `FRONTEND/PLATFORMS/*.BIN` | 8 scene containers: `AudioShop01`, `CarLot01`, `Crib01`, `MegaloShop01`, `PaintShop01`, `PartsShop01`, `PerfShop01`, `Showroom` | Shop/crib/showroom platform scenes |
| `FRONTEND/ENVMAPS/*.bin` | `Showroom.bin` and `ElaborareGT.bin`, 2,098,176 bytes each | Frontend environment-map candidates |
| `FRONTEND/MAGAZINES_*.BIN` | `MAGAZINES_FRONTEND`, `MAGAZINES_SHOWCASE`, and low/high streaming variants | Magazine/showcase metadata and streamed content |
| `GLOBAL/FrontEndMemoryFile.bin` | 16 bytes | Small frontend memory descriptor; structure still unresolved |
| `LANGUAGES/*.bin` | `English.bin`, `Labels.bin`, regional language bins and `LanguageTextures.bin` | Text/hash tables and language texture package |
| `SOUND/FE/FE_MB.abk` | 1,217,700 bytes | Frontend-specific audio bank |
| `SOUND/GLOBAL/FE_COMMON_MB.abk` | 166,268 bytes | Shared frontend audio bank |

`FrontB.lzc` starts with the `JDLZ` header. The header declares a decoded
payload of 5,969,488 bytes and a packed length of 1,750,887 bytes. The decoded
data contains the logical paths `Global\\Pipeline\\PC\\eLabFrontEnd.bin`,
`FRONTEND`, `Global\\FrontEndTextures.tpk` and
`Global\\g_Screens\\Stripped\\`; there is no standalone
`FrontEndTextures.tpk` or extracted `.fng` file in the supplied directory.

## Screen graph evidence

The decoded package contains 80 `.fng` suffix occurrences. Alignment/control
bytes are interleaved with some names, so this is a record-reference count, not
a claim that the retail registry has exactly 80 screens. Directly visible
screen/resource tokens include:

- `UI_Main.fng`
- `UI_StartCareer.fng`
- `UI_CareerCrib.fng`
- `UI_CareerSelect.fng`
- `UI_CareerLot.fng`
- `UI_CareerWorldMap.fng`
- `GarageMain.fng`
- `UI_QuickRaceCarSelect.fng`
- `UI_OptionsMain.fng`, `UI_Options.fng` and `UI_Options_PC_Controller.fng`
- `UI_Menu_Asset_Reputation.fng`

The main-screen record co-occurs with these logical resources:

- `U2_MENU_LOGO`, `EA_GAMES_LOGO`, `EA_GAMES_LOGO_RING`
- `MAIN_ICON_CAREER`
- `MAIN_ICON_QUICK_RACE`
- `MAIN_ICON_CUSTOMIZE_CAR`
- `MAIN_ICON_OPTIONS`
- `MAIN_ICON_GO_ONLINE`
- `MAIN_ICON_LAN`
- `MAIN_ICON_PROFILE_MANAGEMENT`
- `MAIN_ICON_NEW_CAREER` and `MAIN_ICON_LOAD_CAREER`
- `MAIN_ICON_SPLIT_SCREEN`
- `U2_Menu_Box.tga`, `U2_Menu_Box_Layer1.tga`, `U2_Menu_Arrow_White.tga`,
  `CarSelect_icon_backing.tga`, `White16x16.bmp` and
  `ConduitMdITC_TT21i.ffn`

The same package contains the U2 crib/map/shop vocabulary (`CRIB_ICON_*`,
`TRACK_ICON_*`, `VISUAL_PART_*`, `PERFORMANCE_*`, `ONLINE_ICON_*`,
`SECONDARY_LOGO_*`). This is materially different from the old synthetic
three-entry OpenUG2 menu and is the asset-backed direction to implement.

## Localization and rendering implications

`English.bin` and `Labels.bin` are binary/hash tables rather than plain text
files. `LanguageTextures.bin` identifies itself as
`Global\\LanguageTextures.tpk` and contains JDLZ and DXT3 records. Menu labels
must therefore use the retail language/hash lookup instead of hard-coded U1/U2
strings, and the font path is the referenced `ConduitMdITC_TT21i.ffn` resource.

The FNG records reference a shared screen resource namespace and image assets;
they are not independent bitmap layouts. A faithful implementation needs to
parse screen records, resolve their texture/font keys, and then attach the
correct platform/showroom scene where the screen uses a 3D model.

## Next implementation order

1. Add a GL-free JDLZ/frontend index reader that reports FNG names, resource
   paths and screen-local references without loading retail bytes into Git.
2. Decode the frontend TPK and language records using the existing texture
   safety checks; add one small parser test for bounds, JDLZ and DXT records.
3. Load `PLATFORMS/Showroom.BIN` and `ENVMAPS/Showroom.bin` behind a diagnostic
   capture, keeping the normal world renderer untouched.
4. Rebuild `UI_Main.fng` from the measured screen graph and U2 icon set; only
   then add profile/career/quick-race/options/online transitions.

The 16-byte `FrontEndMemoryFile.bin`, the empty `FRONTA.BUN` in this dump, and
the exact FNG field layout remain open research items. Do not guess their
semantics from the current preview UI.

## Reproduction evidence

The measurements above were produced from the existing local data root. The
ignored evidence files are under `scratchpad/m167/`:

- `frontend_asset_inventory.tsv`
- `frontb_strings_clean.txt`
- `frontend_menu_identifiers.txt`
- `frontb_fng_names.txt`

The JDLZ decoder used for the inventory is the repository's existing
`tools/jdlz.py`; no retail bytes were written into `docs/`.
