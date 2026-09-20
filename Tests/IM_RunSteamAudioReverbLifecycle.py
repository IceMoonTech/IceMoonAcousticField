"""Native production SDK lifecycle counterexamples; writes only a new evidence directory."""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess


def capture_w2(bake_directory, output, frames):
    """Freeze actual W2 cold payloads, retaining metadata order and SDK meters."""
    names = ('scene.bin', 'probes.bin', 'bake-metadata.json')
    payloads = {name: (bake_directory / name).read_bytes() for name in names}
    if any((bake_directory / name).read_bytes() != data for name, data in payloads.items()):
        raise RuntimeError('W2 input changed during capture')
    meta = json.loads(payloads['bake-metadata.json'].decode('utf-8'))
    origin, extent, probes = meta['bounds_origin_cm'], meta['bounds_extent_cm'], meta['probes_m']
    if (len(origin) != 3 or len(extent) != 3 or not all(math.isfinite(v) for v in [*origin, *extent])
            or not all(v > 0 for v in extent) or meta['sdk_version'] != 0x040801
            or len(probes) != meta['probe_count'] or not probes):
        raise ValueError('Invalid W2 metadata identity/bounds/probe count')
    for probe in probes:
        if len(probe) != 4 or not all(math.isfinite(v) for v in probe) or probe[3] <= 0:
            raise ValueError('Invalid W2 coverage sphere')
    snapshot = output / 'w2-input'
    snapshot.mkdir()
    for name, data in payloads.items():
        (snapshot / name).write_bytes(data)
    points = []
    for x in (550, 700, 900, 1000, 1100, 1250):
        ue = [x, 300, 150]
        if any(abs(ue[i] - origin[i]) > extent[i] for i in range(3)):
            raise ValueError(f'UE point {ue} outside actual bake bounds')
        sdk = [(ue[1] - origin[1]) * .01, (ue[2] - origin[2]) * .01, -(ue[0] - origin[0]) * .01]
        points.append(dict(name=f'ue-x{x}', ue_cm=ue, sdk_m=sdk))
    # This is the six coordinates explicitly listed by the user, without inventing a seventh.
    lines = [f'{len(probes)} {len(points)} {meta["reflection"]["saved_duration_s"]} {meta["reflection"]["order"]}',
             ' '.join(str(v) for v in origin)]
    lines.extend(' '.join(str(v) for v in probe) for probe in probes)
    lines.extend(' '.join(str(v) for v in [point['name'], *point['ue_cm'], *point['sdk_m']]) for point in points)
    (snapshot / 'coverage-points.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    manifest = dict(source_directory=str(bake_directory), bounds_origin_cm=origin, bounds_extent_cm=extent,
                    sample_rate_hz=48000, block_frames=frames,
                    probe_count=len(probes), triangles=meta['triangles'], recipe_version=meta['recipe_version'],
                    transform='(UE Y-origin Y, UE Z-origin Z, -(UE X-origin X)) * 0.01', points=points,
                    input_sha256={name: hashlib.sha256(data).hexdigest() for name, data in payloads.items()},
                    renderer='new/reset Renderer, 0.5 impulse then silence, full saved IR plus 0.5 seconds',
                    slot='one sequential reused slot; each point also has a fresh slot; Applied set after every successful Render',
                    zero_energy='COUNTEREXAMPLE; never relabeled PASS because reused and fresh are both zero')
    (output / 'w2-input-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return snapshot, len(points)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bake-directory', type=Path,
                        help='Run only actual-W2 cold-data points; skip the four synthetic-room cases.')
    parser.add_argument('--frames', type=int, choices=(512, 1024), default=512,
                        help='Native block frames; 1024 is allowed only with --bake-directory. Default: 512.')
    args = parser.parse_args()
    if args.frames != 512 and not args.bake_directory:
        parser.error('--frames 1024 requires --bake-directory; the original fixture remains 512 frames')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    snapshot, expected_cases = capture_w2(args.bake_directory.resolve(), output, args.frames) if args.bake_directory else (None, 4)
    plugin = Path(__file__).resolve().parents[1]
    source = plugin / 'Source/IceMoonAcousticField/Private'
    sdk = plugin / 'Source/ThirdParty/SteamAudio'
    inputs = [Path(__file__).with_name('IM_SteamAudioReverbLifecycle.cpp'),
              *[source / name for name in ('IMAcousticSimulation.cpp', 'IMAcousticReverbRenderer.cpp',
                                          'IMAcousticAudioRenderer.cpp', 'IMAcousticSDKContext.cpp')]]
    tracked = [*inputs, Path(__file__).resolve(), *source.glob('IMAcoustic*.h'),
               sdk / 'include/phonon.h', sdk / 'lib/windows-x64/phonon.dll',
               sdk / 'lib/windows-x64/phonon.lib', sdk / 'IM_SteamAudio481Provenance.json']
    hashes = {str(p.relative_to(plugin)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    (output / 'source-hashes.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                                      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        raise RuntimeError('MSVC vcvars64.bat unavailable')
    for path in [output, vcvars, *inputs, sdk, source]:
        if any(c in str(path) for c in '\r\n"%&|<>^'):
            raise ValueError('Path contains unsupported shell/response characters')
    exe = output / 'IM_SteamAudioReverbLifecycle.exe'
    options = ['/nologo', '/std:c++20', '/EHsc', '/Od', '/Zi', '/MD',
               f'/DIM_REVERB_TEST_BLOCK_FRAMES={args.frames}', f'/I"{sdk / "include"}"',
               f'/I"{source}"', *[f'"{p}"' for p in inputs], f'/Fe:"{exe}"', '/link', '/DEBUG',
               'Dbghelp.lib', f'"{sdk / "lib/windows-x64/phonon.lib"}"']
    response = output / 'compile.rsp'
    response.write_text('\n'.join(options), encoding='utf-8')
    build = output / 'build.cmd'
    build.write_text(f'@echo off\ncall "{vcvars}" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
                     f'cl @"{response}"\nexit /b %errorlevel%\n', encoding='utf-8', newline='\r\n')
    with (output / 'compile.log').open('w', encoding='utf-8') as log:
        compiled = subprocess.run(['cmd.exe', '/d', '/c', str(build)], cwd=output, stdout=log, stderr=subprocess.STDOUT)
    if compiled.returncode:
        print('Compile failed:', output / 'compile.log', flush=True)
        return compiled.returncode
    changed = [str(p.relative_to(plugin)) for p in tracked
               if hashlib.sha256(p.read_bytes()).hexdigest() != hashes[str(p.relative_to(plugin))]]
    if changed:
        (output / 'build-input-drift.json').write_text(json.dumps(changed, indent=2), encoding='utf-8')
        raise RuntimeError('Concurrent production input change during build; no valid native receipt')
    shutil.copy2(sdk / 'lib/windows-x64/phonon.dll', output / 'phonon.dll')
    with (output / 'run.log').open('w', encoding='utf-8') as log:
        command = [str(exe), str(output)]
        if snapshot:
            command.append(str(snapshot))
        ran = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT)
    cases_file, metrics_file = output / 'case-results.tsv', output / 'metrics.csv'
    cases = list(csv.DictReader(cases_file.open(encoding='utf-8'), delimiter='\t')) if cases_file.exists() else []
    metrics = list(csv.DictReader(metrics_file.open(encoding='utf-8'))) if metrics_file.exists() else []
    for case in cases:
        case['metrics'] = {m['metric']: float(m['value']) for m in metrics if m['case'] == case['case']}
    complete = len(cases) == expected_cases and all(c['status'] in ('PASS', 'COUNTEREXAMPLE') for c in cases)
    receipt = dict(exit_code=ran.returncode, complete=complete,
                   native_pass=complete and ran.returncode == 0 and all(c['status'] == 'PASS' for c in cases),
                   cases=cases, sample_rate_hz=48000, block_frames=args.frames,
                   scope='actual W2 cold-data sequential/fresh slot impulses' if snapshot else 'native production SDK lifecycle and output-only route masks',
                   synthetic_cases='NOT_RUN' if snapshot else 'RUN',
                   slot_ack='caller sets Applied only after successful production ReverbRenderer.Render',
                   source_hashes='source-hashes.json', ue_ubt='NOT_RUN', product_writes='NONE',
                   input_drift_after_run=[str(p.relative_to(plugin)) for p in tracked
                       if hashlib.sha256(p.read_bytes()).hexdigest() != hashes[str(p.relative_to(plugin))]])
    if snapshot:
        receipt['zero_ir_positions'] = {route: [c['case'] for c in cases if c['metrics'].get(route + '_zero_ir') == 1]
                                        for route in ('reused', 'fresh')}
        receipt['query_rejected_positions'] = {route: [c['case'] for c in cases if c['metrics'].get(route + '_query_accepted') == 0]
                                               for route in ('reused', 'fresh')}
        receipt['cold_input_manifest'] = 'w2-input-manifest.json'
    (output / 'terminal-receipt.json').write_text(json.dumps(receipt, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print('Native fixture exit:', ran.returncode, 'complete:', complete, 'evidence:', output, flush=True)
    return ran.returncode if ran.returncode or receipt['native_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
