"""Prepare the Kenney "Impact Sounds" (CC0) footsteps for the acoustic footstep audition and write their license manifest.

usage: python IMPrepareKenneyFootsteps.py <extracted_pack_dir> <zip> <ffmpeg> <reference_wav>...

<extracted_pack_dir> holds Audio/*.ogg and License.txt (the unpacked kenney_impact-sounds.zip), <zip> is the downloaded archive (hashed
into the manifest), <reference_wav>... are the original footstep recordings whose mean level the new samples are trimmed to.

Processing is only decode + one shared gain: no EQ, no layering, no trimming of the files. The decode is ffmpeg ogg -> mono 48 kHz PCM16.
The shared gain makes the mean RMS of the ten new samples equal to the mean RMS of the reference recordings (power mean), so the loudness
context every downstream gain was tuned against (distance gain, equal-loudness compensation) is unchanged and raw level does not
decide the timbre comparison.
"""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np

PACK_URL = 'https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip'
ASSET_PAGE = 'https://kenney.nl/assets/impact-sounds'
DOWNLOAD_DATE = '2026-10-21'
LICENSE_URL = 'http://creativecommons.org/publicdomain/zero/1.0/'
SAMPLE_RATE = 48000
# (UE asset suffix, material, Kenney file stem) in the order the character cycles through them.
SOURCES = [('Wood%02d' % i, 'wood', 'footstep_wood_%03d' % i) for i in range(5)] + [('Stone%02d' % i, 'stone', 'footstep_concrete_%03d' % i) for i in range(5)]
BANDS = [(20, 60), (60, 150), (150, 400), (400, 1000), (1000, 3000), (3000, 8000), (8000, 16000)]

DIRECTORY = Path(__file__).resolve().parent
OUT = DIRECTORY / 'Kenney'
MANIFEST = DIRECTORY.parent / 'IMKenneyFootstepSources.json'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def read_pcm16(path):
    with wave.open(str(path), 'rb') as reader:
        assert reader.getnchannels() == 1 and reader.getframerate() == SAMPLE_RATE and reader.getsampwidth() == 2, path
        return np.frombuffer(reader.readframes(reader.getnframes()), dtype='<i2').astype(np.float64) / 32768.


def rms_db(x):
    return float(10. * np.log10(np.mean(x ** 2) + 1e-20))


def analysis(x):
    size = max(1 << 16, len(x))
    # Unwindowed power spectrum; by Parseval the one-sided band sums below are mean-square contributions of the band to the signal,
    # so band_dbfs is directly comparable with rms_dbfs (both are 10*log10 of a mean square).
    spectrum = np.abs(np.fft.rfft(x, n=size)) ** 2
    freqs = np.fft.rfftfreq(size, 1. / SAMPLE_RATE)
    total = spectrum[(freqs >= 20) & (freqs <= 16000)].sum()
    band_db = {}
    share = {}
    for low, high in BANDS:
        part = spectrum[(freqs >= low) & (freqs < high)].sum()
        share['%d-%d' % (low, high)] = float(part / total)
        band_db['%d-%d' % (low, high)] = float(10. * np.log10(2. * part / (size * len(x)) + 1e-20))
    centroid = float((freqs * spectrum)[(freqs >= 20) & (freqs <= 16000)].sum() / total)
    return {'rms_dbfs': rms_db(x), 'peak_dbfs': float(20. * np.log10(np.abs(x).max() + 1e-12)), 'centroid_hz': centroid,
            'energy_share_20_16000': share, 'band_dbfs': band_db}


def decode(ffmpeg, ogg, target):
    subprocess.run([ffmpeg, '-v', 'error', '-y', '-i', str(ogg), '-ac', '1', '-ar', str(SAMPLE_RATE), '-c:a', 'pcm_s16le', str(target)], check=True)


