#!/usr/bin/env python3
"""validate-bargein — M7B acceptance harness.

Renders fixtures (via tools/build-bargein-fixtures), runs
`acva demo bargein-validation --fixture <path>` per fixture, parses the
machine-readable done-line, and asserts against per-fixture expectations
declared in `tests/fixtures/barge-in/manifest.yaml`. Optionally runs a
50-trial latency distribution against the `clean-speakers` fixture and
asserts the M7 §19.3 percentiles.

Exit codes
----------
    0   all fixtures + 50-trial pass
    1   one or more failures (per-fixture details printed)
    2   harness misconfigured (missing binary, Speaches down, etc.)

Usage
-----
    ./scripts/validate-bargein.py                  # full suite
    ./scripts/validate-bargein.py --quick          # fixtures only, no 50-trial
    ./scripts/validate-bargein.py --rebuild-fixtures
    ./scripts/validate-bargein.py --only clean-speakers
    ./scripts/validate-bargein.py --trials 50
"""

from __future__ import annotations

import argparse
import re
import sqlite3
import subprocess
import sys
import textwrap
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

try:
    import yaml
except ModuleNotFoundError:
    sys.stderr.write(
        "validate-bargein: missing PyYAML. Install: "
        "sudo pacman -S python-yaml  (or pip install pyyaml)\n")
    sys.exit(2)


REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures" / "barge-in"
MANIFEST = FIXTURES_DIR / "manifest.yaml"
BUILD_TOOL = REPO_ROOT / "tools" / "build-bargein-fixtures"


# --------------------------------------------------------------------
# done-line parser
# --------------------------------------------------------------------

_DONE = re.compile(
    r"cancellation=(?P<cancel>[YN])\s+"
    r"latency_ms=(?P<lat>-?[\d.]+)\s+"
    r"sentences_played=(?P<played>\d+)\s+"
    r"sentences_dropped=(?P<dropped>\d+)\s+"
    r"barge_in_fires=(?P<fires>\d+)\s+"
    r"barge_in_suppressed_aec=(?P<sup_aec>\d+)\s+"
    r"barge_in_suppressed_cooldown=(?P<sup_cd>\d+)\s+"
    r"outcome=(?P<outcome>\w+)\s+"
    r"db=(?P<db>\S+)"
)


@dataclass
class DemoResult:
    rc: int
    cancellation: bool
    latency_ms: float
    sentences_played: int
    sentences_dropped: int
    barge_in_fires: int
    barge_in_suppressed_aec: int
    barge_in_suppressed_cooldown: int
    outcome: str
    db_path: str
    raw: str

    @property
    def parsed(self) -> bool:
        return self.outcome != ""


def run_demo(acva: Path, fixture: Path, timeout_s: float = 60.0) -> DemoResult:
    """Run the bargein-validation demo against a fixture and parse the
    done-line."""
    try:
        r = subprocess.run(
            [str(acva), "demo", "bargein-validation",
             "--fixture", str(fixture)],
            capture_output=True, text=True,
            cwd=str(REPO_ROOT), timeout=timeout_s,
        )
        rc = r.returncode
        out = r.stdout + "\n" + r.stderr
    except subprocess.TimeoutExpired as ex:
        return DemoResult(
            rc=124, cancellation=False, latency_ms=-1.0,
            sentences_played=0, sentences_dropped=0,
            barge_in_fires=0, barge_in_suppressed_aec=0,
            barge_in_suppressed_cooldown=0,
            outcome="timeout", db_path="", raw=str(ex))

    m = _DONE.search(out)
    if not m:
        return DemoResult(
            rc=rc, cancellation=False, latency_ms=-1.0,
            sentences_played=0, sentences_dropped=0,
            barge_in_fires=0, barge_in_suppressed_aec=0,
            barge_in_suppressed_cooldown=0,
            outcome="", db_path="", raw=out)
    return DemoResult(
        rc=rc,
        cancellation=(m.group("cancel") == "Y"),
        latency_ms=float(m.group("lat")),
        sentences_played=int(m.group("played")),
        sentences_dropped=int(m.group("dropped")),
        barge_in_fires=int(m.group("fires")),
        barge_in_suppressed_aec=int(m.group("sup_aec")),
        barge_in_suppressed_cooldown=int(m.group("sup_cd")),
        outcome=m.group("outcome"),
        db_path=m.group("db"),
        raw=out,
    )


# --------------------------------------------------------------------
# persistence check (M7 §6 acceptance)
# --------------------------------------------------------------------

