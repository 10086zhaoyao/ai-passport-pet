#!/usr/bin/env python3
"""资源打包：把 assets/art 下的美术真源编成固件要用的 bin + 头文件。

产出（写入 main/）：
  themes.bin       4 套卡面 × 153,600 B = 614,400 B
  sprites.bin      猫帧/姿态/猫窝/配件/形象/落款，统一 RGB565A8
  signs.bin        3 个 20×20 签名 mark（RGB565A8）
  themes_info.h / sprites_info.h / signs_info.h   自动生成，请勿手改

规则：卡面 RGB565（240x320x2=153600B），精灵统一 RGB565A8（先 RGB565 平面，再 A8）。
所有坐标都在 180x196 的猫画布坐标系里标定，固件按画布原点摆放，不做额外偏移。

用法：python tools/pack_assets.py
依赖：Pillow。缺卡面时会编入可见的「占位」卡面并打印 !! 警告，不会静默通过。
"""
import os
import struct
import math
from PIL import Image, ImageDraw, ImageFont


def owner_placeholder(tag):
    """96x96 透明底 Q 版占位（男/女）。美术 PNG 到位后自动优先用真图。"""
    S = 4
    img = Image.new("RGBA", (96 * S, 96 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    skin = (247, 205, 170, 255)
    hair = (46, 42, 40, 255)
    cloth = (91, 141, 239, 255) if tag == "m" else (232, 121, 168, 255)
    dark = (58, 44, 38, 255)
    # 肩/身体
    d.rounded_rectangle([16 * S, 64 * S, 80 * S, 100 * S], radius=18 * S, fill=cloth)
    # 脖子
    d.rectangle([42 * S, 56 * S, 54 * S, 70 * S], fill=skin)
    # 头
    d.ellipse([27 * S, 17 * S, 69 * S, 65 * S], fill=skin)
    if tag == "m":
        d.chord([25 * S, 12 * S, 71 * S, 48 * S], 180, 360, fill=hair)
        d.rectangle([25 * S, 30 * S, 29 * S, 40 * S], fill=hair)
        d.rectangle([67 * S, 30 * S, 71 * S, 40 * S], fill=hair)
    else:
        d.ellipse([22 * S, 12 * S, 74 * S, 70 * S], fill=hair)
        d.ellipse([28 * S, 20 * S, 68 * S, 62 * S], fill=skin)
        d.chord([24 * S, 10 * S, 72 * S, 46 * S], 180, 360, fill=hair)
        d.ellipse([64 * S, 18 * S, 78 * S, 32 * S], fill=(240, 90, 140, 255))
    # 眼睛 + 嘴
    d.ellipse([38 * S, 38 * S, 43 * S, 44 * S], fill=dark)
    d.ellipse([53 * S, 38 * S, 58 * S, 44 * S], fill=dark)
    d.arc([42 * S, 46 * S, 54 * S, 56 * S], 20, 160, fill=dark, width=1 * S)
    d.ellipse([35 * S, 48 * S, 42 * S, 52 * S], fill=(244, 160, 160, 120))
    d.ellipse([54 * S, 48 * S, 61 * S, 52 * S], fill=(244, 160, 160, 120))
    return img.resize((96, 96), Image.LANCZOS)

def cjk_font(size):
    """占位卡面要写中文；找不到系统字体就退默认字体（只是占位，不追求字形）。"""
    for p in (r"C:\Windows\Fonts\msyhbd.ttc", r"C:\Windows\Fonts\msyh.ttc",
              r"C:\Windows\Fonts\simhei.ttf",
              "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
              "/System/Library/Fonts/PingFang.ttc"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except OSError:
                pass
    return ImageFont.load_default()


def ink_placeholder():
    """第 4 套「彩色水墨」占位卡面（240x320）。

    美术出图前顶位置：宣纸底 + 墨线框 + 朱砂重彩标记，并刻意铺满「占位 / PLACEHOLDER」
    水印与区域标注，保证一眼看出是未完成稿，不会被误当成成品。
    真图到位后把 card_D_ink.png 放进 art2 即可，本函数自然不再被调用。
    """
    W, H = 240, 320
    paper = (0xF4, 0xEF, 0xE4)      # 宣纸底
    ink = (0x1C, 0x1A, 0x17)        # 墨线
    accent = (0xB0, 0x3A, 0x2E)     # 朱砂 / 重彩
    soft = (0x6B, 0x61, 0x56)
    img = Image.new("RGB", (W, H), paper)
    d = ImageDraw.Draw(img, "RGBA")

    # 宣纸纤维：极浅斜纹，压在水印下面
    for x in range(-H, W, 7):
        d.line([(x, 0), (x + H, H)], fill=(0xD8, 0xCE, 0xB8, 46), width=1)

    # 斜向水印
    f_wm = cjk_font(26)
    wm = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    wd = ImageDraw.Draw(wm)
    for row, y in enumerate(range(-30, H, 62)):
        for x in range(-70, W, 156):
            wd.text((x + (row % 2) * 74, y), "占位", font=f_wm, fill=(0xB0, 0x3A, 0x2E, 70))
    wm = wm.rotate(-25, resample=Image.BICUBIC, expand=True)
    lw, lh = wm.size
    wm = wm.crop(((lw - W) // 2, (lh - H) // 2, (lw - W) // 2 + W, (lh - H) // 2 + H))
    img.paste(wm, (0, 0), wm)

    # 墨线边框：外 3px + 内 1px
    d = ImageDraw.Draw(img, "RGBA")
    d.rounded_rectangle([7, 7, W - 8, H - 8], radius=10, outline=ink, width=3)
    d.rounded_rectangle([14, 14, W - 15, H - 15], radius=8, outline=ink, width=1)

    # 区域标注：插画区 / 文字牌 / 证件照框（与 HOME_LAYOUT / 头像框坐标一致）
    f_s = cjk_font(12)
    f_xs = cjk_font(10)
    d.rectangle([30, 46, 210, 242], outline=accent, width=1)
    d.text((36, 52), "卡面插画区", font=f_s, fill=accent)
    d.text((36, 70), "ART PENDING", font=f_xs, fill=accent)
    d.rectangle([24, 240, 174, 300], outline=soft, width=1)
    d.text((28, 302), "文字牌 24,247", font=f_xs, fill=soft)
    d.rectangle([182, 246, 228, 292], outline=soft, width=1)
    d.text((184, 294), "46", font=f_xs, fill=soft)

    # 中心大字：占位
    f_big = cjk_font(44)
    d.text((W // 2, 150), "占位", font=f_big, fill=(0xB0, 0x3A, 0x2E, 120), anchor="mm")
    d.text((W // 2, 190), "彩色水墨 · 待美术出图", font=cjk_font(13),
           fill=(0xB0, 0x3A, 0x2E, 200), anchor="mm")
    d.text((W // 2, 212), "PLACEHOLDER", font=f_xs, fill=soft, anchor="mm")
    return img


# 仓库内相对路径：assets/art 是美术真源（全部平铺），main 是生成物落点。
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART = os.path.join(ROOT, "assets", "art")
ART1 = ART
ART2 = ART
OUT = os.path.join(ROOT, "main")
CANVAS = (180, 196)

# 四套卡面：默认暖白、深色、酷黑、彩色水墨
CARDS = [
    ("暖白官方", "card_B_official_warm.png"),
    ("深色官方", "card_A_official_dark.png"),
    ("酷黑工业", "bg_07_cool.png"),
    # 缺文件时自动编入醒目的「占位」卡面（见 ink_placeholder）并打印 !! 警告。
    ("彩色水墨", "card_D_ink.png"),
]

# 三套文字色：美术交付定稿（WCAG 实测值），不再自行推算
THEME_COLORS = [
    dict(bg=0xE2D6C2, card=0xF2EBDF, ink=0x2B2620, ink2=0x6B6153, muted=0x706555,
         screen_ink=0x2B2620, screen_muted=0x6B6153, hdr=0xF2EBDF, hdr_ink=0x2B2620,
         accent=0xC4633A),
    dict(bg=0x0E181E, card=0x18252E, ink=0xF2EDE3, ink2=0xC3D0D3, muted=0x9AA9AD,
         screen_ink=0xF2EDE3, screen_muted=0xC3D0D3, hdr=0x18252E, hdr_ink=0xF2EDE3,
         accent=0xE08A4A),
    dict(bg=0x0B0E0F, card=0xE7EBE4, ink=0x101414, ink2=0x3F4A45, muted=0x55635B,
         screen_ink=0xE8EAE9, screen_muted=0x8E9492, hdr=0x15191B, hdr_ink=0xE8EAE9,
         accent=0xB6E51B),
    # 彩色水墨：宣纸底 + 墨线文字 + 朱砂重彩。美术交付定稿（theme_ink README），
    # muted / screen_muted 取收紧后的值，其余 8 键与美术给定一致。
    dict(bg=0xE7DDC8, card=0xF4EFE4, ink=0x1C1A17, ink2=0x5A5147, muted=0x655C51,
         screen_ink=0x1C1A17, screen_muted=0x5A5147, hdr=0xF4EFE4, hdr_ink=0x1C1A17,
         accent=0xB03A2E),
]

# 首页锚点（美术定稿）：A/B 共用一套，酷卡用自己的文字牌。
# 头像框占掉右侧竖条（182..228 / 酷卡 174..220），姓名宽度收紧到框左；签名 mark 顺势挪到次级行右端。
HOME_B = dict(name=(24, 247, 150), sub=(24, 275, 130), micro=(12, 228, 305),
              mark=(158, 274), av_x=182, name_px=24)
HOME_C = dict(name=(34, 252, 132), sub=(34, 281, 112), micro=(34, 228, 293),
              mark=(150, 279), av_x=174, name_px=22)
# 彩色水墨：沿用 A/B 的文字牌与头像框位置（已按美术终稿卡面复核）。
HOME_D = dict(name=(24, 247, 150), sub=(24, 275, 130), micro=(12, 228, 305),
              mark=(158, 274), av_x=182, name_px=24)
HOME_LAYOUTS = [HOME_B, HOME_B, HOME_C, HOME_D]

# 8 帧既有猫：逐帧标定的左右眼与鼻梁（画布 180x196），沿用已验证数据
ORDER = ["idle", "curious", "sleepy", "happy", "air", "land", "sleep", "pounce"]
EYES = {
    "idle":    ((57, 97), (106, 98)),
    "curious": ((45, 103), (93, 101)),
    "sleepy":  ((52, 133), (103, 121)),
    "happy":   ((40, 102), (88, 100)),
    "air":     ((93, 47), (126, 48)),
    "land":    ((67, 161), (115, 160)),
    "sleep":   ((65, 153), (109, 154)),
    "pounce":  ((39, 117), (79, 118)),
}
NOSE = {
    "idle":    (79, 102),
    "curious": (67, 107),
    "sleepy":  (81, 132),
    "happy":   (64, 108),
    "air":     (112, 50),
    "land":    (90, 166),
    "sleep":   (87, 157),
    "pounce":  (58, 121),
}

# 新姿态：都是眯眼/侧躺，眼镜层按侧脸处理（face=1 隐藏），避免两片镜片平铺穿模
POSES = {"roll": "pose_roll.png", "yarn": "pose_yarn.png"}

# 配件：画布对齐图裁到 bbox；脸部配件按 idle 鼻梁 (79,102)、瞳距 49 做锚点映射
ACC_REF_NOSE = (79, 102)
ACC_REF_EYE_D = 49.0
ACC_REF_EYE_ANG = 12          # 0.1 度，idle 双眼连线
GLASS_BRIDGE_CANVAS = (79, 102)
COOL_FACE_BRIDGE_CANVAS = (79, 102)


def rgb565(v):
    r = (v >> 16) & 0xFF
    g = (v >> 8) & 0xFF
    b = v & 0xFF
    return struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))


def lum(v):
    def ch(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r = ch((v >> 16) & 0xFF)
    g = ch((v >> 8) & 0xFF)
    b = ch(v & 0xFF)
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def contrast(a, b):
    la, lb = lum(a), lum(b)
    if la < lb:
        la, lb = lb, la
    return (la + 0.05) / (lb + 0.05)


def to_rgb565(img):
    img = img.convert("RGB")
    w, h = img.size
    px = img.load()
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
            i += 2
    return bytes(out)


def rgb565a8(img):
    img = img.convert("RGBA")
    w, h = img.size
    px = img.load()
    col = bytearray()
    alp = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            col += struct.pack("<H", v)
            alp.append(a)
    return bytes(col + alp)


def canvas_cat(name):
    img = Image.open(os.path.join(ART1, f"cat_{name}.png")).convert("RGBA")
    if img.size != CANVAS:
        c = Image.new("RGBA", CANVAS, (0, 0, 0, 0))
        c.paste(img, ((CANVAS[0] - img.width) // 2, CANVAS[1] - img.height), img)
        img = c
    return img


def load_canvas(path):
    img = Image.open(path).convert("RGBA")
    if img.size != CANVAS:
        c = Image.new("RGBA", CANVAS, (0, 0, 0, 0))
        c.paste(img, (0, 0), img)
        img = c
    return img


def crop_alpha(img, pad=0):
    bb = img.split()[3].getbbox()
    if bb is None:
        return img, (0, 0)
    x0 = max(0, bb[0] - pad)
    y0 = max(0, bb[1] - pad)
    x1 = min(img.width, bb[2] + pad)
    y1 = min(img.height, bb[3] + pad)
    return img.crop((x0, y0, x1, y1)), (x0, y0)


def main():
    os.makedirs(OUT, exist_ok=True)

    # ---------------- 卡面 ----------------
    theme_blob = bytearray()
    for (label, fn), col in zip(CARDS, THEME_COLORS):
        p = os.path.join(ART2, fn)
        if os.path.exists(p):
            img = Image.open(p).convert("RGB")
        else:
            # 美术还没出图：编入占位卡面。只打警告、不落盘，避免污染 assets/art 真源目录。
            img = ink_placeholder()
            print(f"  !! {label}: {fn} 缺图，已用占位卡面（不可发布）")
        if img.size != (240, 320):
            raise SystemExit(f"{fn} 尺寸不对: {img.size}")
        raw = to_rgb565(img)
        assert len(raw) == 153600, len(raw)
        theme_blob += raw
        for key in ("ink", "ink2", "muted"):
            c = contrast(col[key], col["card"])
            tag = "OK " if c >= 4.5 else "LOW"
            print(f"  {label} {key:5s} {c:4.2f}:1 {tag}")

    open(os.path.join(OUT, "themes.bin"), "wb").write(bytes(theme_blob))

    rows = ",\n    ".join(
        '{"%s", %d, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X, 0x%06X}' % (
            label, i * 153600, c["bg"], c["card"], c["ink"], c["ink2"], c["muted"],
            c["screen_ink"], c["screen_muted"], c["hdr"], c["hdr_ink"], c["accent"])
        for i, ((label, _), c) in enumerate(zip(CARDS, THEME_COLORS)))

    layout_rows = ",\n    ".join(
        "{" + ", ".join(str(v) for v in (
            list(L["name"]) + list(L["sub"]) + list(L["micro"]) + list(L["mark"]) +
            [L["av_x"], 246, 46, L["name_px"]])) + "}"
        for L in HOME_LAYOUTS)

    hdr = f"""// themes_info.h —— 由 pack_v110.py 自动生成，请勿手改。
#pragma once
#include <stdint.h>

#define THEME_COUNT {len(CARDS)}
#define THEME_W 240
#define THEME_H 320
#define THEME_BYTES 153600

// 首页公共锚点（美术定稿，240x320 绝对坐标）。旧 nick y182 / sign y204 / UNIT y238 作废。
#define HOME_STAGE_X 30
#define HOME_STAGE_Y 46
#define HOME_STAGE_W 180
#define HOME_STAGE_H 196
#define HOME_TIME_X 12
#define HOME_TIME_Y 3
#define HOME_BATT_RIGHT 228
#define HOME_BATT_Y 12
#define HOME_BAND_LINE_Y 298
#define HOME_BAND_Y 298

typedef struct {{
    const char *name;   // 设置页显示名（中文）
    uint32_t off;       // themes.bin 内偏移（RGB565 小端，不透明）
    uint32_t bg;        // 机身边框底色
    uint32_t card;      // 卡面底色
    uint32_t ink;       // 卡面主文字
    uint32_t ink2;      // 卡面次级文字
    uint32_t muted;     // 卡面标签/刻度
    uint32_t screen_ink;    // 机身主文字
    uint32_t screen_muted;  // 机身次文字
    uint32_t hdr;       // 抬头带底色
    uint32_t hdr_ink;   // 抬头带文字
    uint32_t accent;    // 唯一点色
}} theme_info_t;

static const theme_info_t THEMES[THEME_COUNT] = {{
    {rows}
}};

// 逐主题文字锚点：A/B 共用（24,247）/（24,275），酷卡用文字牌（34,252）/（34,281），
// 彩色水墨暂沿用 A/B（美术未出图）。
// 头像框：A/B (182,246)，酷卡整体内移 8px → (174,246)，均 46x46、radius 8、1px 主题描边。
typedef struct {{
    int16_t name_x, name_y, name_w;
    int16_t sub_x, sub_y, sub_w;
    int16_t micro_l_x, micro_r_right, micro_y;
    int16_t mark_x, mark_y;
    int16_t av_x, av_y, av_sz;
    uint8_t name_px;
}} home_layout_t;

static const home_layout_t HOME_LAYOUT[THEME_COUNT] = {{
    {layout_rows}
}};
"""
    open(os.path.join(OUT, "themes_info.h"), "w", encoding="utf-8").write(hdr)
    print("themes.bin", len(theme_blob), "B")

    # ---------------- 精灵 ----------------
    blobs, meta = [], []

    def add(name, img, ox, oy, kind, eye=(0, 0), nose=(0, 0), eye_d=0, ang=0, face=0):
        data = rgb565a8(img)
        blobs.append(data)
        meta.append(dict(name=name, w=img.width, h=img.height, ox=ox, oy=oy, eye=eye,
                         nose=nose, eye_d=eye_d, ang=ang, face=face, kind=kind))
        print(f"  {name:12s} {img.width:3d}x{img.height:3d} @({ox},{oy}) {len(data)}B")

    for name in ORDER:
        full = canvas_cat(name)
        (lx, ly), (rx, ry) = EYES[name]
        nx, ny = NOSE[name]
        img, (x, y) = crop_alpha(full)
        ed = math.hypot(rx - lx, ry - ly)
        ang = math.degrees(math.atan2(ry - ly, rx - lx))
        add(f"cat_{name}", img, x, y, 0,
            eye=(round((lx + rx) / 2 - x), round((ly + ry) / 2 - y)),
            nose=(round(nx - x), round(ny - y)), eye_d=round(ed), ang=round(ang * 10))

    for name, fn in POSES.items():
        img, (x, y) = crop_alpha(load_canvas(os.path.join(ART2, fn)))
        add(f"pose_{name}", img, x, y, 0, face=1)

    bed = load_canvas(os.path.join(ART2, "acc_cat_bed_universal.png"))
    add("bed_universal", bed, 0, 0, 1)

    ball, (bx, by) = crop_alpha(load_canvas(os.path.join(ART2, "acc_yarn_ball.png")))
    add("yarn_ball", ball, bx, by, 4)

    gl, (gx, gy) = crop_alpha(load_canvas(os.path.join(ART2, "acc_glasses.png")))
    gbx, gby = GLASS_BRIDGE_CANVAS[0] - gx, GLASS_BRIDGE_CANVAS[1] - gy
    add("acc_glasses", gl, gx, gy, 2)

    cool = load_canvas(os.path.join(ART2, "acc_cool.png"))
    # 脸部（墨镜）与身体（外套）分开：脸跟随鼻梁旋转，身体只在对齐帧出现
    cool_face = cool.crop((0, 78, 180, 122))
    cf, (cfx, cfy) = crop_alpha(cool_face)
    cfx, cfy = cfx + 0, cfy + 78
    add("acc_cool_face", cf, cfx, cfy, 2)
    cool_body = cool.crop((0, 120, 180, 196))
    cb, (cbx, cby) = crop_alpha(cool_body)
    cby += 120
    add("acc_cool_body", cb, cbx, cby, 3)

    off, (ox, oy) = crop_alpha(load_canvas(os.path.join(ART2, "acc_office.png")))
    add("acc_office", off, ox, oy, 3)

    # 主人形象：美术 96x96 透明底 PNG（assets/art）；另有 36px 预缩版给场景合影位。
    for tag, fn, fn36 in (("m", "owner_male_01.png", "owner_male_01_36.png"),
                          ("f", "owner_female_01.png", "owner_female_01_36.png")):
        p = os.path.join(ART2, fn)
        img = Image.open(p).convert("RGBA") if os.path.exists(p) else owner_placeholder(tag)
        img = img.resize((96, 96), Image.LANCZOS)
        add(f"avatar_{tag}", img, 0, 0, 5)
        p36 = os.path.join(ART2, fn36)
        small = (Image.open(p36).convert("RGBA").resize((36, 36), Image.LANCZOS)
                 if os.path.exists(p36) else img.resize((36, 36), Image.LANCZOS))
        add(f"avatar_{tag}_36", small, 0, 0, 5)
        src = "美术素材" if os.path.exists(p) else "占位"
        print(f"    ({src}) {fn}")

    # 猫头小像：优先用美术给的 34px 版，正好塞进 36px 圆框（1px 描边）。
    head34 = os.path.join(ART2, "cat_head_34.png")
    if os.path.exists(head34):
        head = Image.open(head34).convert("RGBA").resize((34, 34), Image.LANCZOS)
    else:
        head = canvas_cat("idle").crop((34, 8, 146, 120)).resize((34, 34), Image.LANCZOS)
    add("cat_head", head, 0, 0, 5)

    # 落款署名区（美术终稿）：合影 68x34 与卡面头像 46x46 都按原始像素打包。
    # 两张图都有画到画布边缘的内容（sign68 第 34 行仍有像素、avatar46 自带 1px 圆环），
    # 所以这里不裁、不补透明边，固件侧也必须 1:1、不缩放地贴。
    signoff = ART2
    for tag, fn, size, kind in (("f", "sign68_f.png", (68, 34), 6),
                                ("m", "sign68_m.png", (68, 34), 6),
                                ("f", "avatar46_f.png", (46, 46), 6),
                                ("m", "avatar46_m.png", (46, 46), 6)):
        name = f"sign_{tag}" if size[0] == 68 else f"avatar46_{tag}"
        img = Image.open(os.path.join(signoff, fn)).convert("RGBA")
        if img.size != size:
            raise SystemExit(f"{fn} 尺寸不对: {img.size}，应为 {size}")
        add(name, img, 0, 0, kind)

    # 特写帧（预渲染近景，替换运行时的 1.8x 缩放）：追加在末尾，绝不动既有索引。
    # 特写态会隐藏配件层，眼位/鼻梁沿用 idle 的标定值作占位。
    close_src = os.path.join(ART, "cat_close_180.png")
    ci, (cx, cy) = crop_alpha(load_canvas(close_src))
    (ilx, ily), (irx, iry) = EYES["idle"]
    add("cat_close", ci, cx, cy, 0,
        eye=(round((ilx + irx) / 2 - cx), round((ily + iry) / 2 - cy)),
        nose=(NOSE["idle"][0] - cx, NOSE["idle"][1] - cy),
        eye_d=round(math.hypot(irx - ilx, iry - ily)),
        ang=round(math.degrees(math.atan2(iry - ily, irx - ilx)) * 10))

    blob = bytearray()
    for m, b in zip(meta, blobs):
        m["off"] = len(blob)
        blob += b
        while len(blob) % 4:
            blob.append(0)
    open(os.path.join(OUT, "sprites.bin"), "wb").write(bytes(blob))

    enum = ",\n    ".join("SPR_" + m["name"].upper() for m in meta)
    srows = ",\n    ".join(
        "{%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d}" % (
            m["off"], m["w"], m["h"], m["ox"], m["oy"], m["eye"][0], m["eye"][1],
            m["nose"][0], m["nose"][1], m["eye_d"], m["ang"], m["face"], m["kind"])
        for m in meta)
    shdr = f"""// sprites_info.h —— 由 pack_v110.py 自动生成，请勿手改。
#pragma once
#include <stdint.h>

#define SPR_COUNT {len(meta)}
#define SPR_FRAME_COUNT {len(ORDER) + len(POSES)}
#define SPR_FRAME_IDLE SPR_IDLE

// 配件锚点：脸部图按 idle 鼻梁/瞳距标定，逐帧映射沿用旧眼镜的鼻梁锚点算法。
#define ACC_REF_NOSE_X {ACC_REF_NOSE[0]}
#define ACC_REF_NOSE_Y {ACC_REF_NOSE[1]}
#define ACC_REF_EYE_D {int(ACC_REF_EYE_D)}
#define ACC_REF_EYE_ANG {ACC_REF_EYE_ANG}
#define ACC_GLASS_BRIDGE_X {gbx}
#define ACC_GLASS_BRIDGE_Y {gby}
#define ACC_COOL_BRIDGE_X {GLASS_BRIDGE_CANVAS[0] - cfx}
#define ACC_COOL_BRIDGE_Y {GLASS_BRIDGE_CANVAS[1] - cfy}

enum {{
    {enum}
}};

// 短名别名：业务代码沿用旧命名（SPR_IDLE / SPR_SLEEP / SPR_AIR…）。
#define SPR_IDLE    SPR_CAT_IDLE
#define SPR_CURIOUS SPR_CAT_CURIOUS
#define SPR_SLEEPY  SPR_CAT_SLEEPY
#define SPR_HAPPY   SPR_CAT_HAPPY
#define SPR_AIR     SPR_CAT_AIR
#define SPR_LAND    SPR_CAT_LAND
#define SPR_SLEEP   SPR_CAT_SLEEP
#define SPR_POUNCE  SPR_CAT_POUNCE
#define SPR_CLOSE   SPR_CAT_CLOSE

typedef struct {{
    uint32_t off;   // sprites.bin 内偏移（RGB565A8：先 RGB565 平面，再 A8 平面）
    uint16_t w, h;
    int16_t ox, oy;   // 在 180x196 画布内的左上角
    int16_t eye_x, eye_y;  // 两眼连线中点（相对图像左上角），逐帧标定
    int16_t nose_x, nose_y;// 鼻梁点（相对图像左上角）：镜桥必须落在这里
    uint16_t eye_d;        // 两眼瞳孔间距（像素），决定眼镜缩放
    int16_t eye_ang;       // 两眼连线角度（0.1 度，顺时针为正），决定眼镜旋转
    uint8_t face;          // 1=侧脸/眯眼帧：不画面部配件
    uint8_t kind;          // 0=猫帧 1=猫窝 2=脸部配件 3=身体配件 4=道具
}} spr_info_t;

static const spr_info_t SPR[SPR_COUNT] = {{
    {srows}
}};
"""
    open(os.path.join(OUT, "sprites_info.h"), "w", encoding="utf-8").write(shdr)
    print("sprites.bin", len(blob), "B")

    # ---------------- 签名 mark ----------------
    marks, mrows = [], []
    for tag, fn in (("B", "mark_char_20_B.png"), ("A", "mark_char_20_A.png"),
                    ("cool", "mark_char_20_cool.png")):
        img = Image.open(os.path.join(ART2, fn)).convert("RGBA")
        bb = img.split()[3].getbbox()
        if bb:
            img = img.crop(bb)
        tw = 20
        th = max(1, min(20, round(img.height * tw / img.width)))
        img = img.resize((tw, th), Image.LANCZOS)
        cv = Image.new("RGBA", (20, 20), (0, 0, 0, 0))
        cv.paste(img, (0, 20 - th), img)
        data = rgb565a8(cv)
        marks.append(data)
        mrows.append(len(cv.tobytes()))
        print(f"  mark_{tag} 20x20 {len(data)}B")

    mblob = bytearray()
    offs = []
    for b in marks:
        offs.append(len(mblob))
        mblob += b
    open(os.path.join(OUT, "signs.bin"), "wb").write(bytes(mblob))
    mhdr = f"""// signs_info.h —— 由 pack_v110.py 自动生成，请勿手改。
#pragma once

#define SIGN_COUNT {len(marks)}
#define SIGN_W 20
#define SIGN_H 20
#define SIGN_BYTES {len(marks[0])}

static const uint32_t SIGN_OFF[SIGN_COUNT] = {{ {", ".join(str(o) for o in offs)} }};
"""
    open(os.path.join(OUT, "signs_info.h"), "w", encoding="utf-8").write(mhdr)
    print("signs.bin", len(mblob), "B")


if __name__ == "__main__":
    main()
