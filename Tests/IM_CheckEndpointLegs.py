#!/usr/bin/env python3
"""Offline C-leg endpoint check (no UE/build)."""
import argparse, csv, hashlib, json, math
from pathlib import Path
END_EPS_M = 1e-4
def sha256(p):
    import hashlib as h
    return h.sha256(Path(p).read_bytes()).hexdigest()
def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def dot(a, b): return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
def seg_tri_hit(A, B, V0, V1, V2, eps):
    D = sub(B, A)
    ln = math.sqrt(dot(D, D))
    if not (ln > 1e-12): return False
    t0 = eps/ln; t1 = 1.0-t0
    if t1 <= t0: return False
    E1 = sub(V1, V0); E2 = sub(V2, V0)
    P = cross(D, E2)
    det = dot(E1, P)
    if -1e-12 < det < 1e-12: return False
    inv = 1.0/det
    S = sub(A, V0)
    u = dot(S, P)*inv
    if u < -1e-9 or u > 1.0+1e-9: return False
    Q = cross(S, E1)
    v = dot(D, Q)*inv
    if v < -1e-9 or u+v > 1.0+1e-9: return False
    t = dot(E2, Q)*inv
    return t0 <= t <= t1
def oracle_viol(A, B, verts, tris):
    h = 0
    for (i, j, k) in tris:
        if seg_tri_hit(A, B, verts[i], verts[j], verts[k], END_EPS_M): h += 1
    return h
def load_obj(p):
    verts, tris = [], []
    for line in Path(p).read_text(encoding='utf-8').splitlines():
        if line.startswith('v '):
            _, x, y, z = line.split()[:4]
            verts.append((float(x), float(y), float(z)))
        elif line.startswith('f '):
            parts = line.split()[1:4]
            tris.append(tuple(int(t.split('/')[0])-1 for t in parts))
    return verts, tris
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--evidence', required=True)
    ap.add_argument('--output', required=True)
    a = ap.parse_args()
    ev = Path(a.evidence); out = Path(a.output)
    out.mkdir(parents=True, exist_ok=True)
    verts, tris = load_obj(ev/'w2-whitebox')
    rows = list(csv.DictReader((ev/'w2-per-point.csv').read_text(encoding='utf-8').splitlines()))
    segs = list(csv.DictReader((ev/'w2-callback-segments.csv').read_text(encoding='utf-8').splitlines()))
    by_case = {}
    for s in segs: by_case.setdefault(s['case'], []).append(s)
    out_rows = []; fails = 0; ep_checked = 0
    for r in rows:
        name = r['case']
        S = (float(r['src_sdk_x_m']), float(r['src_sdk_y_m']), float(r['src_sdk_z_m']))
        L = (float(r['lst_sdk_x_m']), float(r['lst_sdk_y_m']), float(r['lst_sdk_z_m']))
        dh = oracle_viol(S, L, verts, tris)
        dh_ok = (dh == int(float(r['direct_seg_hits'])))
        exp = r['expected_direct']; occ = float(r['occlusion'])
        if exp == 'blocked': occ_ok = occ <= 0.01
        elif exp == 'clear': occ_ok = occ >= 0.99
        else: occ_ok = True
        pv = int(float(r['path_valid']))
        s2p = p2p = p2l = 'NA'; s2p_h = p2l_h = -1; p2p_max = -1
        leg_ok = True
        if pv:
            cs = [s for s in by_case.get(name, []) if s['occluded'] == '0']
            if not cs: leg_ok = False
            else:
                F0 = (float(cs[0]['from_x']), float(cs[0]['from_y']), float(cs[0]['from_z']))
                T1 = (float(cs[-1]['to_x']), float(cs[-1]['to_y']), float(cs[-1]['to_z']))
                s2p_h = oracle_viol(S, F0, verts, tris)
                p2l_h = oracle_viol(T1, L, verts, tris)
                s2p = 'PASS' if s2p_h == 0 else 'FAIL'
                p2l = 'PASS' if p2l_h == 0 else 'FAIL'
                mx = 0
                for s in cs:
                    F = (float(s['from_x']), float(s['from_y']), float(s['from_z']))
                    T = (float(s['to_x']), float(s['to_y']), float(s['to_z']))
                    hh = oracle_viol(F, T, verts, tris)
                    mx = max(mx, hh)
                    if hh != int(s['oracle_hit']): leg_ok = False
                p2p_max = mx
                p2p = 'PASS' if mx == 0 else 'FAIL'
                if s2p != 'PASS' or p2l != 'PASS' or p2p != 'PASS': leg_ok = False
                ep_checked += 1
        row_pass = dh_ok and occ_ok and leg_ok
        if not row_pass: fails += 1
        out_rows.append({'case': name, 'group': r['group'], 'path_valid': pv, 'direct_hits_recomputed': dh, 'direct_hits_native': int(float(r['direct_seg_hits'])), 'direct_hits_match': dh_ok, 'occlusion_agree': occ_ok, 's2p': s2p, 's2p_hits': s2p_h, 'p2p': p2p, 'p2p_max_hits': p2p_max, 'p2l': p2l, 'p2l_hits': p2l_h, 'pass': row_pass})
    exph = None
    try: exph = open(ev/'w2-cases.txt', encoding='utf-8').readline().strip()
    except Exception: exph = 'unreadable'
    with open(out/'endpoint-legs.csv', 'w', newline='', encoding='utf-8') as f:
        w = csv.DictWriter(f, fieldnames=list(out_rows[0].keys()))
        w.writeheader(); w.writerows(out_rows)
    summary = {'scope': 'c-endpoint-legs-offline-w2-75', 'points': len(rows), 'path_valid_points': sum(1 for r in rows if int(float(r['path_valid'])) == 1), 'endpoint_checked_points': ep_checked, 'failed_points': fails, 'passed': fails == 0, 'epsilon_m': END_EPS_M, 'w2_cases_header': exph, 'inputs': {n: sha256(ev/n) for n in ['w2-whitebox', 'w2-per-point.csv', 'w2-callback-segments.csv']}, 'obj_verts': len(verts), 'obj_tris': len(tris)}
    (out/'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary, indent=2))
if __name__ == '__main__': main()
