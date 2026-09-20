"""D1-unit runner: renderer/history fixed A/B. Selects inputs, freezes hashes, runs once, verifies windows."""
import argparse
import hashlib
import json
import math
import os
import struct
from pathlib import Path
import shutil
import subprocess

D1_RATE = 48000
D1_BLOCK = 1024
D1_HIST = 10
D1_POST = 94
D1_TOTAL = D1_HIST + D1_POST
D1_W03 = 14400
D1_GEN = 7
D1_FLAGS = 8
D1_TONE_AMP = 0.079345703125
D1_FREQS = [233, 997, 3109]
D1_HEADER = ['block', 'seq', 'occ', 'flags', 'routes', 'ok', 'fail', 'input_e', 'direct_e', 'path_e']


def read_blocks(path):
    lines = path.read_text(encoding='utf-8').splitlines()
    header = lines[0].split(',')
    rows = [line.split(',') for line in lines[1:] if line]
    return header, rows


def read_f32(path):
    raw = path.read_bytes()
    if not raw or len(raw) % 4 != 0:
        raise ValueError('truncated-or-empty-f32:' + str(path))
    return struct.unpack('<' + str(len(raw) // 4) + 'f', raw)


def lr_tails(vals):
    last_l = -1
    last_r = -1
    nz = 0
    for i, v in enumerate(vals):
        if not math.isfinite(v):
            raise ValueError('nonfinite-sample')
        if v != 0.0:
            nz += 1
            if i % 2 == 0:
                last_l = i // 2
            else:
                last_r = i // 2
    return last_l, last_r, nz


def pcm_expected(vals):
    q = [quantize_d1(v) for v in vals]
    packed = struct.pack('<' + str(len(q)) + 'h', *q)
    last_l = -1
    last_r = -1
    for i, s in enumerate(q):
        if s != 0:
            if i % 2 == 0:
                last_l = i // 2
            else:
                last_r = i // 2
    nz = sum(1 for s in q if s != 0)
    energy = math.fsum(float(s) * s for s in q)
    return packed, nz, last_l, last_r, energy


def verify_pcm(vals, packed_bytes, info):
    reasons = []
    if len(vals) % 2 != 0:
        return False, ['odd-float-count']
    packed, nz, last_l, last_r, energy = pcm_expected(vals)
    if len(packed_bytes) != len(packed):
        return False, ['pcm-length']
    if packed_bytes != packed:
        reasons.append('pcm-bytes')
    try:
        ok_meta = (info.get('samples') == len(vals) // 2
                   and info.get('nonzero_count') == nz
                   and info.get('last_nonzero_l') == last_l
                   and info.get('last_nonzero_r') == last_r
                   and float(info.get('pcm16_energy')) == energy)
    except Exception:
        return False, reasons + ['pcm-meta-unreadable']
    if not ok_meta:
        reasons.append('pcm-meta')
    return (not reasons), reasons


def verify_float_tail(vals, tail):
    try:
        last_l, last_r, _ = lr_tails(vals)
    except ValueError:
        return False, ['float-nonfinite']
    try:
        ok = (tail.get('post_samples') == len(vals) // 2
              and tail.get('last_nonzero_l') == last_l
              and tail.get('last_nonzero_r') == last_r)
    except Exception:
        return False, ['float-tail-unreadable']
    return (True, []) if ok else (False, ['float-tail'])


def verifier_selftest():
    cases = []
    def expect(name, cond):
        cases.append({'name': name, 'pass': bool(cond)})
    z = [0.0] * 8
    expect('all-zero-lr', lr_tails(z) == (-1, -1, 0))
    expect('all-zero-verify', verify_pcm(z, struct.pack('<8h', *([0] * 8)), {'samples': 4, 'nonzero_count': 0, 'last_nonzero_l': -1, 'last_nonzero_r': -1, 'pcm16_energy': 0.0}) == (True, []))
    l_late = [0.0] * 8
    l_late[6] = 0.5
    l_late[3] = 0.25
    expect('l-late-r-early', lr_tails(l_late)[:2] == (3, 1))
    r_late = [0.0] * 8
    r_late[7] = 0.5
    r_late[2] = 0.25
    expect('r-late-l-early', lr_tails(r_late)[:2] == (1, 3))
    mono_l = [0.0] * 8
    mono_l[4] = 1.0
    expect('mono-l-only', lr_tails(mono_l) == (2, -1, 1))
    mono_r = [0.0] * 8
    mono_r[5] = -1.0
    expect('mono-r-only', lr_tails(mono_r) == (-1, 2, 1))
    packed, nz, ll, rl, en = pcm_expected(l_late)
    good_info = {'samples': 4, 'nonzero_count': nz, 'last_nonzero_l': ll, 'last_nonzero_r': rl, 'pcm16_energy': en}
    expect('pcm-accept', verify_pcm(l_late, packed, good_info) == (True, []))
    bad_info = dict(good_info)
    bad_info['last_nonzero_l'] = rl
    bad_info['last_nonzero_r'] = ll
    expect('swapped-tails-rejected', verify_pcm(l_late, packed, bad_info) == (False, ['pcm-meta']))
    bad_info2 = dict(good_info)
    bad_info2['last_nonzero_l'] = ll + 100
    expect('wrong-tail-rejected', verify_pcm(l_late, packed, bad_info2) == (False, ['pcm-meta']))
    corrupt = bytearray(packed)
    corrupt[0] ^= 0xFF
    expect('pcm-bytes-rejected', verify_pcm(l_late, bytes(corrupt), good_info) == (False, ['pcm-bytes']))
    expect('truncated-rejected', verify_pcm(l_late, packed[:-2], good_info) == (False, ['pcm-length']))
    nan_vals = [0.0] * 8
    nan_vals[2] = float('nan')
    try:
        lr_tails(nan_vals)
        expect('nonfinite-rejected', False)
    except ValueError:
        expect('nonfinite-rejected', True)
    expect('float-tail-accept', verify_float_tail(r_late, {'post_samples': 4, 'last_nonzero_l': 1, 'last_nonzero_r': 3}) == (True, []))
    expect('float-tail-rejected', verify_float_tail(r_late, {'post_samples': 4, 'last_nonzero_l': 3, 'last_nonzero_r': 1}) == (False, ['float-tail']))
    try:
        read_f32(Path('__nonexistent_selftest__.f32'))
        expect('missing-rejected', False)
    except Exception:
        expect('missing-rejected', True)
    ok_all = all(c['pass'] for c in cases)
    return ok_all, cases


def quantize_d1(v):
    r = math.floor(v * 32767.0 + 0.5) if v >= 0 else math.ceil(v * 32767.0 - 0.5)
    if r > 32767:
        r = 32767
    if r < -32768:
        r = -32768
    return int(r)


def evaluate_control(evdir):
    reasons = []
    det = {}
    try:
        params = json.loads((evdir / 'params.json').read_text(encoding='utf-8'))
    except Exception:
        return False, ['params-unreadable'], det
    frozen = {'rate': 48000, 'block': 1024, 'hist_blocks': 10, 'post_blocks': 94,
              'generation': 7, 'routes_pre': 7, 'routes_post': 1,
              'hist_occ_a': 0.0, 'hist_occ_b': 1.0, 'post_occ': 0.0,
              'w03_samples': 14400, 'post_samples': 96256, 'direct_flags': 8}
    for key, want in frozen.items():
        if params.get(key) != want:
            reasons.append('param-' + key + '=' + repr(params.get(key)))
    if params.get('tone_amp') != D1_TONE_AMP:
        reasons.append('param-tone_amp')
    if list(params.get('freqs', [])) != D1_FREQS:
        reasons.append('param-freqs')
    branches = {}
    for branch, hist_occ in (('A', 0.0), ('B', 1.0)):
        try:
            header, rows = read_blocks(evdir / (branch + '_blocks.csv'))
        except Exception:
            reasons.append(branch + '-blocks-unreadable')
            continue
        if header != D1_HEADER:
            reasons.append(branch + '-header')
            continue
        if len(rows) != D1_TOTAL:
            reasons.append(branch + '-rowcount')
            continue
        bad = False
        for i, r in enumerate(rows):
            if int(r[0]) != i or int(r[1]) != i + 1:
                reasons.append(branch + '-identity')
                bad = True
                break
        for i, r in enumerate(rows):
            want_occ = hist_occ if i < D1_HIST else 0.0
            if float(r[2]) != want_occ:
                reasons.append(branch + '-occ')
                bad = True
                break
        if any(int(r[3]) != D1_FLAGS for r in rows):
            reasons.append(branch + '-flags')
            bad = True
        for i, r in enumerate(rows):
            want_routes = 7 if i < D1_HIST else 1
            if int(r[4]) != want_routes:
                reasons.append(branch + '-routes')
                bad = True
                break
        if any(int(r[5]) != 1 for r in rows):
            reasons.append(branch + '-render')
            bad = True
        if any(int(r[6]) != 0 for r in rows):
            reasons.append(branch + '-fail')
            bad = True
        branches[branch] = (rows, bad)
    if reasons:
        return False, reasons, det
    stems = {}
    for branch in ('A', 'B'):
        for stem in ('pre_direct', 'pre_path', 'post_direct', 'post_path', 'post_stereo'):
            try:
                vals = read_f32(evdir / (branch + '_' + stem + '.f32'))
            except Exception:
                reasons.append(branch + '-' + stem + '-unreadable')
                continue
            stems[(branch, stem)] = vals
    if len(stems.get(('A', 'pre_direct'), ())) != D1_HIST * D1_BLOCK * 2:
        reasons.append('A-pre-size')
    if len(stems.get(('B', 'pre_direct'), ())) != D1_HIST * D1_BLOCK * 2:
        reasons.append('B-pre-size')
    for key in (('A', 'post_direct'), ('A', 'post_stereo'), ('B', 'post_direct'), ('B', 'post_stereo'), ('A', 'post_path'), ('B', 'post_path')):
        if len(stems.get(key, ())) != D1_POST * D1_BLOCK * 2:
            reasons.append(key[0] + '-' + key[1] + '-size')
    if reasons:
        return False, reasons, det
    for key, vals in stems.items():
        if any(not math.isfinite(v) for v in vals):
            reasons.append(key[0] + '-' + key[1] + '-nonfinite')
            break
    for branch in ('A', 'B'):
        rows, _ = branches[branch]
        segs = ((0, D1_HIST, (branch, 'pre_direct'), 8, 'pre-direct'),
                (D1_HIST, D1_TOTAL, (branch, 'post_direct'), 8, 'post-direct'),
                (0, D1_HIST, (branch, 'pre_path'), 9, 'pre-path'),
                (D1_HIST, D1_TOTAL, (branch, 'post_path'), 9, 'post-path'))
        for r0, r1, stemkey, col, tag in segs:
            vals = stems[stemkey]
            for bi in range(r0, r1):
                off = (bi - r0) * D1_BLOCK * 2
                seg = vals[off:off + D1_BLOCK * 2]
                e = math.fsum(v * v for v in seg)
                rec = float(rows[bi][col])
                if abs(e - rec) / max(1.0, abs(rec), e) > 1e-6:
                    reasons.append(branch + '-' + tag + '-block' + str(bi))
                    break
    pre_a = stems[('A', 'pre_direct')]
    pre_b = stems[('B', 'pre_direct')]
    last_a = pre_a[(D1_HIST - 1) * D1_BLOCK * 2:]
    last_b = pre_b[(D1_HIST - 1) * D1_BLOCK * 2:]
    if any(v != 0.0 for v in last_a):
        reasons.append('hist-last-A-direct-nonzero')
    if not any(v != 0.0 for v in last_b):
        reasons.append('hist-last-B-direct-zero')
    if tuple(pre_a) == tuple(pre_b):
        reasons.append('hist-direct-identical')
    post_a_d = stems[('A', 'post_direct')]
    post_a_s = stems[('A', 'post_stereo')]
    if any(v != 0.0 for v in post_a_d):
        reasons.append('A-obs-direct-nonzero')
    if any(v != 0.0 for v in post_a_s):
        reasons.append('A-obs-stereo-nonzero')
    post_b_s = stems[('B', 'post_stereo')]
    post_b_d = stems[('B', 'post_direct')]
    w = post_b_s[D1_W03 * 2:]
    det['b_e_after'] = math.fsum(v * v for v in w)
    det['b_nz_after'] = sum(1 for v in w if v != 0.0)
    det['b_e_w03'] = math.fsum(v * v for v in post_b_s[:D1_W03 * 2])
    det['b_nz_w03'] = sum(1 for v in post_b_s[:D1_W03 * 2] if v != 0.0)
    det['b_e_all'] = math.fsum(v * v for v in post_b_s)
    det['a_e_all'] = math.fsum(v * v for v in post_a_s)
    wd = post_b_d[D1_W03 * 2:]
    det['b_direct_e_after'] = math.fsum(v * v for v in wd)
    det['b_direct_nz_after'] = sum(1 for v in wd if v != 0.0)
    if reasons:
        return False, reasons, det
    return True, reasons, det


def check_pcm(evdir):
    reasons = []
    for branch in ('A', 'B'):
        try:
            vals = read_f32(evdir / (branch + '_post_stereo.f32'))
            packed_bytes = (evdir / (branch + '_post.pcm16')).read_bytes()
            info = json.loads((evdir / (branch + '_pcm16.json')).read_text(encoding='utf-8'))
        except Exception:
            reasons.append(branch + '-pcm-unreadable')
            continue
        try:
            ok, sub = verify_pcm(vals, packed_bytes, info)
        except ValueError:
            reasons.append(branch + '-pcm-nonfinite')
            continue
        if not ok:
            reasons.extend(branch + '-' + s for s in sub)
    return (not reasons), reasons


def check_tails(evdir):
    reasons = []
    for branch in ('A', 'B'):
        try:
            vals = read_f32(evdir / (branch + '_post_stereo.f32'))
            tail = json.loads((evdir / (branch + '_float_tail.json')).read_text(encoding='utf-8'))
        except Exception:
            reasons.append(branch + '-float-unreadable')
            continue
        ok, sub = verify_float_tail(vals, tail)
        if not ok:
            reasons.extend(branch + '-' + s for s in sub)
    return (not reasons), reasons


OFFLINE_DIRNAME = 'D1-01-offline-01'

D1_EVIDENCE_FILES = ['params.json', 'A_blocks.csv', 'B_blocks.csv',
                     'A_pre_direct.f32', 'A_pre_path.f32', 'A_pre_stereo.f32',
                     'B_pre_direct.f32', 'B_pre_path.f32', 'B_pre_stereo.f32',
                     'A_post_direct.f32', 'A_post_path.f32', 'A_post_stereo.f32',
                     'B_post_direct.f32', 'B_post_path.f32', 'B_post_stereo.f32',
                     'A_post.pcm16', 'B_post.pcm16', 'A_pcm16.json', 'B_pcm16.json',
                     'A_float_tail.json', 'B_float_tail.json', 'verdict.json', 'run.log']


def hash_evidence(evdir):
    digest = {}
    for name in D1_EVIDENCE_FILES:
        digest[name] = hashlib.sha256((evdir / name).read_bytes()).hexdigest()
    return digest


def run_offline(evdir, outdir):
    if outdir.exists():
        raise SystemExit('offline output exists; refusing to overwrite:' + str(outdir))
    outdir.mkdir(parents=True, exist_ok=False)
    log_lines = []
    def log(msg):
        print(msg, flush=True)
        log_lines.append(msg)
    runner_hash = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    (outdir / 'verifier-hashes.json').write_text(json.dumps({'runner_sha256': runner_hash}, indent=2), encoding='utf-8')
    self_ok, cases = verifier_selftest()
    (outdir / 'verifier-selftest.json').write_text(json.dumps({'pass': self_ok, 'cases': cases}, indent=2), encoding='utf-8')
    log('selftest pass=' + repr(self_ok))
    try:
        pre = hash_evidence(evdir)
    except Exception as exc:
        verdict = {'result': 'INCONCLUSIVE', 'reason': 'offline-evidence-missing', 'detail': str(exc)}
        (outdir / 'evidence-hashes-pre.json').write_text(json.dumps({}, indent=2), encoding='utf-8')
        (outdir / 'evidence-hashes-post.json').write_text(json.dumps({}, indent=2), encoding='utf-8')
        (outdir / 'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
        (outdir / 'offline.log').write_text('\n'.join(log_lines) + '\n', encoding='utf-8')
        return 3
    (outdir / 'evidence-hashes-pre.json').write_text(json.dumps(pre, indent=2), encoding='utf-8')
    d_dir = Path('C:/Users/Administrator/AppData/Local/Temp/im-w1d-evidence/9F3C7A2B4D1E4F8A9C6D5E7F0A1B2C3')
    try:
        d_verdict_raw = json.loads((d_dir / 'verdict.json').read_text(encoding='utf-8'))
    except Exception as exc:
        verdict = {'result': 'INCONCLUSIVE', 'reason': 'D-evidence-unreadable', 'detail': str(exc)}
        (outdir / 'evidence-hashes-post.json').write_text(json.dumps(hash_evidence(evdir), indent=2), encoding='utf-8')
        (outdir / 'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
        (outdir / 'offline.log').write_text('\n'.join(log_lines) + '\n', encoding='utf-8')
        return 3
    d_valid, d_reasons, _ = evaluate_control(d_dir)
    d_reject = {'d_verdict_control_ok': d_verdict_raw.get('control_ok'), 'd_checker_valid': d_valid, 'd_reasons': d_reasons}
    log('D reject=' + json.dumps(d_reject))
    try:
        valid, reasons, det = evaluate_control(evdir)
        pcm_ok, pcm_reasons = check_pcm(evdir)
        tail_ok, tail_reasons = check_tails(evdir)
    except Exception as exc:
        valid, reasons, det = False, ['offline-read-failed:' + str(exc)], {}
        pcm_ok, pcm_reasons, tail_ok, tail_reasons = False, ['offline-read-failed'], False, ['offline-read-failed']
    control_ok = valid and pcm_ok and tail_ok and self_ok
    verdict = {'control_ok': control_ok, 'control_reasons': reasons, 'pcm_ok': pcm_ok,
               'pcm_reasons': pcm_reasons, 'tails_ok': tail_ok, 'tail_reasons': tail_reasons,
               'selftest_pass': self_ok, 'd_reject': d_reject, 'offline_only': True,
               'scope': 'H-fixed-input-direct-chain-history-occ0-observation-gt0.3s-nonzero-difference'}
    verdict.update(det)
    b_after = det.get('b_nz_after', 0) > 0 and det.get('b_e_after', 0.0) > 0.0
    verdict['b_after_nonzero'] = b_after
    if (not d_valid) and d_verdict_raw.get('control_ok') is False and control_ok and b_after:
        verdict['result'] = 'PASS-history-holds'
        code = 0
    elif (not d_valid) and control_ok and det.get('b_nz_after', -1) == 0:
        verdict['result'] = 'FAIL-history-falsified'
        code = 10
    else:
        if not self_ok:
            verdict['reason'] = 'verifier-selftest-failed'
        elif d_valid or d_verdict_raw.get('control_ok') is not False:
            verdict['reason'] = 'D-known-invalid-control-accepted'
        else:
            verdict['reason'] = 'control-or-evidence-incomplete'
        verdict['result'] = 'INCONCLUSIVE'
        code = 3
    (outdir / 'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
    post = hash_evidence(evdir)
    (outdir / 'evidence-hashes-post.json').write_text(json.dumps(post, indent=2), encoding='utf-8')
    verdict['evidence_unchanged'] = (pre == post)
    (outdir / 'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
    log('result=' + verdict['result'] + ' code=' + str(code))
    log('control_reasons=' + json.dumps(reasons))
    log('pcm_reasons=' + json.dumps(pcm_reasons))
    log('tail_reasons=' + json.dumps(tail_reasons))
    log('b_after=' + repr(b_after) + ' b_e_after=' + repr(det.get('b_e_after')) + ' b_nz_after=' + repr(det.get('b_nz_after')))
    (outdir / 'offline.log').write_text('\n'.join(log_lines) + '\n', encoding='utf-8')
    return code


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--d-evidence', type=Path, default=Path('C:/Users/Administrator/AppData/Local/Temp/im-w1d-evidence/9F3C7A2B4D1E4F8A9C6D5E7F0A1B2C3'))
    parser.add_argument('--offline-evidence', type=Path, default=None)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        ok_all, cases = verifier_selftest()
        print(json.dumps({'pass': ok_all, 'cases': cases}, indent=2), flush=True)
        return 0 if ok_all else 3
    if args.offline_evidence is not None:
        return run_offline(args.offline_evidence.resolve(), args.output.resolve())
    output = args.output.resolve()
    d_evidence = args.d_evidence.resolve()
    output.mkdir(parents=True, exist_ok=True)
    plugin = Path(__file__).resolve().parents[1]
    sdk = plugin / 'Source/ThirdParty/SteamAudio'
    source = plugin / 'Source/IceMoonAcousticField/Private'
    vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    if not vcvars.is_file():
        raise RuntimeError('MSVC vcvars64.bat not found')
    inputs = [plugin / 'Tests/IM_SteamAudioSmoke.cpp', source/'IMAcousticAudioRenderer.cpp', source/'IMAcousticSDKContext.cpp']
    for path in [output, vcvars, *inputs, sdk, source, Path(__file__)]:
        if any(c in str(path) for c in '\r\n"%&|<>^'):
            raise ValueError('Path contains unsupported shell/response characters')
    runner_path = Path(__file__).resolve()
    tracked = [*inputs, source/'IMAcousticAudioRenderer.h', source/'IMAcousticSDKContext.h', sdk/'IM_SteamAudio481Provenance.json', sdk/'include/phonon.h', sdk/'lib/windows-x64/phonon.dll', sdk/'lib/windows-x64/phonon.lib']
    pre_hashes = {str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in [runner_path, *tracked]}
    (output/'input-hashes-pre.json').write_text(json.dumps(pre_hashes, indent=2), encoding='utf-8')
    d_verdict_raw = json.loads((d_evidence / 'verdict.json').read_text(encoding='utf-8'))
    d_params_raw = json.loads((d_evidence / 'params.json').read_text(encoding='utf-8'))
    d_header_a, d_rows_a = read_blocks(d_evidence / 'A_blocks.csv')
    _, d_rows_b = read_blocks(d_evidence / 'B_blocks.csv')
    d_valid, d_reasons, _ = evaluate_control(d_evidence)
    d_reject = {'d_verdict_control_ok': d_verdict_raw.get('control_ok'), 'd_checker_valid': d_valid, 'd_reasons': d_reasons}
    (output/'d-reject.json').write_text(json.dumps(d_reject, indent=2), encoding='utf-8')
    if d_valid or d_verdict_raw.get('control_ok') is not False:
        print('D negative control NOT rejected; verifier untrusted', flush=True)
        (output/'verdict.json').write_text(json.dumps({'result': 'INCONCLUSIVE', 'reason': 'D-known-invalid-control-accepted', 'd_reject': d_reject}, indent=2), encoding='utf-8')
        return 4
    print('D negative control rejected as expected:', d_reasons, flush=True)
    exe = output/'IM_SteamAudioSmoke.exe'
    incs = ['/I' + str(sdk / 'include'), '/I' + str(source)]
    options = ['/nologo', '/std:c++20', '/EHsc', '/Od', '/Zi', '/MD'] + incs + [str(p) for p in inputs] + ['/Fe:' + str(exe), '/link', '/DEBUG', str(sdk / 'lib/windows-x64/phonon.lib')]
    response = output/'compile.rsp'
    quoted = [('"' + o + '"') if (' ' in o) else o for o in options]
    response.write_text(chr(10).join(quoted), encoding='utf-8')
    build = output/'build.cmd'
    build.write_text('@echo off' + chr(10) + 'call "' + str(vcvars) + '" >nul' + chr(10) + 'if errorlevel 1 exit /b 1' + chr(10) + 'cl @"' + str(response) + '"' + chr(10) + 'exit /b %errorlevel%' + chr(10), encoding='utf-8')
    with (output/'compile.log').open('w', encoding='utf-8') as log:
        try:
            result = subprocess.run(['cmd.exe', '/d', '/c', str(build)], cwd=output, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        except subprocess.TimeoutExpired:
            print('Compile timed out after 180s', flush=True)
            return 2
    if result.returncode:
        print('Compile failed:', output/'compile.log', flush=True)
        return 2
    shutil.copy2(sdk/'lib/windows-x64/phonon.dll', output/'phonon.dll')
    shutil.copy2(sdk/'lib/windows-x64/GPUUtilities.dll', output/'GPUUtilities.dll')
    shutil.copy2(sdk/'lib/windows-x64/TrueAudioNext.dll', output/'TrueAudioNext.dll')
    manifest = {str(p.relative_to(plugin)): hashlib.sha256(p.read_bytes()).hexdigest() for p in tracked}
    (output/'source-hashes.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    with (output/'run.log').open('w', encoding='utf-8') as log:
        try:
            result = subprocess.run([str(exe), str(output)], cwd=output, stdout=log, stderr=subprocess.STDOUT, timeout=30)
        except subprocess.TimeoutExpired:
            print('Native fixture timed out after 30s', flush=True)
            (output/'verdict.json').write_text(json.dumps({'result': 'INCONCLUSIVE', 'reason': 'run-timeout-30s'}, indent=2), encoding='utf-8')
            return 3
    print('Native fixture exit:', result.returncode, 'evidence:', output, flush=True)
    if result.returncode:
        (output/'verdict.json').write_text(json.dumps({'result': 'INCONCLUSIVE', 'reason': 'native-exit-nonzero', 'code': result.returncode}, indent=2), encoding='utf-8')
        return 3
    post_hashes = {str(p): hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in [runner_path, *tracked]}
    (output/'input-hashes-post.json').write_text(json.dumps(post_hashes, indent=2), encoding='utf-8')
    if post_hashes != pre_hashes:
        print('Post-run fingerprint drift', flush=True)
        (output/'verdict.json').write_text(json.dumps({'result': 'INCONCLUSIVE', 'reason': 'fingerprint-drift'}, indent=2), encoding='utf-8')
        return 3
    valid, reasons, det = evaluate_control(output)
    pcm_ok, pcm_reasons = check_pcm(output)
    tail_ok, tail_reasons = check_tails(output)
    control_ok = valid and pcm_ok and tail_ok
    verdict = {'control_ok': control_ok, 'control_reasons': reasons, 'pcm_ok': pcm_ok,
               'pcm_reasons': pcm_reasons, 'tails_ok': tail_ok, 'tail_reasons': tail_reasons,
               'd_reject': d_reject, 'fingerprints_match': True}
    verdict.update(det)
    b_after = det.get('b_nz_after', 0) > 0 and det.get('b_e_after', 0.0) > 0.0
    verdict['b_after_nonzero'] = b_after
    if control_ok and b_after:
        verdict['result'] = 'PASS-history-holds'
        code = 0
    elif control_ok and det.get('b_nz_after', -1) == 0:
        verdict['result'] = 'FAIL-history-falsified'
        code = 10
    else:
        verdict['result'] = 'INCONCLUSIVE'
        code = 3
    (output/'verdict.json').write_text(json.dumps(verdict, indent=2), encoding='utf-8')
    print(json.dumps(verdict, indent=2), flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
