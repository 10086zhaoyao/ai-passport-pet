**English** · [简体中文](asset-pipeline.zh_CN.md)

# Asset pipeline

The firmware does not decode PNG at runtime. All artwork is converted ahead of
time into raw pixel blobs and `*_info.h` tables, embedded with
`EMBED_FILES`/generated sources.

## One command

```bash
python tools/pack_assets.py
```

- Input: `assets/art/` (flat directory, art sources only).
- Output: `main/themes.bin`, `main/sprites.bin`, `main/signs.bin`,
  `main/themes_info.h`, `main/sprites_info.h`, `main/signs_info.h`.
- Dependency: Pillow.

Never edit `*_info.h` by hand; rerun the script instead.

## Source files

| Source | Output | Format |
| --- | --- | --- |
| `card_B_official_warm.png`, `card_A_official_dark.png`, `bg_07_cool.png`, `card_D_ink.png` | `themes.bin` | 240 × 320 RGB565 LE, 153,600 B per card |
| `cat_{idle,curious,sleepy,happy,air,land,sleep,pounce}.png` | `sprites.bin` | RGBA, packed as RGB565A8 |
| `pose_roll.png`, `pose_yarn.png`, `cat_close_180.png` | `sprites.bin` | RGBA canvas 180 × 196 |
| `acc_cat_bed_universal.png`, `acc_yarn_ball.png`, `acc_glasses.png`, `acc_cool.png`, `acc_office.png` | `sprites.bin` | RGBA, cropped to alpha bbox |
| `owner_male_01.png`, `owner_male_01_36.png`, `owner_female_01.png`, `owner_female_01_36.png` | `sprites.bin` | 96 × 96 and 36 × 36 RGBA |
| `cat_head_34.png` | `sprites.bin` | 34 × 34 RGBA |
| `sign68_{m,f}.png`, `avatar46_{m,f}.png` | `sprites.bin` | exact pixel size, no scaling |
| `mark_char_20_{A,B,cool}.png` | `signs.bin` | cropped, fitted to 20 × 20 RGB565A8 |

## What the script guarantees

- **Card faces are exactly 240 × 320.** A wrong size aborts the run.
- **`themes.bin` is exactly `THEME_COUNT × 153,600` B.** The theme offset in
  `themes_info.h` is `i × 153600`, so appending a theme only appends bytes.
- **Sprites are RGB565A8**: a little-endian RGB565 plane followed by an 8-bit
  alpha plane, `w × h × 3` bytes each. `SPR[i].off` points at the RGB565 plane.
- **Each sprite blob is 4-byte aligned** before the next offset is recorded.
- **Anchors are baked in**: eye midpoint, nose bridge, eye distance, and eye
  angle are stored per frame; the firmware never rescales a face accessory.
- **Missing card art does not fail silently**: a visibly watermarked placeholder
  is packed and a `!!` warning is printed.

## Verifying a theme segment

After packing, the Nth card must be byte-identical to the source image converted
to RGB565 little-endian — compare bytes, not just sizes:

```python
DATA = open("main/themes.bin", "rb").read()
seg = DATA[3 * 153600: 4 * 153600]          # 4th card, offset 460,800 = 0x70800
# seg must equal the RGB565 LE conversion of assets/art/card_D_ink.png
```

## Runtime image sizes (not packed here)

- Portrait avatar uploaded through the portal: **88 × 88 RGB565A8 = 23,232 B**
  (15,488 B color + 7,744 B alpha), written to NVS in chunks with A/B banks.
- Display scaling is integer-only: 88 → 176 (2×) → 352 (4×). Do not insert a
  downscale step in between; it costs RAM and softens the portrait.

## Sound effects

`tools/mk_sounds.py` regenerates `main/snd_*.bin` from 16-bit WAV sources. The
original recordings are third-party media and are **not** distributed with this
repository; set `FAP_AUDIO_SRC` to a directory holding the source files listed in
the script. The packed `.bin` files are committed, so a normal build does not
need the WAVs.
