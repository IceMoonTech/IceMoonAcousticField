from pathlib import Path
import hashlib
import json
import wave
import numpy as np


DIRECTORY = Path(__file__).parent
MANIFEST = DIRECTORY.parent / 'IMPresenceFootstepSources.json'
INPUT_GAIN = .125


if __name__ == '__main__':
    manifest = json.loads(MANIFEST.read_text(encoding='utf-8'))
    assert 'prepared_files' not in manifest
    prepared = DIRECTORY / 'Prepared'
    prepared.mkdir(exist_ok=True)
    rows = []
    for original in manifest['files']:
        source = Path(original['file'])
        assert hashlib.sha256(source.read_bytes()).hexdigest() == original['sha256']
        with wave.open(str(source), 'rb') as reader:
            assert reader.getnchannels() == 1 and reader.getframerate() == 48000 and reader.getsampwidth() == 2
            values = np.frombuffer(reader.readframes(reader.getnframes()), dtype='<i2')
        samples = np.round(values.astype(np.float64) * INPUT_GAIN).astype('<i2')
        target = prepared / source.name
        assert not target.exists()
        with wave.open(str(target), 'wb') as writer:
            writer.setparams((1, 2, 48000, len(samples), 'NONE', 'not compressed'))
            writer.writeframes(samples.tobytes())
        rows.append({**{key: original[key] for key in ('ue_asset', 'source_url', 'author', 'license', 'license_url', 'download_date', 'generated_date')},
            'file': str(target), 'sha256': hashlib.sha256(target.read_bytes()).hexdigest(), 'original_file': str(source), 'original_sha256': original['sha256'],
            'channels': 1, 'sample_rate': 48000, 'bits_per_sample': 16, 'common_input_gain': INPUT_GAIN,
            'common_input_gain_db': float(20. * np.log10(INPUT_GAIN)), 'preparation_source_url': Path(__file__).resolve().as_uri(),
            'preparation_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()})
    manifest['prepared_files'] = rows
    manifest['input_level_rationale'] = 'One fixed -18.0618dB content trim for all four new sources. Their original RMS is about -25 to -26dBFS versus the original Stone about -36dBFS, and the previous 3m distance gain was 0.26 to 0.33. Offset the measured source/nearfield level uplift so raw loudness does not hide timbre; this is not the final measured equal-loudness compensation. Original generated WAVs are preserved.'
    MANIFEST.write_bytes((json.dumps(manifest, ensure_ascii=False, indent=2) + chr(10)).encode('utf-8'))
    print(json.dumps({'prepared_files': len(rows), 'one_shared_input_gain_db': rows[0]['common_input_gain_db'], 'originals_preserved': True}))
