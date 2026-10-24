from pathlib import Path
import hashlib
import json
import sys
import wave
import numpy as np
from scipy.signal import butter, sosfilt


RATE = 48000
DIRECTORY = Path(__file__).parent
LICENSE_URL = 'https://creativecommons.org/publicdomain/zero/1.0/'


def filtered_noise(random, frames, low, high):
    return sosfilt(butter(2, [low, high], btype='bandpass', fs=RATE, output='sos'), random.normal(0., 1., frames))


def synthesize(material, variant):
    seed = (1729 if material == 'Wood' else 3407) + variant * 97
    random = np.random.default_rng(seed)
    seconds = .32 if material == 'Wood' else .28
    time = np.arange(round(seconds * RATE), dtype=np.float64) / RATE
    attack = 1. - np.exp(-time / .00015)
    contact = filtered_noise(random, len(time), 1800., 7800.) * attack * np.exp(-time / (.012 if material == 'Wood' else .009))
    scuff = filtered_noise(random, len(time), 450., 4800.) * attack * np.exp(-time / .035)
    modes = ((210., .18, .07), (470., .12, .045), (1030., .12, .025), (2650., .15, .016)) if material == 'Wood' else ((165., .08, .065), (1650., .12, .035), (3400., .16, .024), (5600., .13, .014))
    body = np.zeros_like(time)
    tuning = 1. + .045 * variant
    for frequency, amplitude, decay in modes:
        body += amplitude * np.sin(2. * np.pi * frequency * tuning * time) * attack * np.exp(-time / decay)
    if material == 'Wood':
        second = np.maximum(time - .008, 0.)
        knock = (time >= .008) * np.sin(2. * np.pi * 780. * second) * np.exp(-second / .019)
        values = .62 * contact + .11 * scuff + body + .09 * knock
    else:
        grains = np.zeros_like(time)
        for delay in (.006, .014, .023):
            phase = np.maximum(time - delay, 0.)
            grains += (time >= delay) * np.exp(-phase / .003)
        grit = filtered_noise(random, len(time), 2200., 7200.) * grains
        values = .82 * contact + .09 * scuff + body + .12 * grit
    values *= np.minimum(1., np.maximum(0., (seconds - time) / .02))
    return seed, values


def spectrum(values):
    powers = np.abs(np.fft.rfft(values)) ** 2 / len(values) ** 2
    powers[1:-1] *= 2.
    frequencies = np.fft.rfftfreq(len(values), 1. / RATE)
    bands = [(20, 80), (80, 200), (200, 1000), (1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)]
    energy = {str(low) + '-' + str(high): float(powers[(frequencies >= low) & (frequencies < high)].sum()) for low, high in bands}
    return {'rms_dbfs': 10. * np.log10(np.mean(values ** 2)), 'peak_dbfs': 20. * np.log10(np.abs(values).max()),
        'band_dbfs': {key: 10. * np.log10(max(value, 1.e-30)) for key, value in energy.items()},
        'fraction_2000_8000_of_20_16000': (energy['2000-4000'] + energy['4000-8000']) / sum(energy.values())}


def main(verify_only):
    generated = {(material, variant): synthesize(material, variant) for material in ('Wood', 'Stone') for variant in range(2)}
    shared_gain = min(1., .8 / max(float(np.abs(values).max()) for seed, values in generated.values()))
    rows = []
    for (material, variant), (seed, values) in generated.items():
        name = 'SW_IMFootstepPresence' + material + ('A' if variant == 0 else 'B')
        path = DIRECTORY / (name + '.wav')
        samples = np.round(values * shared_gain * 32767.).astype('<i2')
        assert np.isfinite(values).all() and np.abs(values * shared_gain).max() < 1.
        if verify_only:
            with wave.open(str(path), 'rb') as reader:
                assert reader.getnchannels() == 1 and reader.getframerate() == RATE and reader.getsampwidth() == 2
                assert reader.readframes(reader.getnframes()) == samples.tobytes()
        else:
            assert not path.exists()
            with wave.open(str(path), 'wb') as writer:
                writer.setparams((1, 2, RATE, len(samples), 'NONE', 'not compressed'))
                writer.writeframes(samples.tobytes())
        rows.append({'file': str(path), 'ue_asset': '/IceMoonAcousticField/Tests/Audio/Footsteps/' + name,
            'source_url': Path(__file__).resolve().as_uri(), 'author': 'Test550 task procedural synthesis by OpenAI Codex; no third-party recordings',
            'license': 'CC0-1.0', 'license_url': LICENSE_URL, 'download_date': None, 'download_not_applicable': 'Generated locally; the original generated WAV is retained here.',
            'generated_date': '2026-10-18', 'seed': seed, 'generator_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'sample_rate': RATE, 'channels': 1, 'bits_per_sample': 16,
            'shared_source_gain': shared_gain, 'analysis': spectrum(samples.astype(np.float64) / 32768.)})
    manifest = DIRECTORY.parent / 'IMPresenceFootstepSources.json'
    if verify_only:
        existing = json.loads(manifest.read_text(encoding='utf-8'))
        assert existing['files'] == rows
    else:
        assert not manifest.exists()
        manifest.write_bytes((json.dumps({'license_dedication': 'The original procedural audio files listed here are dedicated under CC0 1.0 Universal to the extent permissible. No external audio was downloaded or sampled.',
            'license_url': LICENSE_URL, 'generator': str(Path(__file__).resolve()), 'generator_requires': ['Python 3', 'numpy', 'scipy'],
            'format': 'Mono PCM16 48000Hz original generated WAV; UE imports do not replace these originals.', 'files': rows}, ensure_ascii=False, indent=2) + chr(10)).encode('utf-8'))
    print(json.dumps({'verified_reproducible': verify_only, 'shared_source_gain': shared_gain, 'materials': [{'asset': row['ue_asset'], 'rms_dbfs': row['analysis']['rms_dbfs'], '2_to_8_kHz_fraction': row['analysis']['fraction_2000_8000_of_20_16000']} for row in rows]}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main('--verify' in sys.argv)
