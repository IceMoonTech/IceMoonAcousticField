"""Build/run native SDK path-oracle counterexample fixture. All output goes to task evidence."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess


def _w2_cases(bake_dir: Path, output: Path):
    """Freeze the real bake and transform UE world cm using its own origin.

    Probe positions are coverage evidence only. They never select test endpoints.
    A second read detects an overlapping bake write; failure cannot become a
    door-only success when --bake-directory was requested.
    """
    names = ('bake-metadata.json', 'scene.bin', 'probes.bin')
    payload = {name: (bake_dir / name).read_bytes() for name in names}
    if any((bake_dir / name).read_bytes() != data for name, data in payload.items()):
        raise RuntimeError('W2 input changed while being captured')
    meta = json.loads(payload['bake-metadata.json'].decode('utf-8'))
    origin, extent = meta['bounds_origin_cm'], meta['bounds_extent_cm']
    if (len(origin) != 3 or len(extent) != 3
            or not all(math.isfinite(x) for x in [*origin, *extent])
            or not all(x > 0 for x in extent)
            or meta['sdk_version'] != 0x040801 or meta['triangles'] <= 0
            or meta['probe_count'] != len(meta['probes_m']) or not meta['probes_m']):
        raise ValueError('Invalid W2 metadata identity/bounds/probe count')
    snapshot = output / 'w2-input'
    snapshot.mkdir()
    for name, data in payload.items():
        (snapshot / name).write_bytes(data)
    cases = []

    def add(name, group, src, lst, direct, path, step=-1, t=-1):
        def sdk(ue):
            if any(abs(ue[i] - origin[i]) > extent[i] for i in range(3)):
                raise ValueError(f'{name}: UE endpoint outside actual bake bounds')
            x, y, z = [ue[i] - origin[i] for i in range(3)]
            return [y * .01, z * .01, -x * .01]
        cases.append(dict(name=name, group=group, step=step, t=t,
                          source_ue_cm=list(src), listener_ue_cm=list(lst),
                          source_sdk_m=sdk(src), listener_sdk_m=sdk(lst),
                          expected_direct=direct, expected_path=path))

    # Frozen user-specified points: geometry expectations are gates, not hints.
    add('room-a', 'fixed', (300, 300, 150), (700, 300, 150), 'clear', 'absent')
    add('door-a', 'fixed', (800, 300, 150), (1200, 300, 150), 'clear', 'absent')
    add('outside-door-adjacent', 'fixed', (970, 225, 150), (1030, 225, 150), 'blocked', 'optional')
    add('room-b-lower', 'fixed', (1800, 300, 150), (2200, 300, 150), 'clear', 'absent')
    add('room-b-upper', 'fixed', (1800, 300, 490), (2200, 300, 490), 'clear', 'absent')
    add('cross-floor', 'floor', (1800, 300, 150), (1800, 300, 490), 'blocked', 'absent')
    add('room-b-lower-reverse', 'fixed', (2200, 300, 150), (1800, 300, 150), 'clear', 'absent')
    add('room-b-upper-reverse', 'fixed', (2200, 300, 490), (1800, 300, 490), 'clear', 'absent')
    add('cross-floor-reverse', 'floor', (1800, 300, 490), (1800, 300, 150), 'blocked', 'absent')
    add('cross-floor-x2200', 'floor', (2200, 300, 150), (2200, 300, 490), 'blocked', 'absent')
    add('cross-floor-x2000', 'floor', (2000, 350, 150), (2000, 350, 490), 'blocked', 'absent')
    for step in range(64):
        t = step / 63
        add(f'trajectory-{step:02d}', 'trajectory',
            (500 + 1500 * t, 300 + 20 * math.sin(2 * math.pi * t), 150),
            (600 + 1500 * t, 300, 150), 'oracle', 'optional', step, t)
    manifest = dict(
        source_directory=str(bake_dir), bounds_origin_cm=origin, bounds_extent_cm=extent,
        sdk_version=meta['sdk_version'], recipe_version=meta['recipe_version'],
        triangles=meta['triangles'], probe_count=meta['probe_count'],
        input_sha256={name: hashlib.sha256(data).hexdigest() for name, data in payload.items()},
        transform='(UE Y-origin Y, UE Z-origin Z, -(UE X-origin X)) * 0.01',
        cases=cases,
        coverage='Fixed pairs and 64 sequential samples only, not exhaustive full-map verification.',
        floor_contract='The actual full lower B ceiling blocks these cross-floor pairs. '
                       'Upper slab stair-hole geometry does not remove it. No connected stair/walking claim.',
        callback_coverage='Probe-interior only; source/probe and probe/listener endpoint legs UNCOVERED.',
        continuity='SH/EQ deltas are descriptive native samples; no audible continuity acceptance.')
    (output / 'w2-cases.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    # Whitespace table is consumed by the native fixture without adding a JSON dependency.
    lines = [f'{len(cases)} {meta["triangles"]}']
    for c in cases:
        fields = [c['name'], c['group'], c['step'], c['t'], *c['source_ue_cm'],
                  *c['listener_ue_cm'], *c['source_sdk_m'], *c['listener_sdk_m'],
                  c['expected_direct'], c['expected_path']]
        lines.append(' '.join(str(x) for x in fields))
    (output / 'w2-cases.txt').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return snapshot


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bake-directory', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    # Never reuse a previous run: failed gates and their raw evidence are immutable.
    output.mkdir(parents=True, exist_ok=False)
    bake = _w2_cases(args.bake_directory.resolve(), output) if args.bake_directory else None
    plugin = Path(__file__).resolve().parents[1]
    sdk = plugin / 'Source/ThirdParty/SteamAudio'
    source = plugin / 'Source/IceMoonAcousticField/Private'
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        raise RuntimeError('MSVC vcvars64.bat not found')
    # Production sources reused with the same native-cl method as IM_RunSteamAudioSmoke.py.
    # No audio/HRTF renderer: this fixture records Evaluate frames + the real
    # pathing callback and checks them with an independent intersection oracle.
    inputs = [Path(__file__).parent / 'IM_SteamAudioPaths.cpp', source / 'IMAcousticSimulation.cpp', source / 'IMAcousticSDKContext.cpp']
    tracked = [*inputs, Path(__file__).resolve(), *source.glob('IMAcoustic*.h'),
               sdk / 'IM_SteamAudio481Provenance.json', sdk / 'include/phonon.h',
               sdk / 'lib/windows-x64/phonon.dll', sdk / 'lib/windows-x64/phonon.lib']
    manifest = {str(p.relative_to(plugin)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    (output / 'source-hashes.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    for path in [output, vcvars, *inputs, sdk, source]:
        if any(c in str(path) for c in '\r\n"%&|<>^'):
            raise ValueError('Path contains unsupported shell/response characters')
    exe = output / 'IM_SteamAudioPaths.exe'
    options = ['/nologo', '/std:c++20', '/EHsc', '/Od', '/Zi', '/MD', f'/I"{sdk / "include"}"', f'/I"{source}"', *[f'"{p}"' for p in inputs], f'/Fe:"{exe}"', '/link', '/DEBUG', 'Dbghelp.lib', f'"{sdk / "lib/windows-x64/phonon.lib"}"']
    response = output / 'compile.rsp'
    response.write_text('\n'.join(options), encoding='utf-8')
    build = output / 'build.cmd'
    build.write_text(f'@echo off\ncall "{vcvars}" >nul\nif errorlevel 1 exit /b %errorlevel%\ncl @"{response}"\nexit /b %errorlevel%\n', encoding='utf-8', newline='\r\n')
    with (output / 'compile.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(['cmd.exe', '/d', '/c', str(build)], cwd=output, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print('Compile failed:', output / 'compile.log', flush=True)
        return result.returncode
    shutil.copy2(sdk / 'lib/windows-x64/phonon.dll', output / 'phonon.dll')
    if any(hashlib.sha256(p.read_bytes()).hexdigest() != manifest[str(p.relative_to(plugin))] for p in tracked):
        raise RuntimeError('Native build inputs changed during compilation; receipt is invalid')
    command = [str(exe), str(output)]
    if bake:
        command.extend([str(bake), str(output / 'w2-cases.txt')])
    with (output / 'run.log').open('w', encoding='utf-8') as log:
        result = subprocess.run(command, cwd=output, stdout=log, stderr=subprocess.STDOUT)
    (output / 'native-receipt.json').write_text(json.dumps(dict(
        exit_code=result.returncode, command=command,
        meaning={0: 'complete_no_counterexample', 2: 'complete_counterexample'}.get(result.returncode, 'harness_error'),
        ue_ubt='NOT_RUN', full_map='NOT_VERIFIED'), indent=2), encoding='utf-8')
    print('Native fixture exit:', result.returncode, 'evidence:', output, flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
