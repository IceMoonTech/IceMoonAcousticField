"""Create deterministic audition input under this managed copy's Saved directory.

No UE launch or asset import. PrepareAudition imports this file as a SoundWave.
The integer waveform avoids random-library/libm differences between machines.
Existing identical input is verified; a different file is never overwritten.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

RATE = 48000
SECONDS = 8
NAME = 'IM_V2Reference.wav'


def reference_pcm():
    """Three gated broadband bursts, a decay gap, then a sustained multitone.

    0..3 s: 0.5 s bursts once per second; 3..4.5 s: silence;
    4.5..7 s: continuous tones; 7..8 s: silence. The loop boundary is silent.
    Frequencies 233/997/3109 Hz match the native smoke's three-band stimulus.
    """
    pcm = bytearray()
    state, filtered = 0x0693481, 0
    half = RATE // 2
    fade_in, fade_out = RATE // 200, RATE // 20  # 5 ms / 50 ms, in samples.
    for n in range(RATE * SECONDS):
        # Explicit uint32 xorshift; no ambient RNG state participates.
        state ^= (state << 13) & 0xffffffff
        state ^= state >> 17
        state ^= (state << 5) & 0xffffffff
        state &= 0xffffffff
        filtered = (3 * filtered + (state >> 16) - 32768) // 4
        tones = 0
        for frequency in (233, 997, 3109):
            phase = (n * frequency) % RATE
            sign = 1 if phase < half else -1
            x = phase if sign == 1 else phase - half
            product = x * (half - x)
            # Bhaskara-I sine approximation on [0, pi], entirely integer.
            tones += sign * (32767 * 16 * product // (5 * half * half - 4 * product))
        burst = n < 3 * RATE
        if burst:
            local, length = n % RATE, RATE // 2
        else:
            local, length = n - 9 * RATE // 2, 5 * RATE // 2
        envelope = 0
        if 0 <= local < length:
            envelope = min(32768, local * 32768 // fade_in, (length - 1 - local) * 32768 // fade_out)
        sample = ((tones // 16 + (filtered // 16 if burst else 0)) * envelope) // 32768
        if not -32768 <= sample <= 32767:
            raise RuntimeError('Reference clipped; do not silently normalize it')
        pcm.extend(struct.pack('<h', sample))
    return bytes(pcm)


def reference_wave(pcm):
    return (b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt '
            + struct.pack('<IHHIIHH', 16, 1, 1, RATE, RATE * 2, 2, 16)
            + b'data' + struct.pack('<I', len(pcm)) + pcm)


def main():
    project = Path(__file__).resolve().parents[3]
    saved = (project / 'Saved').resolve()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-directory', type=Path, default=saved / 'AcousticV2/AuditionInput')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    output = args.output_directory.resolve()
    try:
        output.relative_to(saved)
    except ValueError:
        parser.error('Output must stay within this managed project copy Saved directory')
    pcm = reference_pcm()
    wav = reference_wave(pcm)
    target = output / NAME
    if target.exists():
        if target.read_bytes() != wav:
            raise RuntimeError(f'Existing reference differs; refusing overwrite: {target}')
    elif args.verify_only:
        raise FileNotFoundError(target)
    else:
        output.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(wav)
    manifest = dict(contract='IM_V2Reference_v1', rate_hz=RATE, channels=1, bits=16,
                    frames=RATE * SECONDS, duration_s=SECONDS,
                    pcm_sha1=hashlib.sha1(pcm).hexdigest().upper(),
                    wav_sha256=hashlib.sha256(wav).hexdigest(),
                    generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    import_asset='/IceMoonAcousticField/Tests/IM_V2AuditionReference',
                    preparation_test='IceMoon.AcousticField.W3.PrepareAudition')
    if not args.verify_only:
        manifest_path = output / 'IM_V2Reference.json'
        if not manifest_path.exists():
            with manifest_path.open('x', encoding='utf-8', newline='\n') as stream:
                stream.write(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(path=str(target), verified=True, **manifest), indent=2))


if __name__ == '__main__':
    main()
