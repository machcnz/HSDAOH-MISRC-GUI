# Case panel KiCad fonts

Bundled so the v2.5 case panel KiCad projects render correctly on Windows/macOS
without requiring system-installed fonts. All fonts are SIL Open Font License
(OFL) — see the accompanying *-OFL.txt files.

| Face name in .kicad_pcb | Bundled file | Why |
|---|---|---|
| Carlito | Carlito-Regular/Bold/Italic/BoldItalic.ttf | OFL metric-compatible replacement for Calibri (Calibri is Microsoft proprietary and cannot be redistributed). |
| Lato | Lato-Regular/Bold.ttf | Used 150x in the panel silkscreen. |
| Noto Sans Symbols | NotoSansSymbols-Regular/Bold.ttf | Only source for U+2393 ⎓ (DC power symbol). |
| Inter 18pt Black | Inter_18pt-Black.ttf | Renders U+21BA ↺ / U+21BB ↻ arrows on the gain jog labels (the "Inter Display Black" face lacks these codepoints; the static Inter 18pt Black instance has them). |
| DejaVu Sans | DejaVuSans.ttf / DejaVuSans-Bold.ttf | OFL fallback that also has U+21BA/U+21BB, in case a KiCad build cannot resolve the Inter static instance. |

## Why Calibri was replaced
The upstream PCBs used `(face "Calibri")` for 166 silkscreen text elements.
Calibri is a Microsoft proprietary font (ships with Windows/Office only) and
cannot be legally redistributed in this repository. KiCad on Linux/macOS (and
Windows without Office) substitutes it, shifting the silkscreen text layout.
Carlito is Google's SIL-OFL metric-compatible Calibri replacement — same
metrics, so the silkscreen geometry is preserved.

## Why "Inter Display Black" was repointed
The gain jog labels `↺ -    ↻ +` used `(face "Inter Display Black")`, but that
face does not contain U+21BA ↺ or U+21BB ↻, so KiCad substituted and the
arrows dropped on Windows. Repointed to "Inter 18pt Black" (the repo's static
Inter Black instance), which contains both arrow codepoints.
