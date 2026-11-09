#!/usr/bin/env python3
"""把用户提供的 4 段真实猫叫转成固件用的 16kHz/16bit/单声道裸 PCM。

- 首尾静音裁掉（-50 dBFS 门限，留 30ms 余量），保留自然起音/收尾
- 峰值归一化到 -3 dBFS
- 首尾各 5ms 淡入淡出，避免爆音（不改音色）
- 呼噜做 15ms 首尾交叉，循环播放无爆点
输出：main/snd_*.bin（可用 FAP_AUDIO_OUT 改），同时打印原始/成品参数。
"""
import os, wave, audioop, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 原始 WAV 属于有版权的录音素材，不随仓库分发；重跑本脚本时把 4 个源文件
# 按下面的文件名放进 SRC 目录（默认 assets/audio/src，需自行创建）。
SRC = os.environ.get("FAP_AUDIO_SRC", os.path.join(ROOT, "assets", "audio", "src"))
OUT = os.environ.get("FAP_AUDIO_OUT", os.path.join(ROOT, "main"))
DST_SR = 16000

FILES = {
    "snd_meow.bin":    "e84e9eaf-ec1f-44ca-8084-d2492b7cd528_01_cat_meow_cute.wav",
    "snd_curious.bin": "de19119e-dec9-4f22-bad8-b2ca3faf9366_04_cat_curious_meow.wav",
    "snd_kitten.bin":  "5e191c54-0ab7-47e2-9884-d3c7428ee31a_03_kitten_tiny_meow.wav",
    "snd_purr.bin":    "304a555e-efda-4d73-bf2b-31e0efa0532a_02_cat_purr.wav",
}


def read_mono(path):
    w = wave.open(path)
    sr, sw, ch, n = w.getframerate(), w.getsampwidth(), w.getnchannels(), w.getnframes()
    raw = w.readframes(n)
    w.close()
    if sw != 2:
        raise SystemExit("只支持 16bit: " + path)
    if ch == 2:
        raw = audioop.tomono(raw, 2, 0.5, 0.5)
    if sr != DST_SR:
        raw, _ = audioop.ratecv(raw, 2, 1, sr, DST_SR, None)
    return raw, sr, sw * 8, ch, n


def peak(raw):
    return audioop.max(raw, 2)


def trim_silence(raw, thresh_db=-50.0, pad_ms=30):
    """按 10ms 窗口判断静音，返回裁掉首尾后的 PCM。"""
    win = DST_SR // 100 * 10 * 2  # 10ms 的字节数
    peak_full = max(1, peak(raw))
    thresh = peak_full * (10 ** (thresh_db / 20.0))
    start = 0
    while start + win <= len(raw) and audioop.max(raw[start:start + win], 2) < thresh:
        start += win
    end = len(raw)
    while end - win >= 0 and audioop.max(raw[end - win:end], 2) < thresh:
        end -= win
    pad = int(DST_SR * pad_ms / 1000) * 2
    return raw[max(0, start - pad):min(len(raw), end + pad)]


def normalize(raw, target_db=-3.0):
    p = peak(raw)
    if p == 0:
        return raw, 0.0
    gain = (10 ** (target_db / 20.0)) * 32767.0 / p
    out = audioop.mul(raw, 2, gain)
    return out, peak(out)


def fade(raw, ms=5):
    n = int(DST_SR * ms / 1000) * 2
    if n * 2 > len(raw):
        n = len(raw) // 4
    head = audioop.mul(raw[:n], 2, 0.0)
    # 线性淡入/淡出
    out = bytearray(raw)
    steps = max(1, n // 2)
    for i in range(steps):
        g = i / steps
        s = struct.unpack_from("<h", out, i * 2)[0]
        struct.pack_into("<h", out, i * 2, int(s * g))
        j = len(out) - 2 - i * 2
        s = struct.unpack_from("<h", out, j)[0]
        struct.pack_into("<h", out, j, int(s * g))
    return bytes(out)


def seamless_loop(raw, ms=15):
    """首尾交叉：把尾部 ms 淡入到开头，循环点无爆音。"""
    n = int(DST_SR * ms / 1000) * 2
    if n * 2 >= len(raw):
        return raw
    body = bytearray(raw[:-n])          # 去掉尾部 n 字节
    tail = raw[-n:]
    # body 开头 n 字节与 tail 交叉
    a = audioop.mul(body[:n], 2, 0.5)   # 粗略等功率交叉
    b = audioop.mul(tail, 2, 0.5)
    mix = struct.pack("<%dh" % (n // 2),
                      *[max(-32768, min(32767, x + y)) for x, y in zip(
                          struct.unpack("<%dh" % (n // 2), a),
                          struct.unpack("<%dh" % (n // 2), b))])
    body[:n] = mix
    return bytes(body)


def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    print(f"{'文件':16s} {'原始':>22s} {'成品':>26s}")
    for dst, src in FILES.items():
        raw, sr, bits, ch, n = read_mono(os.path.join(SRC, src))
        orig_dur = n / sr
        out = trim_silence(raw)
        out = normalize(out)[0]
        out = fade(out, 5)
        if dst == "snd_purr.bin":
            out = seamless_loop(out, 15)
        p = peak(out)
        dur = len(out) / 2 / DST_SR
        open(os.path.join(OUT, dst), "wb").write(out)
        total += len(out)
        print(f"{dst:16s} {sr}Hz/{bits}bit/{ch}ch/{orig_dur:5.2f}s  "
              f"{DST_SR}Hz/16bit/1ch/{dur:5.2f}s {len(out)}B peak={p} "
              f"({20*__import__('math').log10(max(p,1)/32767):.1f}dBFS)")
    print("合计", total, "B")
    # 对照：旧资源（同目录下已存在的三个旧文件）
    old = sum(os.path.getsize(os.path.join(OUT, f))
              for f in ("snd_meow.bin", "snd_purr.bin", "snd_chirp.bin")
              if os.path.exists(os.path.join(OUT, f)))
    print("旧(meow+purr+chirp) =", old, "B; 新增净额约", total - old, "B")


if __name__ == "__main__":
    main()
