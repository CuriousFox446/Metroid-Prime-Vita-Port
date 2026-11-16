#!/usr/bin/env python3
"""PS Vita: the generated programs compiled on the PC with the Vita's own Cg compiler.

The compiler is Sony's SceShaccCg, the module vitashark loads on the Vita (ur0:data/libshacccg.suprx,
which the user extracts from their own console): open-shacccg's oracle (MIT, tools/oracle in
https://github.com/frangarcj/open-shacccg) runs it here under Unicorn's ARM emulation. Each program
is compiled as vitashark compiles it for vita_gl.cpp's compile_gxp (the source named "<built-in>",
entry "main", useFx, optimisation 3, fast math and integers, full precision, warnings off), so the
binaries are the ones the Vita would make. The module is never copied into this tree.

  # once: the module as an ELF (unwrapped from its SELF, its stack guard import resolved), and the
  # oracle's import and trap maps for it
  precompile.py prepare --module libshacccg.suprx --oracle ~/open-shacccg --work ~/shacc/work

  # the sources (precompile_sources: build.sh) compiled into the pack the Vita reads; each
  # program's GXPs are kept in <work>/gxp, so a second run compiles only what is new
  precompile.py compile --oracle ~/open-shacccg --work ~/shacc/work --sources <dir> --out precompiled.bin

Run it with Python 3.10 or later and Unicorn 2 (pip install unicorn). Checked with the module of
SDK 3.0.0 (shacc r13225): its GXPs were byte for byte the ones the Vita compiled.

The pack (vita_gl.cpp, read_pack_index): "VPK1", the count, then per program sorted by key
(u64 key, u32 vertex offset, u32 vertex size, u32 fragment offset, u32 fragment size), then the
GXPs. It goes in ux0:data/MetroidPrime/vita_programs/ beside the card's program binaries.
"""
from __future__ import annotations

import argparse
import json
import multiprocessing
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

MAGIC = b"VPK1"


