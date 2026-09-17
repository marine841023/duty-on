import wave, math, array

SR = 44100
BEAT = 0.5  # 120 BPM, one beat = 0.5s

# Ode to Joy melody, C major (E E F G | G F E D | C C D E | E. D D) x2
C4 = 261.63; D4 = 293.66; E4 = 329.63; F4 = 349.23; G4 = 392.00
C3 = 130.81; F3 = 174.61; G2 = 98.00; G3 = 196.00

melody = [
    (E4,1),(E4,1),(F4,1),(G4,1),
    (G4,1),(F4,1),(E4,1),(D4,1),
    (C4,1),(C4,1),(D4,1),(E4,1),
    (E4,1.5),(D4,0.5),(D4,2),
    (E4,1),(E4,1),(F4,1),(G4,1),
    (G4,1),(F4,1),(E4,1),(D4,1),
    (C4,1),(C4,1),(D4,1),(E4,1),
    (D4,1.5),(C4,0.5),(C4,2),
]
bass = [
    (C3,4),(G2,4),(C3,4),(G3,4),
    (C3,4),(G2,4),(F3,4),(C3,4),
]

total = sum(b for _, b in melody)
dur = total * BEAT
N = int(SR * dur)


def env_apply(n, atk, rel):
    pass


try:
    import numpy as np
    engine = "numpy"
    buf = np.zeros(N, dtype=np.float64)

    def add(f, s, d, vol, bright):
        i0 = int(s * SR); i1 = min(N, int((s + d) * SR)); n = i1 - i0
        if n <= 0:
            return
        t = np.arange(n, dtype=np.float64) / SR
        x = 2 * np.pi * f * t
        if bright:
            v = np.sin(x) + 0.35 * np.sin(2 * x) + 0.15 * np.sin(3 * x)
        else:
            v = np.sin(x) + 0.25 * np.sin(2 * x)
        atk = int(0.015 * SR); rel = int(0.08 * SR)
        e = np.ones(n)
        if 0 < atk < n:
            e[:atk] = np.linspace(0, 1, atk)
        if 0 < rel < n:
            e[n - rel:] *= np.linspace(1, 0, rel)
        buf[i0:i1] += e * v * vol

    t = 0.0
    for f, b in melody:
        add(f, t, b * BEAT, 0.30, True); t += b * BEAT
    t = 0.0
    for f, b in bass:
        add(f, t, b * BEAT, 0.20, False); t += b * BEAT

    peak = float(np.max(np.abs(buf))) or 1.0
    y = np.clip(buf * (0.88 / peak), -1.0, 1.0)
    si = (y * 32767).astype(np.int16)
    stereo = np.column_stack((si, si))
    data = stereo.tobytes()
except ImportError:
    engine = "pure-python"
    SR = 22050
    N = int(SR * dur)
    buf = array.array('f', bytes(4 * N))
    pi2 = 2 * math.pi; sin = math.sin

    def add(f, s, d, vol, bright):
        i0 = int(s * SR); i1 = min(N, int((s + d) * SR)); n = i1 - i0
        if n <= 0:
            return
        atk = int(0.015 * SR); rel = int(0.08 * SR); w = pi2 * f / SR
        for k in range(n):
            x = w * k
            if k < atk:
                e = k / atk
            elif k > n - rel:
                e = (n - k) / rel
            else:
                e = 1.0
            v = sin(x)
            if bright:
                v += 0.35 * sin(2 * x) + 0.15 * sin(3 * x)
            else:
                v += 0.25 * sin(2 * x)
            buf[i0 + k] += e * v * vol

    t = 0.0
    for f, b in melody:
        add(f, t, b * BEAT, 0.30, True); t += b * BEAT
    t = 0.0
    for f, b in bass:
        add(f, t, b * BEAT, 0.20, False); t += b * BEAT

    peak = 0.0
    for x in buf:
        a = x if x > 0 else -x
        if a > peak:
            peak = a
    peak = peak or 1.0
    sc = 0.88 / peak
    fr = array.array('h')
    for x in buf:
        v = int(x * sc * 32767)
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        fr.append(v); fr.append(v)
    data = fr.tobytes()

w = wave.open('/tmp/song.wav', 'wb')
w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR)
w.writeframes(data); w.close()
print("engine=" + engine + " SR=" + str(SR) + " dur=" + str(round(dur, 1)) + "s -> /tmp/song.wav")
