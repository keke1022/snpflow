#!/usr/bin/env python3
"""Run SNPFlow regressions against a built DPDK tree (no NIC/root required).

Reuse Meson's compiler/linker commands so these tests exercise the shipped
DPDK code and its real mbufs, mempools and forwarding engines. PMD tests
instrument the validation boundary only; the burst/completion functions are
included from the supplied source, with exported symbols namespaced to avoid
collisions with the normally linked driver. No production test hooks exist.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured and compiled DPDK build directory")
    parser.add_argument("--only", choices=["rx", "tx", "bridge"])
    parser.add_argument("--sanitize", action="store_true", help="instrument test translation units with ASan/UBSan")
    parser.add_argument("--pmd-source", type=Path,
                        help="alternate DPDK source for a pre-fix RX/TX regression comparison")
    args = parser.parse_args()
    if args.pmd_source and args.only not in ("rx", "tx"):
        parser.error("--pmd-source requires --only rx or --only tx")
    build = args.build.resolve()
    suffix = "-asan" if args.sanitize else ""
    if args.pmd_source:
        suffix += "-alternate"
    output = build / ("snpflow-tests" + suffix)
    output.mkdir(exist_ok=True)
    database = json.loads((build / "compile_commands.json").read_text())

    def run(cmd, **kwargs):
        return subprocess.run(cmd, cwd=build, check=True, **kwargs)

    def entry(suffix):
        return next(e for e in database if e["file"].endswith(suffix))

    def compile_test(template, source, target, extra=()):
        command = shlex.split(template["command"])
        kept = []
        skip = False
        for word in command:
            if skip:
                skip = False
            elif word in ("-o", "-MF", "-MQ", "-MT"):
                skip = True
            elif word not in ("-c", "-MD", "-MMD", template["file"]):
                kept.append(word)
        if args.sanitize:
            kept += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        # Preserve compiler wrappers such as "ccache cc" while placing source
        # override include paths before Meson's normal source include paths.
        first_option = next(i for i, word in enumerate(kept) if word.startswith("-"))
        run(kept[:first_option] + list(extra) + kept[first_option:] +
            ["-c", str(source), "-o", str(target)])

    commands = run(["ninja", "-t", "commands", "app/dpdk-testpmd"],
                   capture_output=True, text=True).stdout.splitlines()
    link = next(shlex.split(c) for c in reversed(commands)
                if " -o app/dpdk-testpmd " in c)
    cpus = sorted(os.sched_getaffinity(0))
    for kind in ([args.only] if args.only else ["rx", "tx", "bridge"]):
        print(f"Building/running {kind} production-code regression", flush=True)
        obj = output / f"test_{kind}.o"
        binary = output / f"test_{kind}"
        if kind in ("rx", "tx"):
            template = entry(f"/mlx5_{kind}.c")
            old_obj = shlex.split(template["command"])
            old_obj = old_obj[old_obj.index("-o") + 1]
            symbols = run(["nm", "-g", "--defined-only", old_obj],
                          capture_output=True, text=True).stdout.splitlines()
            defines = [f"-D{line.split()[-1]}=snpflow_test_{line.split()[-1]}"
                       for line in symbols if len(line.split()) == 3]
            if args.pmd_source:
                defines.insert(0, "-I" + str(args.pmd_source.resolve() / "drivers/net/mlx5"))
            compile_test(template, ROOT / f"tests/dpdk/test_{kind}_snapshot.c", obj, defines)
            test_link = [w for w in link if not w.startswith("app/dpdk-testpmd.p/")]
        else:
            template = entry("/testpmd.c")
            compile_test(template, ROOT / "tests/dpdk/test_bridge.c", obj,
                         ["-I" + str((build / template["file"]).resolve().parents[2] / "drivers/net/ring")])
            original = shlex.split(template["command"])
            original = original[original.index("-o") + 1]
            renamed = output / "testpmd-main.o"
            run(["objcopy", "--redefine-sym", "main=snpflow_original_testpmd_main", original, str(renamed)])
            test_link = [str(renamed) if w == original else w for w in link]
            if args.sanitize:
                # Instrument the changed bridge and its generating paths too,
                # rather than only the fixture linked to ordinary app objects.
                for name in ("cvm_mbuf_bridge.c", "txonly.c", "flowgen.c", "csumonly.c",
                             "cvm_chunk_fwd.c", "cvm_chunk_crypto.c", "cvm_chunk_packet.c",
                             "cvm_chunk_receiver.c", "cvm_chunk_wire.c"):
                    matches = [e for e in database if e["file"].endswith("/" + name)]
                    if not matches:
                        continue
                    unit = matches[0]
                    instrumented = output / (name + ".o")
                    compile_test(unit, (build / unit["file"]).resolve(), instrumented)
                    command = shlex.split(unit["command"])
                    old = command[command.index("-o") + 1]
                    test_link = [str(instrumented) if w == old else w for w in test_link]
        test_link[test_link.index("-o") + 1] = str(binary)
        test_link.insert(test_link.index("-o"), str(obj))
        if args.sanitize:
            test_link.insert(test_link.index("-o"), "-fsanitize=address,undefined")
        run(test_link)
        env = os.environ.copy()
        if args.sanitize:
            env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        run([str(binary), "-l", str(cpus[0]), "-m", "256", "--no-huge", "--no-pci",
             "--no-shconf", "--no-telemetry", "--log-level=*:warning"], timeout=90, env=env)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"FAILED: {error.cmd[0]} exited with {error.returncode}", flush=True)
        raise SystemExit(1)
