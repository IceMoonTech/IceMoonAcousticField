"""Recompute W3.RoomRecordings' oracle from actual UE PCM16 WAVs.

Run after the root-owned UE automation:
  python IM_AnalyzeAcousticRooms.py --evidence <copy>/Saved/AcousticV2/W3-Rooms/<id>
Exit 0 means these four recordings/metadata satisfy the room gate. It does not
claim lifecycle, performance, independent code review, or user audition passed.
Exit 2 is a counterexample/incomplete gate; exit 1 is an invocation/I/O error.
All generated analysis stays under this script's project Saved/AcousticV2.
"""
import argparse
import array
import csv
import hashlib
import json
import math
from pathlib import Path
import sys
import wave

RATE = 48000
GAP = 0.10
CASES = ("closed-low", "closed-high", "corridor", "outdoor-notop")
SCOPE = "UE-device-room-wet-decay"


def require(value, message):
    if not value:
        raise ValueError(message)


def read_json(path):
    def invalid(value):
        raise ValueError(f"non-finite JSON number: {value}")
    return json.loads(path.read_text(encoding="utf-8-sig"), parse_constant=invalid)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def analyze_wave(path, saved_ir_s):
    with wave.open(str(path), "rb") as wav:
        require((wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getcomptype())
                == (2, 2, RATE, "NONE"), "expected 48 kHz stereo PCM16")
        frames = wav.getnframes()
        require(5.5 * RATE <= frames <= 35 * RATE, "incomplete or unbounded capture length")
        raw = wav.readframes(frames)
        require(len(raw) == frames * 4, "truncated WAV data")
    pcm = array.array("h", raw)
    if sys.byteorder != "little":
        pcm.byteswap()
    energy = [(pcm[i] ** 2 + pcm[i + 1] ** 2) / 32768.0 ** 2 for i in range(0, len(pcm), 2)]
    total = math.fsum(energy)
    peak = max(abs(v) for v in pcm)
    first = next((i for i, e in enumerate(energy) if e > 0), -1)
    leading = math.fsum(energy[:RATE // 4])
    trailing = math.fsum(energy[-RATE // 2:])
    require(total > 0 and peak < 32760, "silent or clipped wet impulse")
    require(leading == trailing == 0, "non-silent pre-roll or truncated/contaminated tail")
    require(RATE / 4 <= first <= RATE * 1.5, "wet impulse onset outside capture contract")
    require(frames - first >= (saved_ir_s + .5) * RATE, "full saved IR tail not captured")
    edc = [0.0] * frames
    suffix = 0.0
    for i in range(frames - 1, -1, -1):
        suffix += energy[i]
        edc[i] = 10 * math.log10(max(suffix / total, 1e-30))
    start = next((i for i, d in enumerate(edc) if d <= -5), None)
    end = next((i for i, d in enumerate(edc) if d <= -25), None)
    require(start is not None and end is not None and end > start, "missing -5..-25 dB interval; no T20")
    times = [(i - start) / RATE for i in range(start, end + 1)]
    dbs = edc[start:end + 1]
    count = len(times)
    st, sd = math.fsum(times), math.fsum(dbs)
    stt = math.fsum(t * t for t in times)
    std = math.fsum(t * d for t, d in zip(times, dbs))
    den = count * stt - st * st
    require(den > 0, "degenerate decay fit")
    slope = (count * std - st * sd) / den
    require(math.isfinite(slope) and slope < 0, "non-decaying fit")
    # As in nativeDecay, this is RT60 extrapolated from the T20 fit interval.
    t20 = -60 / slope
    require(math.isfinite(t20) and t20 > 0, "invalid T20")
    result = {"frames": frames, "sample_rate_hz": RATE, "channels": 2,
              "total_energy": total, "peak_abs_pcm16": peak, "first_nonzero_frame": first,
              "leading_quarter_second_energy": leading, "trailing_half_second_energy": trailing,
              "t20_s": t20, "slope_db_per_s": slope, "fit_samples": count,
              "edc_minus5_frame": start, "edc_minus25_frame": end, "decay_fit_usable": True}
    windows = [(i / RATE, math.fsum(energy[i:i + 480]), edc[i]) for i in range(0, frames, 480)]
    return result, windows


def validate_case(directory, name):
    report = read_json(directory / "room.json")
    metadata = read_json(directory / "bake-metadata.json")
    require(report["scope"] == SCOPE and report["name"] == name, "wrong evidence scope/case")
    require(report["map"].startswith("/IceMoonAcousticField/Tests/Rooms/IM_"), "not an isolated room map")
    require(report["sdk_version"] == metadata["sdk_version"] == 0x040801, "wrong SDK version")
    require(report["recipe_version"] == metadata["recipe_version"], "recipe mismatch")
    require(report["exported_triangles"] == metadata["triangles"] > 0, "missing exported triangles")
    require(report["generated_probes"] == metadata["probe_count"] == len(metadata["probes_m"]) > 0, "missing probes")
    require(report["mesh_actors"] == (5 if name == "outdoor-notop" else 6), "incomplete fixture geometry")
    require(report["probe_spacing_cm"] == metadata["probe_spacing_cm"] == 100
            and report["probe_height_cm"] == metadata["probe_height_cm"] == 150, "wrong probe configuration")
    absorption = .8 if name == "closed-high" else .1
    require(report["absorption_each_band"] == absorption, "wrong absorption control")
    require(len(metadata["materials"]) == 1, "unexpected material set")
    material = metadata["materials"][0]
    require(all(abs(x - absorption) < 1e-6 for x in material["absorption"])
            and len(material["absorption"]) == 3 and material["scattering"] == .5, "baked material differs from control")
    require(report["scattering"] == .5 and report["transmission_each_band"] == 0, "wrong scattering/transmission")
    require(report["wet_gain"] == 1 and report["source_listener_distance_cm"] == 100, "gain/distance differs")
    saved = metadata["reflection"]["saved_duration_s"]
    require(saved == report["saved_ir_s"] and saved > 0 and metadata["reflection"]["type"] == "CONVOLUTION", "wrong baked reflection")
    require((directory / "scene.bin").stat().st_size > 0 and (directory / "probes.bin").stat().st_size > 0,
            "missing actual SDK bake payload")
    raw = (directory / "input-pcm16.bin").read_bytes()
    pcm = array.array("h", raw)
    if sys.byteorder != "little":
        pcm.byteswap()
    require(len(pcm) == math.ceil((.5 + saved + .75) * RATE), "queued impulse payload length mismatch")
    require(report["impulse_pcm16"] == 16384 and pcm[RATE // 2] == 16384
            and sum(v != 0 for v in pcm) == 1, "queued input is not the fixed single impulse")
    for key in ("direct_nonzero_blocks", "path_nonzero_blocks", "degraded_blocks", "reverb_rejected_blocks", "dry_dropped_blocks"):
        require(report[key] == 0, f"invalid wet-only capture: {key}={report[key]}")
    require(report["device_block_frames"] > 0 and report["world_epoch"] > 0, "missing device/world binding")
    expected = 6 * RATE / report["device_block_frames"]
    require(report["wet_nonzero_blocks"] > 0 and report["reverb_processed_blocks"] >= expected * .9
            and report["reverb_dry_blocks"] >= expected * .9, "wet output/input processing gate failed")
    require(report["capture_counters_pass"] is True and report["passed"] is True, "UE did not pass this capture")
    metrics, windows = analyze_wave(directory / "wet.wav", saved)
    require(math.isclose(metrics["t20_s"], report["t20_s"], rel_tol=1e-6, abs_tol=1e-8), "UE/Python T20 disagreement")
    files = ("wet.wav", "room.json", "bake-metadata.json", "geometry-identity.txt", "input-pcm16.bin", "scene.bin", "probes.bin")
    return {"name": name, **metrics, "hashes_sha256": {f: sha256(directory / f) for f in files}}, windows, metadata, report


def analyze_run(evidence, output):
    output.mkdir(parents=True, exist_ok=True)
    result = {"scope": SCOPE, "analyzer": "independent-python-wav-recalculation", "passed": False,
              "required_relative_gap": GAP, "cases": [], "errors": []}
    metas, reports = {}, {}
    for name in CASES:
        try:
            case, windows, metadata, report = validate_case(evidence / name, name)
            result["cases"].append(case)
            metas[name], reports[name] = metadata, report
            with (output / f"{name}-energy-10ms.csv").open("w", newline="", encoding="utf-8") as stream:
                writer = csv.writer(stream, lineterminator="\n")
                writer.writerow(("time_s", "energy", "edc_db")); writer.writerows(windows)
        except (OSError, ValueError, KeyError, TypeError, wave.Error, OverflowError) as exc:
            result["errors"].append(f"{name}: {exc}")
    if len(result["cases"]) == 4:
        try:
            by_name = {c["name"]: c for c in result["cases"]}
            low, high = by_name["closed-low"], by_name["closed-high"]
            require(low["hashes_sha256"]["geometry-identity.txt"] == high["hashes_sha256"]["geometry-identity.txt"], "low/high geometry differs")
            require(len({c["hashes_sha256"]["input-pcm16.bin"] for c in result["cases"]}) == 1, "room inputs differ")
            for key in ("recipe_version", "sdk_version", "triangles", "probe_count", "probe_spacing_cm", "probe_height_cm",
                        "bounds_origin_cm", "bounds_extent_cm", "probes_m", "reflection", "path"):
                require(metas["closed-low"][key] == metas["closed-high"][key], f"low/high bake controls differ: {key}")
            require(metas["closed-low"]["materials"][0]["asset_path"] == metas["closed-high"]["materials"][0]["asset_path"], "different closed material assets")
            for key in ("source_sdk_m", "listener_sdk_m", "wet_gain", "source_listener_distance_cm", "device_block_frames"):
                require(reports["closed-low"][key] == reports["closed-high"][key], f"low/high playback controls differ: {key}")
            gap = (low["t20_s"] - high["t20_s"]) / high["t20_s"]
            result["relative_gap"] = gap
            require(gap >= GAP, "closed-low decay does not exceed closed-high by 10%")
            terminal = read_json(evidence / "terminal.json")
            summary = read_json(evidence / "summary.json")
            require(terminal["passed"] is True and terminal["cases_completed"] == 4
                    and summary["passed"] is True and summary["scope"] == SCOPE, "UE room automation did not complete")
            result["passed"] = True
        except (OSError, ValueError, KeyError, TypeError) as exc:
            result["errors"].append(str(exc))
    result["analyzer_sha256"] = sha256(Path(__file__))
    (output / "analysis.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8", newline="\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="Default: <evidence>/analysis")
    args = parser.parse_args()
    try:
        evidence = args.evidence.resolve(strict=True)
        output = (args.output or evidence / "analysis").resolve()
        saved = (Path(__file__).resolve().parents[3] / "Saved/AcousticV2").resolve()
        require(output.is_relative_to(saved), "analysis writes must stay in this copy's Saved/AcousticV2")
        result = analyze_run(evidence, output)
        print(json.dumps({"passed": result["passed"], "errors": result["errors"], "output": str(output)}, ensure_ascii=False))
        return 0 if result["passed"] else 2
    except (OSError, ValueError) as exc:
        print(f"Analyzer error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
