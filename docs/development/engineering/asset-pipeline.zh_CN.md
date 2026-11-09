**简体中文** · [English](asset-pipeline.md)

# 资源流水线

固件不在运行期解 PNG。所有美术都事先转成裸像素块和 `*_info.h` 表，再用
`EMBED_FILES` / 生成源文件编进固件。

## 一条命令

```bash
python tools/pack_assets.py
```

- 输入：`assets/art/`（平铺目录，只放美术真源）。
- 输出：`main/themes.bin`、`main/sprites.bin`、`main/signs.bin`、
  `main/themes_info.h`、`main/sprites_info.h`、`main/signs_info.h`。
- 依赖：Pillow。

`*_info.h` 一律不要手改，改完重跑脚本。

## 源文件对照

| 源文件 | 产物 | 格式 |
| --- | --- | --- |
| `card_B_official_warm.png`、`card_A_official_dark.png`、`bg_07_cool.png`、`card_D_ink.png` | `themes.bin` | 240 × 320 RGB565 小端，每套 153,600 B |
| `cat_{idle,curious,sleepy,happy,air,land,sleep,pounce}.png` | `sprites.bin` | RGBA，按 RGB565A8 打包 |
| `pose_roll.png`、`pose_yarn.png`、`cat_close_180.png` | `sprites.bin` | RGBA，画布 180 × 196 |
| `acc_cat_bed_universal.png`、`acc_yarn_ball.png`、`acc_glasses.png`、`acc_cool.png`、`acc_office.png` | `sprites.bin` | RGBA，裁到 alpha 包围盒 |
| `owner_male_01.png`、`owner_male_01_36.png`、`owner_female_01.png`、`owner_female_01_36.png` | `sprites.bin` | 96 × 96 与 36 × 36 RGBA |
| `cat_head_34.png` | `sprites.bin` | 34 × 34 RGBA |
| `sign68_{m,f}.png`、`avatar46_{m,f}.png` | `sprites.bin` | 按原像素尺寸，不缩放 |
| `mark_char_20_{A,B,cool}.png` | `signs.bin` | 裁切后装进 20 × 20 RGB565A8 |

## 脚本保证的事

- **卡面必须是 240 × 320**，尺寸不对直接中止。
- **`themes.bin` 恰好是 `THEME_COUNT × 153,600` B**；`themes_info.h` 里的偏移就是
  `i × 153600`，加一套主题只追加字节。
- **精灵统一 RGB565A8**：先小端 RGB565 平面，再 8 位 alpha 平面，共 `w × h × 3` B；
  `SPR[i].off` 指向 RGB565 平面起点。
- **每个精灵块 4 字节对齐**，再记录下一个偏移。
- **锚点是烘进去的**：眼中心、鼻梁、眼距、眼角角度逐帧存表，固件不缩放脸部配件。
- **缺卡面不会静默通过**：编入带水印的占位卡面，并打印 `!!` 警告。

## 校验主题段

打包后，第 N 套卡面必须与源图转成的 RGB565 小端数据**逐字节一致**（别只看大小）：

```python
DATA = open("main/themes.bin", "rb").read()
seg = DATA[3 * 153600: 4 * 153600]          # 第 4 套，偏移 460,800 = 0x70800
# seg 应等于 assets/art/card_D_ink.png 的 RGB565 小端转换结果
```

## 运行期图片尺寸（不在此打包）

- 门户上传的证件照：**88 × 88 RGB565A8 = 23,232 B**（15,488 B 颜色 + 7,744 B
  alpha），分块写入 NVS，A/B 双 bank。
- 显示链只走整数倍：88 → 176（2×）→ 352（4×）。中间不要插降采样，既费 RAM
  又让证件照发糊。

## 音效

`tools/mk_sounds.py` 从 16 位 WAV 重新生成 `main/snd_*.bin`。原始录音是第三方
媒体，**不**随仓库分发；重跑时用 `FAP_AUDIO_SRC` 指向存放脚本所列源文件的目录。
打包好的 `.bin` 已入库，正常编译不需要 WAV。