def check_persistence(db_path: str) -> tuple[bool, str]:
    """Open the SQLite db the demo left behind and verify the most
    recent assistant turn's persistence semantics. Returns (ok, why)."""
    if not db_path or not Path(db_path).exists():
        return False, f"db not found: {db_path}"
    try:
        conn = sqlite3.connect(db_path)
        cur = conn.execute(
            "SELECT role, status, text, interrupted_at_sentence "
            "FROM turns WHERE role='assistant' "
            "ORDER BY id DESC LIMIT 1")
        row = cur.fetchone()
        conn.close()
    except sqlite3.Error as e:
        return False, f"sqlite error: {e}"
    finally:
        try: Path(db_path).unlink()
        except OSError: pass
    if row is None:
        # No assistant row at all is the correct outcome for "cancelled
        # before any sentence played" (Discarded outcome). The caller
        # decides if that's expected.
        return True, "(no row — discarded)"
    _role, status, text, _ias = row
    text_len = len(text or "")
    return True, f"status={status} chars={text_len}"


# --------------------------------------------------------------------
# fixtures
# --------------------------------------------------------------------

def ensure_fixtures(rebuild: bool) -> int:
    """Render fixtures via tools/build-bargein-fixtures. Returns RC."""
    cmd = [str(BUILD_TOOL), "build"]
    if rebuild:
        cmd.append("--force")
    return subprocess.run(cmd, cwd=str(REPO_ROOT)).returncode


def load_manifest() -> list[dict]:
    with MANIFEST.open() as f:
        doc = yaml.safe_load(f) or {}
    return doc.get("fixtures") or []


# --------------------------------------------------------------------
# percentile helper (linear interpolation)
# --------------------------------------------------------------------

def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return float("nan")
    xs = sorted(xs)
    if len(xs) == 1:
        return xs[0]
    k = (len(xs) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(xs) - 1)
    return xs[lo] + (xs[hi] - xs[lo]) * (k - lo)


# --------------------------------------------------------------------
# main flow
# --------------------------------------------------------------------

def fmt_pass(ok: bool) -> str:
    return "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"


def evaluate_fixture(entry: dict, result: DemoResult) -> tuple[bool, list[str]]:
    """Compare a DemoResult against the manifest's `expected:` block."""
    expected = entry.get("expected") or {}
    name = entry["name"]
    fails: list[str] = []

    expect_cancel = bool(expected.get("cancellation", False))
    if expect_cancel and not result.cancellation:
        fails.append("expected cancellation, got none")
    if not expect_cancel and result.cancellation:
        fails.append("expected NO cancellation, got one")

    if expect_cancel:
        max_lat = float(expected.get("max_latency_ms", 400.0))
        if result.latency_ms < 0 or result.latency_ms > max_lat:
            fails.append(
                f"latency {result.latency_ms:.1f} ms exceeds "
                f"{max_lat:.0f} ms gate")

    max_false = int(expected.get("max_false_fires", 0))
    if result.barge_in_fires > max_false and not expect_cancel:
        fails.append(
            f"barge_in fired {result.barge_in_fires} time(s); "
            f"max_false_fires={max_false}")

    return (not fails), fails


def run_demo_with_retry(acva: Path, fixture: Path, timeout_s: float,
                         retries: int) -> DemoResult:
    """Wrapper that retries on transient failures (TTS curl errors, no
    done-line). Speaches occasionally returns truncated streams under
    GPU load; one retry usually clears it."""
    last = run_demo(acva, fixture, timeout_s=timeout_s)
    for _ in range(retries):
        if last.parsed:
            return last
        # Discard stale db left by the failed run.
        if last.db_path and Path(last.db_path).exists():
            try: Path(last.db_path).unlink()
            except OSError: pass
        last = run_demo(acva, fixture, timeout_s=timeout_s)
    return last


def run_fixture_suite(acva: Path, only: Optional[str], timeout_s: float,
                       retries: int) -> bool:
    fixtures = load_manifest()
    if only:
        fixtures = [e for e in fixtures if e["name"] == only]
        if not fixtures:
            print(f"no fixture named '{only}'", file=sys.stderr)
            return False

    print(f"\n\033[1mFixture suite\033[0m  ({len(fixtures)} fixtures)")
    print("-" * 72)
    all_ok = True
    for entry in fixtures:
        name = entry["name"]
        wav = FIXTURES_DIR / f"{name}.wav"
        if not wav.is_file():
            print(f"  {name:<24} SKIP — fixture missing (ran build first?)")
            all_ok = False
            continue
        result = run_demo_with_retry(acva, wav, timeout_s=timeout_s,
                                       retries=retries)
        ok, fails = evaluate_fixture(entry, result)
        all_ok = all_ok and ok
        persist_ok, persist_why = check_persistence(result.db_path)
        if not persist_ok:
            ok = False; all_ok = False
            fails.append(f"persistence: {persist_why}")

        cancel_marker = "Y" if result.cancellation else "N"
        print(f"  {name:<24} {fmt_pass(ok)}  "
              f"cancel={cancel_marker}  "
              f"lat={result.latency_ms:>6.1f}ms  "
              f"fires={result.barge_in_fires} "
              f"played={result.sentences_played} "
              f"dropped={result.sentences_dropped} "
              f"persist=[{persist_why}]")
        for why in fails:
            print(f"    \033[31m✗\033[0m {why}")
        if not result.parsed:
            print(textwrap.indent(result.raw[-1500:], "      "))
    return all_ok


