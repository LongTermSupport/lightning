#!/usr/bin/env python3
"""Runs the real test-job scripts of .gitlab/remote-build.yml against a fake
`nix` that plays canned cargo/ctest output and exit status, and checks which
outcomes the gates accept. No build happens.

Usage: test-ci-test-gates.py [path/to/remote-build.yml]
"""
import os
import stat
import subprocess
import sys
import tempfile

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
YML = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", ".gitlab", "remote-build.yml")

errors = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        sys.stdout.flush()
        print(f"  FAIL: {msg}", file=sys.stderr)
        errors.append(msg)


class Loader(yaml.SafeLoader):
    pass


Loader.add_constructor("!reference", lambda l, n: ("!reference", l.construct_sequence(n)))
with open(YML) as f:
    doc = yaml.load(f, Loader=Loader)


def script_of(job):
    lines = []
    for line in doc[job]["script"]:
        if isinstance(line, tuple):
            target, key = line[1]
            if target == ".qt-test-env":  # writes /etc; irrelevant to the gate
                continue
            lines += doc[target][key]
        else:
            lines.append(line)
    return lines


FAKE_NIX = r"""#!/usr/bin/env bash
[[ $1 == develop ]] || { echo /fake; exit 0; }
shift 2
case " $* " in
    *" cargo test "*) cat "$FAKE_DIR/cargo.out"; exit "$(cat "$FAKE_DIR/cargo.rc")" ;;
    *" ctest "*" -N "*) cat "$FAKE_DIR/registered.out" ;;
    *" ctest "*) cat "$FAKE_DIR/ctest.out"; exit "$(cat "$FAKE_DIR/ctest.rc")" ;;
esac
exit 0
"""


def run_job(job, files):
    with tempfile.TemporaryDirectory() as work:
        os.mkdir(os.path.join(work, "bin"))
        nix = os.path.join(work, "bin", "nix")
        with open(nix, "w") as f:
            f.write(FAKE_NIX)
        os.chmod(nix, stat.S_IRWXU)
        fake = os.path.join(work, "fake")
        os.mkdir(fake)
        for name, text in files.items():
            with open(os.path.join(fake, name), "w") as f:
                f.write(text)
        proj = os.path.join(work, "proj")
        os.mkdir(proj)
        # GitLab's bash runner executes the whole script under `set -eo pipefail`.
        body = "set -eo pipefail\n" + "\n".join(script_of(job)) + "\n"
        env = dict(os.environ, PATH=f"{work}/bin:{os.environ['PATH']}", FAKE_DIR=fake)
        p = subprocess.run(["bash", "-c", body], cwd=proj, env=env, capture_output=True, text=True)
        out = {}
        outdir = os.path.join(proj, "out")
        if os.path.isdir(outdir):
            for name in os.listdir(outdir):
                with open(os.path.join(outdir, name)) as f:
                    out[name] = f.read()
        return p.returncode, out, p.stdout + p.stderr


OK_SUMMARY = "running 412 tests\ntest result: ok. 412 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 9.10s\n"

print("== test-rust: the cargo exit status decides ==")
cases = [
    ("passing summary, exit 0", OK_SUMMARY, 0, True),
    ("passing summary, then a later process error, exit 101", OK_SUMMARY
     + "error: test failed, to rerun pass `--doc`\nCaused by: process didn't exit successfully (signal: 11)\n", 101, False),
    ("explicit failed-test summary", "test result: FAILED. 411 passed; 1 failed; 0 ignored\n", 101, False),
    ("startup/compiler failure", "error[E0425]: cannot find value `x` in this scope\nerror: could not compile `lightning`\n", 101, False),
    ("exit 0 but no test ran", "test result: ok. 0 passed; 0 failed; 0 ignored\n", 0, False),
]
for name, log, rc, accept in cases:
    code, out, _ = run_job("test-rust", {"cargo.out": log, "cargo.rc": str(rc)})
    check((code == 0) == accept, f"{name}: {'accepted' if accept else 'refused'}")
    check(out.get("cargo.rc", "").strip() == str(rc), f"{name}: out/cargo.rc records {rc}")
    check(out.get("cargo.log") == log, f"{name}: out/cargo.log is the complete producer output")


def ctest_log(lines, total, failed_final):
    pct = 100 * (total - len(failed_final)) // total
    tail = f"\n{pct}% tests passed, {len(failed_final)} tests failed out of {total}\n"
    if failed_final:
        tail += "\nThe following tests FAILED:\n" + "".join(f"\t  1 - {t} (Failed)\n" for t in failed_final)
    return "Lightning dev shell\n" + "\n".join(lines) + tail


FIRST = [
    " 1/3 Test  #1: alpha ............................   Passed    1.00 sec",
    " 2/3 Test  #2: beta-qml .........................***Failed   60.63 sec",
    " 3/3 Test  #3: gamma ............................***Timeout 1500.02 sec",
]
RETRY_OK = [
    "      Test  #2: beta-qml .........................   Passed   11.02 sec",
    "      Test  #3: gamma ............................   Passed    2.00 sec",
]
CLEAN = [
    " 1/3 Test  #1: alpha ............................   Passed    1.00 sec",
    " 2/3 Test  #2: beta-qml .........................   Passed    1.00 sec",
    " 3/3 Test  #3: gamma ............................   Passed    1.00 sec",
]
HEADER = "test\tfirst_attempt\tfinal\n"

for job, log in (("test-nixos", "ctest"), ("test-nonrust", "ctest-nonrust")):
    print(f"== {job}: first attempts are kept, every registered test must run ==")
    tsv = f"{log}-first-attempt-failures.tsv"

    def run(lines, failed_final, rc, registered=3, total=3):
        return run_job(job, {"ctest.out": ctest_log(lines, total, failed_final), "ctest.rc": str(rc),
                             "registered.out": f"Lightning dev shell\nTest project /x\nTotal Tests: {registered}\n"})

    code, out, _ = run(CLEAN, [], 0)
    check(code == 0, "a clean run is accepted")
    check(out.get(tsv) == HEADER, f"a clean run leaves {tsv} with the header only")

    code, out, text = run(FIRST + RETRY_OK, [], 0)
    check(code == 0, "a run that passes on retry is accepted (the retry policy stays)")
    check(out.get(tsv) == HEADER + "beta-qml\tFailed\tPassed\ngamma\tTimeout\tPassed\n",
          "both first-attempt failures are kept with their eventual outcome")
    check("beta-qml\tFailed\tPassed" in text, "the first-attempt failures are printed in the job log")

    code, out, _ = run(FIRST + ["      Test  #2: beta-qml .........................***Failed   60.37 sec", RETRY_OK[1]], ["beta-qml"], 8)
    check(code != 0, "a test failing both attempts is refused")
    check("beta-qml\tFailed\tFailed\n" in out.get(tsv, ""), "a final failure is recorded as such")

    code, _, _ = run(CLEAN, [], 0, registered=4)
    check(code != 0, "a registered test that never ran is refused")
    code, _, _ = run(CLEAN, [], 8)
    check(code != 0, "a nonzero ctest exit is refused despite a passing summary")
    code, _, _ = run([], [], 0, registered=0, total=1)
    check(code != 0, "a run with nothing registered is refused")

if errors:
    print(f"CI test-gate tests FAILED ({len(errors)})", file=sys.stderr)
    sys.exit(1)
print("CI test-gate tests passed")
