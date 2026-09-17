"""生成三段萝莉音提示音（英文），使用微软 edge-tts。

音色默认 en-US-AvaNeural（年轻活泼女声），可通过环境变量或 --voice 覆盖：
  en-US-AvaNeural        —— 年轻女声（推荐，明显女性）
  en-US-EmmaNeural       —— 年轻女声，略甜
  en-US-AriaNeural       —— 明亮活泼女声
  en-US-AnaNeural        —— 儿童音（旧默认，实际偏中性/男）
  zh-CN-XiaoshuangNeural —— 中文儿童音（说英文有口音，慎用）

后处理：峰值归一化 → 25ms 淡入淡出 → 首尾 80ms 静音 padding，彻底消除爆音。

输出到 .userdata/sounds/：
  mission_start.wav      —— 切出空闲（开始工作）时播放
  mission_complete.wav   —— 进入空闲（工作结束）时播放
  attention.wav          —— 有提醒时播放，"Attention" × 3，段间加长静音
"""

import argparse
import asyncio
import os
import struct
import subprocess
import sys
import wave
from array import array

import edge_tts

try:
    import imageio_ffmpeg
    FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()
except Exception:
    FFMPEG = "ffmpeg"

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sounds")
os.makedirs(OUT_DIR, exist_ok=True)


async def synth(text: str, out_mp3: str, voice: str, rate: str, pitch: str) -> None:
    tts = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
    await tts.save(out_mp3)


def mp3_to_wav(mp3: str, wav: str, sr: int = 24000, ch: int = 2) -> None:
    """转成【内容采样率 = 硬件真实速率】的 stereo S16LE wav。

    注意：H616 codec 在当前内核（6.18.45 / Armbian trunk）下 LRCK 实际只有
    标称的一半，因此这里按 24000Hz 生成内容，随后由 rewrite_header_rate()
    把 header 改成 48000Hz —— 播放时长与音调即可双双恢复正确。
    """
    subprocess.run(
        [FFMPEG, "-y", "-loglevel", "error",
         "-i", mp3, "-ac", str(ch), "-ar", str(sr), "-c:a", "pcm_s16le", wav],
        check=True,
    )