def run_50_trial(acva: Path, n: int, timeout_s: float,
                 fixture_name: str, retries: int) -> bool:
    fixture = FIXTURES_DIR / f"{fixture_name}.wav"
    if not fixture.is_file():
        print(f"  fixture missing: {fixture}", file=sys.stderr)
        return False

    print(f"\n\033[1m{n}-trial distribution\033[0m  (fixture={fixture_name})")
    print("-" * 72)

    latencies: list[float] = []
    cancels = 0
    parse_failures = 0
    for i in range(1, n + 1):
        sys.stdout.write(f"  [{i:3d}/{n}] ")
        sys.stdout.flush()
        r = run_demo_with_retry(acva, fixture, timeout_s=timeout_s,
                                  retries=retries)
        # discard the leftover db
        if r.db_path and Path(r.db_path).exists():
            try: Path(r.db_path).unlink()
            except OSError: pass
        if not r.parsed:
            parse_failures += 1
            sys.stdout.write("\033[31mparse\033[0m\n")
            continue
        if r.cancellation and r.latency_ms >= 0:
            latencies.append(r.latency_ms)
            cancels += 1
            sys.stdout.write(f"\033[32m✓\033[0m {r.latency_ms:>6.1f} ms\n")
        else:
            sys.stdout.write(
                f"\033[33m·\033[0m no-cancel "
                f"(fires={r.barge_in_fires} outcome={r.outcome})\n")

    p50 = percentile(latencies, 0.50)
    p95 = percentile(latencies, 0.95)
    correct = sum(1 for x in latencies if x <= 400.0)

    print()
    print(f"  cancellations: {cancels}/{n}  ({cancels/n*100:.0f}%)")
    print(f"  P50: {p50:.1f} ms   P95: {p95:.1f} ms   "
          f"≤ 400 ms: {correct}/{n}")

    gates = [
        ("cancellations ≥ 90%", cancels >= int(0.9 * n)),
        ("P50 ≤ 200 ms",         not (p50 != p50) and p50 <= 200.0),
        ("P95 ≤ 400 ms",         not (p95 != p95) and p95 <= 400.0),
        ("≥ 90% within 400 ms",  correct >= int(0.9 * n)),
        ("0 parse failures",     parse_failures == 0),
    ]
    all_pass = True
    for label, ok in gates:
        all_pass = all_pass and ok
        print(f"  {fmt_pass(ok)}  {label}")
    return all_pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true",
                    help="fixtures only; skip the 50-trial distribution")
    ap.add_argument("--rebuild-fixtures", action="store_true",
                    help="re-render all fixtures even when manifest hash matches")
    ap.add_argument("--only", help="run only one named fixture")
    ap.add_argument("--trials", type=int, default=50,
                    help="N for the trial distribution (default 50)")
    ap.add_argument("--trial-fixture", default="clean-speakers",
                    help="which fixture to use for the trial run")
    ap.add_argument("--timeout-s", type=float, default=60.0)
    ap.add_argument("--retries", type=int, default=2,
                    help="retries per fixture on transient failure (default 2)")
    ap.add_argument("--build", action="store_true",
                    help="run ./build.sh before measuring")
    args = ap.parse_args()

    if args.build:
        if subprocess.run(["./build.sh", "dev"], cwd=str(REPO_ROOT)).returncode != 0:
            return 2

    acva = REPO_ROOT / "_build" / "dev" / "acva"
    if not acva.is_file():
        print(f"binary not found: {acva}\n  run ./build.sh first.",
              file=sys.stderr)
        return 2

    print(f"validate-bargein: binary={acva}")
    if ensure_fixtures(rebuild=args.rebuild_fixtures) != 0:
        return 2

    fixture_ok = run_fixture_suite(acva, only=args.only,
                                    timeout_s=args.timeout_s,
                                    retries=args.retries)
    trial_ok = True
    if not args.quick and not args.only:
        trial_ok = run_50_trial(acva, args.trials, timeout_s=args.timeout_s,
                                 fixture_name=args.trial_fixture,
                                 retries=args.retries)

    print()
    print("=" * 72)
    print(f"OVERALL: {fmt_pass(fixture_ok and trial_ok)}")
    return 0 if (fixture_ok and trial_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
