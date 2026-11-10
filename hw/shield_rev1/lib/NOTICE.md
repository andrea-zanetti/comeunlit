# Licences of the library files in this folder

This folder holds project-local copies of the symbols (`shield_rev1.kicad_sym`),
footprints (`shield_rev1.pretty/`) and 3D models (`3dmodels/`) used by the
shield, so the project opens complete without any external library.

These files are **not** covered by the project's CC BY-NC-SA 4.0 licence
(`LICENSE-docs-hw`). They keep the licences of their sources:

## From SnapEDA (SnapMagic Search)

Licensed under [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
with the SnapEDA Design Exception 1.0 (SnapMagic Terms of Use, sections 4.1
and 4.2). Source: <https://www.snapeda.com>.

| Part | Symbol | Footprint | 3D model |
|------|--------|-----------|----------|
| SN74HCT125DRE4 (Texas Instruments) | `SN74HCT125DRE4` | `SOIC127P600X175-14N` | `SN74HCT125DRE4.stp` |
| SN74LVC125ANSR (Texas Instruments; the symbol is used for the fitted SN74LVC125ADR, footprint `SOIC127P600X175-14N`) | `SN74LVC125ANSR` | | |
| SMBJ6.0CA-TR | `SMBJ6.0CA-TR` | `DIONM5436X265N` | `SMBJ6.0CA-TR.stp` |
| nanoASMDCH005F-2 | `nanoASMDCH005F-2` | `FUSC3216X85N` | `nanoASMDCH005F-2.stp` |

At most 10 SnapEDA files are distributed here (SnapMagic Terms of Use,
section 5.1(g)).

## From the KiCad libraries

All other symbols, footprints and 3D models are copies of KiCad 10 library
parts, including `MountingHole_3.2mm` (KiCad's `MountingHole_3.2mm_M3`,
renamed, from the project's former `Arduino_MountingHole` library), licensed
under [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/)
with the KiCad libraries exception, by the KiCad Community.
Source: <https://gitlab.com/kicad/libraries>.

## zmonad logo: all rights reserved

`LOGO` is the zmonad logo, made by Andrea Zanetti from their own artwork.
The zmonad name and logo are **not** licensed under any of the licences in
this repository, CC BY-SA 4.0 and CC BY-NC-SA 4.0 included: all rights
reserved. Unmodified copies of this repository may keep the logo; modified
or derived boards must remove it (delete the `LOGO` footprint from the board
and this library).