def rewrite_header_rate(wav_path: str, nominal_sr: int = 48000) -> None:
    """保留 PCM 数据不变，只把 wav header 的采样率改成 nominal_sr。"""
    with wave.open(wav_path, "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        frames = w.readframes(w.getnframes())
    with wave.open(wav_path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(sw)
        w.setframerate(nominal_sr)
        w.writeframes(frames)


def postprocess(wav_path: str,
                fade_ms: int = 25,
                pad_ms: int = 80,
                target_peak: float = 0.92) -> None:
    """归一化 + 淡入淡出 + 首尾静音 padding，就地改写 wav。支持 mono/stereo。"""
    with wave.open(wav_path, "rb") as w:
        sr = w.getframerate()
        sw = w.getsampwidth()
        ch = w.getnchannels()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise RuntimeError(f"expect S16LE, got sw={sw}")

    samples = array("h")
    samples.frombytes(raw)
    total = len(samples)  # n * ch

    # 1) 峰值归一化到 target_peak（≈ -0.7 dBFS）
    peak = max(abs(int(s)) for s in samples) or 1
    scale = (32767.0 * target_peak) / peak
    if abs(scale - 1.0) > 0.01:
        for i in range(total):
            v = int(samples[i] * scale)
            if v > 32767: v = 32767
            elif v < -32768: v = -32768
            samples[i] = v

    # 2) 首尾线性淡入淡出（按帧计算，每帧 ch 个样本）
    fade_frames = min(int(sr * fade_ms / 1000), n // 2)
    for f in range(fade_frames):
        gain = f / fade_frames
        for c in range(ch):
            idx_head = f * ch + c
            idx_tail = (n - 1 - f) * ch + c
            samples[idx_head] = int(samples[idx_head] * gain)
            samples[idx_tail] = int(samples[idx_tail] * gain)

    # 3) 首尾静音 padding
    pad_frames = int(sr * pad_ms / 1000)
    pad_samples = [0] * (pad_frames * ch)
    out = array("h", pad_samples)
    out.extend(samples)
    out.extend(pad_samples)

    with wave.open(wav_path, "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(out.tobytes())


def concat_wavs_with_silence(wavs: list, out_wav: str, gap_ms: int = 800) -> None:
    with wave.open(wavs[0], "rb") as w0:
        sr = w0.getframerate()
        sw = w0.getsampwidth()
        ch = w0.getnchannels()
    gap_frames = int(sr * gap_ms / 1000)
    gap_bytes = b"\x00" * (gap_frames * sw * ch)
    with wave.open(out_wav, "wb") as out:
        out.setnchannels(ch); out.setsampwidth(sw); out.setframerate(sr)
        for i, p in enumerate(wavs):
            with wave.open(p, "rb") as w:
                out.writeframes(w.readframes(w.getnframes()))
            if i < len(wavs) - 1:
                out.writeframes(gap_bytes)


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--voice", default=os.environ.get("DUTYON_VOICE", "en-US-AvaNeural"))
    ap.add_argument("--rate",  default=os.environ.get("DUTYON_RATE",  "+0%"))
    ap.add_argument("--pitch", default=os.environ.get("DUTYON_PITCH", "+4Hz"))
    ap.add_argument("--gap-ms", type=int, default=800, help="Attention 段间隔静音")
    ap.add_argument("--fade-ms", type=int, default=60)
    ap.add_argument("--pad-ms",  type=int, default=200)
    ap.add_argument("--peak",    type=float, default=0.92)
    # 半速率补偿：当前内核 codec LRCK 只有标称一半，默认开启
    ap.add_argument("--compensate", action="store_true", default=True)
    ap.add_argument("--no-compensate", dest="compensate", action="store_false")
    ap.add_argument("--content-sr", type=int, default=24000,
                    help="PCM 内容实际采样率（= 硬件真实速率）")
    ap.add_argument("--header-sr", type=int, default=48000,
                    help="wav header 标称采样率")
    args = ap.parse_args()

    print(f"voice = {args.voice}   rate = {args.rate}   pitch = {args.pitch}")
    print(f"post  = normalize {args.peak} + fade {args.fade_ms}ms + pad {args.pad_ms}ms")
    print(f"rate  = content {args.content_sr}Hz -> header {args.header_sr}Hz "
          f"(compensate={'ON' if args.compensate else 'OFF'})")
    print(f"out   = {OUT_DIR}\n")

    def finish(mp3: str, wav: str) -> None:
        """mp3 -> wav（内容按硬件真实速率）+ 后处理。不做 header 补偿，
        由调用方在最后一步统一 rewrite_header_rate，以免 concat/fade 的
        时长参数误用 header 采样率而翻倍。"""
        mp3_to_wav(mp3, wav, sr=args.content_sr, ch=2)
        postprocess(wav, fade_ms=args.fade_ms, pad_ms=args.pad_ms, target_peak=args.peak)
        os.remove(mp3)

    def finalize(wav: str) -> None:
        if args.compensate:
            rewrite_header_rate(wav, args.header_sr)

    # 1. Mission Start
    mp3 = os.path.join(OUT_DIR, "_ms.mp3")
    wav = os.path.join(OUT_DIR, "mission_start.wav")
    await synth("Mission Start!", mp3, args.voice, args.rate, args.pitch)
    finish(mp3, wav)
    finalize(wav)
    print(f"[1/3] mission_start.wav     {os.path.getsize(wav)} bytes")

    # 2. Mission Complete
    mp3 = os.path.join(OUT_DIR, "_mc.mp3")
    wav = os.path.join(OUT_DIR, "mission_complete.wav")
    await synth("Mission Complete!", mp3, args.voice, args.rate, args.pitch)
    finish(mp3, wav)
    finalize(wav)
    print(f"[2/3] mission_complete.wav  {os.path.getsize(wav)} bytes")

    # 3. Attention × 3：先拼接、再整体后处理，最后才做 header 补偿
    mp3 = os.path.join(OUT_DIR, "_at.mp3")
    single_wav = os.path.join(OUT_DIR, "_at_one.wav")
    await synth("Attention!", mp3, args.voice, args.rate, args.pitch)
    finish(mp3, single_wav)
    final_wav = os.path.join(OUT_DIR, "attention.wav")
    concat_wavs_with_silence([single_wav] * 3, final_wav, gap_ms=args.gap_ms)
    os.remove(single_wav)
    # 拼接后再对整体做一次淡入淡出（target_peak=1.0 不二次增益）
    postprocess(final_wav, fade_ms=args.fade_ms, pad_ms=args.pad_ms, target_peak=1.0)
    finalize(final_wav)
    print(f"[3/3] attention.wav         {os.path.getsize(final_wav)} bytes  "
          f"(3 x Attention! + {args.gap_ms}ms gap)")


if __name__ == "__main__":
    asyncio.run(main())
