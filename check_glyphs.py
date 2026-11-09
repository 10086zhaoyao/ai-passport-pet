#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_glyphs.py —— 逐字检查待用中文文案在 font_cjk_8 里有没有字模。

背景：LVGL 遇到缺字不画方块，只表现为「短一截」，属于不会被用户报的隐形 bug。
      旧落款「你 和 小咪」就是因为 `咪`(U+54AA) 不在字库，实际渲染成了「你 和 小」。

用法：python check_glyphs.py "小糯米" "你 和 小糯米"
退出码：0 = 全部有字模；1 = 有缺字；2 = 用法错误。
"""
import os
import re
import sys

FONT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'main', 'font_cjk_8.c')


def load_cmap(path):
    """cmap 是 FORMAT0_TINY：unicode_list 存 unicode - 0x20 的稀疏表。"""
    src = open(path, encoding='utf-8').read()
    body = src.split('unicode_list[] = {')[1].split('};')[0]
    return set(int(v) for v in re.findall(r'\d+', body))


def has_glyph(cmap, ch):
    u = ord(ch)
    return 0x20 <= u <= 0xFF5E and (u - 0x20) in cmap


def main():
    if len(sys.argv) < 2:
        print('用法: python check_glyphs.py <文本> [文本...]')
        return 2
    cmap = load_cmap(FONT)
    bad = 0
    for text in sys.argv[1:]:
        marks = ''.join('.' if has_glyph(cmap, c) else 'X' for c in text)
        miss = [c for c in text if not has_glyph(cmap, c)]
        print('%s  %s' % ('OK  ' if not miss else 'MISS', text))
        print('      [%s]' % marks)
        if miss:
            bad = 1
            seen = []
            for c in miss:
                if c not in seen:
                    seen.append(c)
            print('      缺字: ' + '  '.join('%s(U+%04X)' % (c, ord(c)) for c in seen))
    return bad


if __name__ == '__main__':
    sys.exit(main())
