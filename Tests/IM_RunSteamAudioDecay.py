"""Build/run native SDK decay-counterexample fixture. All output goes to task evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    plugin = Path(__file__).resolve().parents[1]
    sdk = plugin / 'Source/ThirdParty/SteamAudio'
    source = plugin / 'Source/IceMoonAcousticField/Private'
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        raise RuntimeError('MSVC vcvars64.bat not found')
    # Production sources reused with the same native-cl method as IM_RunSteamAudioSmoke.py.
    # IM_AcousticReverbData is header-only, so it needs no cpp entry here.
    inputs = [Path(__file__).parent / 'IM_SteamAudioDecay.cpp', source / 'IMAcousticReverbRenderer.cpp', source / 'IMAcousticSimulation.cpp', source / 'IMAcousticSDKContext.cpp']
    for path in [output, vcvars, *inputs, sdk, source]:
        if any(c in str(path) for c in '\r\n"%&|<>^'):
            raise ValueError('Path contains unsupported shell/response characters')
    exe = output / 'IM_SteamAudioDecay.exe'
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
    tracked = [*inputs, *source.glob('IMAcoustic*.h'), sdk / 'IM_SteamAudio481Provenance.json']
    manifest = {str(p.relative_to(plugin)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    (output / 'source-hashes.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    with (output / 'run.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([str(exe), str(output)], cwd=output, stdout=log, stderr=subprocess.STDOUT)
    print('Native fixture exit:', result.returncode, 'evidence:', output, flush=True)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