def unwrap_self(data: bytes) -> bytes:
    """The ELF inside a fake-signed SELF (what ShaRKF00D writes): the ELF header and program headers
    the SELF keeps, and each segment where its program header puts it, inflated when compressed.
    What vita-unmake-fself does, which VitaSDK's builds lack."""
    elf_offset, phdr_offset = struct.unpack_from("<QQ", data, 0x40)
    section_info_offset = struct.unpack_from("<Q", data, 0x58)[0]
    ehdr = bytearray(data[elf_offset:elf_offset + 0x34])
    if ehdr[:4] != b"\x7fELF":
        raise ValueError("no ELF header in the SELF")
    phoff, = struct.unpack_from("<I", ehdr, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", ehdr, 0x2A)
    # No section headers in the output (the SELF does not keep them).
    struct.pack_into("<I", ehdr, 0x20, 0)
    struct.pack_into("<HHH", ehdr, 0x2E, 0, 0, 0)
    phdrs = data[phdr_offset:phdr_offset + phentsize * phnum]
    out = bytearray(max(phoff + len(phdrs), 0x34))
    out[:0x34] = ehdr
    out[phoff:phoff + len(phdrs)] = phdrs
    for i in range(phnum):
        p_offset, _, _, p_filesz = struct.unpack_from("<IIII", phdrs, i * phentsize + 4)
        offset, length, compression, encryption = struct.unpack_from("<QQQQ", data, section_info_offset + 32 * i)
        if encryption != 2:
            raise ValueError(f"segment {i} is encrypted: not a fake-signed SELF")
        segment = data[offset:offset + length]
        if compression == 2:
            segment = zlib.decompress(segment)
        segment = segment[:p_filesz]
        if len(out) < p_offset + len(segment):
            out.extend(b"\0" * (p_offset + len(segment) - len(out)))
        out[p_offset:p_offset + len(segment)] = segment
    return bytes(out)


def resolve_variables(elf: bytes, imports: dict, address: int) -> bytes:
    """The module's variable imports pointed at `address`, a word of zeros the oracle maps: the oracle
    resolves the functions a module imports, not its variables, and SceShaccCg reads one, SceLibKernel's
    __stack_chk_guard, in its functions' prologues (module_start's first read of it was of address 0).
    A variable's import entry points to the table of its references: its size in bytes (header
    included) shifted left 4, then 8 bytes a place, the relocation's code in bits 8-15 (a Thumb MOVW
    or MOVT, all of them here) and the segment in bits 4-7, and the place's offset in that segment."""
    data = bytearray(elf)
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    segments = [struct.unpack_from("<IIIII", data, phoff + phentsize * i) for i in range(phnum)]

    def file_offset(segment: int, offset: int, size: int) -> int:
        p_type, p_offset, _, _, p_filesz = segments[segment]
        if p_type != 1 or offset + size > p_filesz:  # PT_LOAD
            raise ValueError(f"segment {segment} + {offset:#x} is outside its file data")
        return p_offset + offset

    def segment_of(va: int) -> tuple[int, int]:
        for i, (p_type, _, p_vaddr, _, p_filesz) in enumerate(segments):
            if p_type == 1 and p_vaddr <= va < p_vaddr + p_filesz:
                return i, va - p_vaddr
        raise ValueError(f"{va:#x} is in no segment")

    patched = 0
    for lib in imports["imports"]:
        for entry in lib["variable_entries"]:
            table = file_offset(*segment_of(entry), 4)
            size = struct.unpack_from("<I", data, table)[0] >> 4
            for pos in range(4, size, 8):
                info, offset = struct.unpack_from("<II", data, table + pos)
                code, segment = (info >> 8) & 0xFF, (info >> 4) & 0xF
                at = file_offset(segment, offset, 4)
                if code == 2:  # R_ARM_ABS32
                    struct.pack_into("<I", data, at, address)
                elif code in (47, 48):  # R_ARM_THM_MOVW_ABS_NC, R_ARM_THM_MOVT_ABS
                    imm = (address if code == 47 else address >> 16) & 0xFFFF
                    first, second = struct.unpack_from("<HH", data, at)
                    first = (first & 0xFBF0) | (imm >> 12) | (((imm >> 11) & 1) << 10)
                    second = (second & 0x8F00) | (((imm >> 8) & 7) << 12) | (imm & 0xFF)
                    struct.pack_into("<HH", data, at, first, second)
                else:
                    raise ValueError(f"a variable's reference of relocation type {code} is not handled")
                patched += 1
    print(f"variable imports: {patched} references pointed at {address:#x}")
    return bytes(data)


def load_oracle(oracle_dir: Path):
    sys.path.insert(0, str(oracle_dir / "tools" / "oracle"))
    import unicorn_oracle  # noqa: E402

    if getattr(unicorn_oracle, "vita_precompile", False):
        return unicorn_oracle
    unicorn_oracle.vita_precompile = True
    # The oracle's heap never reuses what it frees: room for a program's two compiles.
    unicorn_oracle.HEAP_SIZE = 0x0E000000
    # Two SceLibKernel imports of the module the Vita has (SDK 3.0.0, shacc r13225) that the
    # oracle's (1.6.5) did not make (names from VitaSDK's NID database).
    unicorn_oracle.KNOWN_IMPORTS.setdefault(0x37691BF8, "__stack_chk_fail")
    unicorn_oracle.KNOWN_IMPORTS.setdefault(0xB295EB61, "sceKernelGetTLSAddr")
    dispatch = unicorn_oracle.Oracle._dispatch_import

    def dispatch_more(self, nid, name, a0, a1, a2, a3):
        if name == "sceKernelGetTLSAddr":
            # A slot of the thread's TLS, which TPIDRURO points to (the oracle's one thread's).
            self._ret(unicorn_oracle.TLS_BASE + 4 * a0 if 4 * a0 < unicorn_oracle.TLS_SIZE else 0)
            return
        if name == "__stack_chk_fail":
            raise RuntimeError("the guest's stack check failed")
        dispatch(self, nid, name, a0, a1, a2, a3)

    unicorn_oracle.Oracle._dispatch_import = dispatch_more
    return unicorn_oracle


def prepare(ns) -> int:
    work = ns.work
    work.mkdir(parents=True, exist_ok=True)
    # The GXPs compiled with the module prepared before (compile_pack).
    shutil.rmtree(work / "gxp", ignore_errors=True)
    elf = work / "libshacccg.elf"
    data = ns.module.read_bytes()
    # The SELF's segments come relocated for the addresses their program headers give, which is
    # where the oracle maps them.
    if data[:4] == b"\x7fELF":
        elf.write_bytes(data)
    elif data[:4] == b"SCE\0":
        elf.write_bytes(unwrap_self(data))
    else:
        print("the module is neither an ELF nor a SELF", file=sys.stderr)
        return 1
    tools = ns.oracle / "tools" / "oracle"
    imports = work / "imports.json"
    with imports.open("w") as out:
        subprocess.run([sys.executable, str(tools / "vita_imports.py"), str(elf), "--json"], check=True, stdout=out)
    um = load_oracle(ns.oracle)
    # Past the oracle's four callback trampolines, in their page.
    elf.write_bytes(resolve_variables(elf.read_bytes(), json.loads(imports.read_text()), um.TRAMP_BASE + 0x100))
    subprocess.run([sys.executable, str(tools / "patch_import_stubs.py"), str(elf), str(imports),
                    str(work / "libshacccg.oracle.elf"), str(work / "trap_map.json")], check=True)
    oracle = um.Oracle(elf, imports, work / "trap_map.json")
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
    # Each program's GXPs are kept as it is compiled (5 to 45 s each here), so a run cut short goes
    # on where it stopped, and a pack made again with more material records compiles only theirs. A
    # key is its sources' hash; prepare empties this for another module.
    cache = ns.work / "gxp"
    cache.mkdir(exist_ok=True)
    built: dict[int, tuple[bytes, bytes]] = {}
    todo = []
    for key in keys:
        vertex, fragment = cache / f"{key}.vgxp", cache / f"{key}.fgxp"
        if vertex.exists() and fragment.exists():
            built[int(key, 16)] = (vertex.read_bytes(), fragment.read_bytes())
        else:
            todo.append(key)
    print(f"{len(keys)} programs: {len(built)} compiled before, {len(todo)} to compile", flush=True)
    tasks = [(ns.oracle, ns.work, ns.sources, key) for key in todo]
    failed = 0
    start = time.time()
    with multiprocessing.Pool(ns.jobs, maxtasksperchild=1) as pool:
        for n, result in enumerate(pool.imap_unordered(compile_program, tasks), 1):
            if "error" in result:
                failed += 1
                print(f"{result['key']}: {result['error']}", file=sys.stderr)
            else:
                key = result["key"]
                # The fragment GXP last: both there means both whole.
                for stage, suffix in (("vertex", "vgxp"), ("fragment", "fgxp")):
                    partial = cache / f"{key}.{suffix}.part"
                    partial.write_bytes(result[stage])
                    partial.replace(cache / f"{key}.{suffix}")
                built[int(key, 16)] = (result["vertex"], result["fragment"])
            if n % 25 == 0 or n == len(tasks):
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