def main():
    pack, archive, ffmpeg = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    references = [Path(p) for p in sys.argv[4:]]
    original_dir = OUT / 'Original'
    prepared_dir = OUT / 'Prepared'
    for directory in (original_dir, prepared_dir):
        directory.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(pack / 'License.txt', OUT / 'License.txt')

    decoded = {}
    with tempfile.TemporaryDirectory() as temp:
        for suffix, material, stem in SOURCES:
            ogg = pack / 'Audio' / (stem + '.ogg')
            shutil.copyfile(ogg, original_dir / ogg.name)
            target = Path(temp) / (stem + '.wav')
            decode(ffmpeg, ogg, target)
            decoded[stem] = read_pcm16(target)

    reference_power = np.mean([np.mean(read_pcm16(p) ** 2) for p in references])
    new_power = np.mean([np.mean(x ** 2) for x in decoded.values()])
    trim_db = round(float(10. * np.log10(reference_power / new_power)), 2)
    gain = 10. ** (trim_db / 20.)

    rows = []
    for suffix, material, stem in SOURCES:
        x = decoded[stem]
        samples = np.round(x * gain * 32768.).astype('<i2')
        name = 'SW_IMFootstepKenney%s' % suffix
        target = prepared_dir / (name + '.wav')
        with wave.open(str(target), 'wb') as writer:
            writer.setparams((1, 2, SAMPLE_RATE, len(samples), 'NONE', 'not compressed'))
            writer.writeframes(samples.tobytes())
        ogg = original_dir / (stem + '.ogg')
        rows.append({
            'file': str(target), 'ue_asset': '/IceMoonAcousticField/Tests/Audio/Footsteps/' + name, 'material': material,
            'source_url': PACK_URL, 'asset_page': ASSET_PAGE, 'author': 'Kenney (www.kenney.nl)', 'license': 'CC0-1.0',
            'license_url': LICENSE_URL, 'download_date': DOWNLOAD_DATE, 'sha256': sha256(target),
            'original_member': 'Audio/%s.ogg' % stem, 'original_file': str(ogg), 'original_sha256': sha256(ogg),
            'sample_rate': SAMPLE_RATE, 'channels': 1, 'bits_per_sample': 16, 'shared_trim_db': trim_db,
            'analysis': analysis(np.frombuffer(target.read_bytes()[44:], dtype='<i2').astype(np.float64) / 32768.),
        })
    manifest = {
        'license_dedication': 'Kenney "Impact Sounds" 1.0 (created 2019-12-19) is released by Kenney under CC0 1.0 Universal; the pack License.txt is kept beside the files.',
        'license_url': LICENSE_URL,
        'pack': {'name': 'Impact Sounds 1.0', 'author': 'Kenney (www.kenney.nl)', 'asset_page': ASSET_PAGE, 'archive_url': PACK_URL,
                 'archive_sha256': sha256(archive), 'archive_bytes': archive.stat().st_size, 'download_date': DOWNLOAD_DATE,
                 'license_file': str(OUT / 'License.txt'), 'license_file_sha256': sha256(OUT / 'License.txt')},
        'preparation': {'script': str(Path(__file__).resolve()), 'script_sha256': sha256(__file__), 'decoder': 'ffmpeg ogg -> mono 48 kHz PCM16',
                        'processing': 'decode plus one shared gain only; no EQ, layering or trimming',
                        'shared_trim_db': trim_db,
                        'trim_rationale': 'Mean RMS (power mean) of the ten new samples is set equal to the power-mean RMS of the reference recordings listed below, '
                                          'so the loudness context of the existing distance gain and equal-loudness compensation is unchanged. '
                                          'Not a final loudness calibration.',
                        'reference_recordings': [{'file': str(p), 'sha256': sha256(p), 'rms_dbfs': rms_db(read_pcm16(p))} for p in references]},
        'format': 'Mono PCM16 48000Hz; the original Kenney .ogg files are retained under Source/Kenney/Original.',
        'files': rows,
    }
    MANIFEST.write_bytes((json.dumps(manifest, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
    print(json.dumps({'files': len(rows), 'shared_trim_db': trim_db, 'mean_rms_dbfs_new': rms_db(np.concatenate([x * gain for x in decoded.values()]))}))


if __name__ == '__main__':
    main()
