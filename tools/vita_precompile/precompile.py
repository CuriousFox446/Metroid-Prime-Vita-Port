#!/usr/bin/env python3
"""PS Vita: the generated programs compiled on the PC with the Vita's own Cg compiler.

The compiler is Sony's SceShaccCg, the module vitashark loads on the Vita (ur0:data/libshacccg.suprx,
which the user extracts from their own console): open-shacccg's oracle (MIT, tools/oracle in
https://github.com/frangarcj/open-shacccg) runs it here under Unicorn's ARM emulation. Each program
is compiled as vitashark compiles it for vita_gl.cpp's compile_gxp (the source named "<built-in>",
entry "main", useFx, optimisation 3, fast math and integers, full precision, warnings off), so the
binaries are the ones the Vita would make. The module is never copied into this tree.

  # once: the module as an ELF, and the oracle's import and trap maps for it
  precompile.py prepare --module libshacccg.suprx --oracle ~/open-shacccg --work ~/shacc/work

  # the sources (precompile_sources: build.sh) compiled into the pack the Vita reads
  precompile.py compile --oracle ~/open-shacccg --work ~/shacc/work --sources <dir> --out precompiled.bin

The pack (vita_gl.cpp, read_pack_index): "VPK1", the count, then per program sorted by key
(u64 key, u32 vertex offset, u32 vertex size, u32 fragment offset, u32 fragment size), then the
GXPs. It goes in ux0:data/MetroidPrime/vita_programs/ beside the card's program binaries.
"""
from __future__ import annotations

import argparse
import multiprocessing
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

MAGIC = b"VPK1"


def load_oracle(oracle_dir: Path):
    sys.path.insert(0, str(oracle_dir / "tools" / "oracle"))
    import unicorn_oracle  # noqa: E402

    # The oracle's heap never reuses what it frees: room for a program's two compiles.
    unicorn_oracle.HEAP_SIZE = 0x0E000000
    return unicorn_oracle


def prepare(ns) -> int:
    work = ns.work
    work.mkdir(parents=True, exist_ok=True)
    elf = work / "libshacccg.elf"
    data = ns.module.read_bytes()
    if data[:4] == b"\x7fELF":
        elf.write_bytes(data)
    elif data[:4] == b"SCE\0":
        # A fake-signed SELF: its ELF follows the header (vita-unmake-fself's own unwrapping).
        tool = shutil.which("vita-unmake-fself")
        if tool is None:
            print("the module is a SELF: put VitaSDK's bin in PATH (vita-unmake-fself)", file=sys.stderr)
            return 1
        subprocess.run([tool, str(ns.module), str(elf)], check=True)
    else:
        print("the module is neither an ELF nor a SELF", file=sys.stderr)
        return 1
    tools = ns.oracle / "tools" / "oracle"
    imports = work / "imports.json"
    with imports.open("w") as out:
        subprocess.run([sys.executable, str(tools / "vita_imports.py"), str(elf), "--json"], check=True, stdout=out)
    subprocess.run([sys.executable, str(tools / "patch_import_stubs.py"), str(elf), str(imports),
                    str(work / "libshacccg.oracle.elf"), str(work / "trap_map.json")], check=True)
    oracle = load_oracle(ns.oracle).Oracle(elf, imports, work / "trap_map.json")
    print(f"prepared: {oracle.get_version()}")
    return 0


