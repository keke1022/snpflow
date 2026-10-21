#!/usr/bin/env python3
"""Check BPF registration and missing-scheduler behavior in the CI build."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


BPF_TESTS = tuple(f"bpf_{name}_autotest" for name in (
    "no_instructions", "exit_only", "no_exit", "minimal_working", "add_one",
    "subtract_one", "jump_over_invalid_first", "jump_over_invalid_non_first",
    "stack_alignment", "stack_copy_uint128", "stack_sse2", "stack_memcpy",
))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    inventory = {t["name"]: t for t in
                 json.loads((build / "meson-info/intro-tests.json").read_text())}
    for name in BPF_TESTS:
        if name not in inventory or "--no-huge" not in inventory[name]["cmd"]:
            raise SystemExit(f"Missing no-huge Meson registration: {name}")
    config = (build / "rte_build_config.h").read_text()
    if re.search(r"^#define RTE_CRYPTO_SCHEDULER\b", config, re.M):
        raise SystemExit("This availability regression requires crypto/scheduler disabled")

    env = os.environ.copy()
    for key in ("DPDK_TEST", "DPDK_TEST_PARAMS", "DPDK_TEST_SKIP"):
        env.pop(key, None)
    cpu = str(min(os.sched_getaffinity(0)))
    subprocess.run([
        "meson", "test", "-C", str(build), "--no-rebuild", "--num-processes", "1",
        "--logbase=snpflow-registration-bpf",
        f"--test-args=-l {cpu} --no-pci --no-shconf --no-telemetry", *BPF_TESTS,
    ], env=env, check=True)
    results = [json.loads(line) for line in
               (build / "meson-logs/snpflow-registration-bpf.json").read_text().splitlines()]
    outcomes = {row["name"].rsplit(" / ", 1)[-1]: row["result"] for row in results}
    if set(outcomes) != set(BPF_TESTS) or any(value != "OK" for value in outcomes.values()):
        raise SystemExit(f"Expected all twelve BPF commands to pass, got: {outcomes}")

    logs = build / "snpflow-upstream-tests"
    logs.mkdir(exist_ok=True)
    command = [str(build / "app/dpdk-test"), "-l", cpu, "--no-huge", "-m", "256",
               "--no-pci", "--no-shconf", "--no-telemetry"]
    env["DPDK_TEST"] = "cryptodev_scheduler_autotest"
    with (logs / "registration-scheduler.log").open("w") as log:
        result = subprocess.run(command, env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=30)
    record = {"bpf_meson_tests": list(BPF_TESTS), "bpf_outcomes": outcomes,
              "scheduler_command": command,
              "scheduler_test": env["DPDK_TEST"], "scheduler_exit_status": result.returncode,
              "expected_scheduler_exit_status": 77,
              "scope": "Missing-driver reporting; scheduler functionality is not exercised"}
    (logs / "registration.json").write_text(json.dumps(record, indent=2) + "\n")
    if result.returncode != 77:
        raise SystemExit(f"Expected scheduler SKIP (77), got {result.returncode}")
    print("BPF tests passed; absent scheduler driver correctly reports SKIP (not a functional pass).")


if __name__ == "__main__":
    main()