def compile_shader(oracle, um, source: str, fragment: bool) -> tuple[bytes, list[str]]:
    if oracle.install_allocator() != 0:
        raise RuntimeError("sceShaccCgSetDefaultAllocator failed")
    name = oracle.put_cstr("<built-in>")
    encoded = source.encode()
    text = oracle.put_bytes(encoded + b"\0", 1)
    oracle.source_file_ptr = oracle.heap.malloc(12)
    oracle.write(oracle.source_file_ptr, um.p32(name) + um.p32(text) + um.p32(len(encoded)))
    options = oracle.heap.malloc(0x68)
    oracle.write(options, b"\0" * 0x68)
    oracle.call_export(0x3B58AFA0, options)  # sceShaccCgInitializeCompileOptions
    # vitashark's shark_compile_shader_extended(SHARK_OPT_FAST, fastmath, no fastprecision, fastint).
    for offset, value in ((0x00, name), (0x04, 1 if fragment else 0), (0x08, oracle.put_cstr("main")),
                          (0x18, 0), (0x2C, 0), (0x30, 1), (0x38, 3), (0x3C, 1), (0x40, 0), (0x44, 1),
                          (0x50, 0), (0x54, 0), (0x58, 0)):
        oracle.write_u32(options + offset, value)
    callbacks = oracle.heap.malloc(0x18)
    oracle.write(callbacks, b"\0" * 0x18)
    oracle.call_export(0xA8C2C1C8, callbacks, 1)  # sceShaccCgInitializeCallbackList, trivial
    oracle.write_u32(callbacks + 0x00, oracle.open_file_cb)
    output = oracle.call_export(0x66814F35, options, callbacks, 0, max_insn=20_000_000_000)
    if not output:
        return b"", ["CompileProgram returned nothing"]
    program, size = oracle.read_u32(output), oracle.read_u32(output + 4)
    count, diags = oracle.read_u32(output + 8), oracle.read_u32(output + 12)
    gxp = oracle.read(program, size) if program and size else b""
    messages = []
    for i in range(count):
        message = oracle.read_u32(diags + 16 * i + 12)
        messages.append(oracle.read_cstr(message) if message else "")
    oracle.call_export(0xAA82EF0C, output)  # sceShaccCgDestroyCompileOutput
    return gxp, messages


def compile_program(task):
    oracle_dir, work, sources, key = task
    um = load_oracle(oracle_dir)
    oracle = um.Oracle(work / "libshacccg.elf", work / "imports.json", work / "trap_map.json")
    out = {"key": key}
    for stage, fragment in (("vertex", False), ("fragment", True)):
        source = (sources / f"{key}.{'fcg' if fragment else 'vcg'}").read_text()
        gxp, messages = compile_shader(oracle, um, source, fragment)
        if gxp[:4] != b"GXP\0":
            out["error"] = f"the {stage} program: " + "; ".join(messages[:3])
            return out
        out[stage] = gxp
    return out


def compile_pack(ns) -> int:
    keys = [line.split()[0] for line in (ns.sources / "manifest.txt").read_text().splitlines() if line.strip()]
    tasks = [(ns.oracle, ns.work, ns.sources, key) for key in keys]
    built: dict[int, tuple[bytes, bytes]] = {}
    failed = 0
    start = time.time()
    with multiprocessing.Pool(ns.jobs, maxtasksperchild=1) as pool:
        for n, result in enumerate(pool.imap_unordered(compile_program, tasks), 1):
            if "error" in result:
                failed += 1
                print(f"{result['key']}: {result['error']}", file=sys.stderr)
            else:
                built[int(result["key"], 16)] = (result["vertex"], result["fragment"])
            if n % 50 == 0 or n == len(tasks):
                print(f"{n}/{len(tasks)} ({failed} failed), {time.time() - start:.0f} s", flush=True)
    order = sorted(built)
    header = MAGIC + struct.pack("<I", len(order))
    table_size = 24 * len(order)
    offset = len(header) + table_size
    table, blobs = bytearray(), bytearray()
    for key in order:
        vertex, fragment = built[key]
        table += struct.pack("<QIIII", key, offset + len(blobs), len(vertex),
                             offset + len(blobs) + len(vertex), len(fragment))
        blobs += vertex + fragment
    ns.out.write_bytes(header + table + blobs)
    print(f"{ns.out}: {len(order)} programs, {failed} failed, {(len(header) + len(table) + len(blobs)) / 1024:.0f} KiB")
    return 0 if not failed else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--module", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--work", type=Path, required=True)
    c = sub.add_parser("compile")
    c.add_argument("--oracle", type=Path, required=True)
    c.add_argument("--work", type=Path, required=True)
    c.add_argument("--sources", type=Path, required=True)
    c.add_argument("--out", type=Path, required=True)
    c.add_argument("--jobs", type=int, default=multiprocessing.cpu_count())
    ns = ap.parse_args()
    return prepare(ns) if ns.command == "prepare" else compile_pack(ns)


if __name__ == "__main__":
    raise SystemExit(main())
