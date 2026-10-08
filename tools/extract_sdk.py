"""Writes sdk/stellaris_sdk.hpp: the part of the generated Stellaris SDK header that guidll uses.

The full header (about 7000 lines: every serializer offset, every command spec) is produced from the installed stellaris.exe by tools/sdk_dumper in the
Stellaris MCP repository (https://github.com/Yidhar/stellaris-mcp). guidll only needs a few dozen engine functions, globals, runtime offsets and one
command spec, so this script copies those and nothing else.

    python tools/extract_sdk.py <full stellaris_sdk.hpp> [-o sdk/stellaris_sdk.hpp]

Which symbols are needed is read from the sources: every `sdk::<namespace>::<name>` and `sdk::k<Name>` in src/. A symbol missing from the full header is an
error. After a game patch: run the dumper, run this script on the new header, rebuild (the static_asserts in src/imgui_host.cpp stop the build when
the engine's Dear ImGui no longer has the layout this DLL is built against).
"""
import argparse
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCAN_DIRS = [os.path.join(ROOT, "src")]
NAMESPACES = ("glob", "db", "rt", "vt", "fn")


def used_symbols():
    used = {ns: set() for ns in NAMESPACES}
    cmds, top, ent = set(), set(), {}  # ent: sdk::ent::<Class>::<field>
    pat = re.compile(r"\bsdk::(\w+)(?:::(\w+))?")
    pat_ent = re.compile(r"\bsdk::ent::(\w+)::(\w+)")
    for base in SCAN_DIRS:
        for dirpath, _, files in os.walk(base):
            for name in files:
                if not name.endswith((".cpp", ".hpp", ".h")):
                    continue
                with open(os.path.join(dirpath, name), encoding="utf-8", errors="replace") as f:
                    text = f.read()
                for m in pat_ent.finditer(text):
                    ent.setdefault(m.group(1), set()).add(m.group(2))
                for m in pat.finditer(text):
                    first, second = m.group(1), m.group(2)
                    if first == "cmd" and second:
                        cmds.add(second)
                    elif second is None:
                        top.add(first)
                    elif first in used:
                        used[first].add(second)
    return used, cmds, top, ent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("full_header")
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "sdk", "stellaris_sdk.hpp"))
    args = ap.parse_args()

    with open(args.full_header, encoding="utf-8") as f:
        lines = f.read().splitlines()
    used, cmds, top, ent_used = used_symbols()

    # preamble: the exe timestamp, the engine allocator, and the command spec struct
    preamble = [l for l in lines if re.match(r"inline constexpr \w+ (kExeTimestamp|kRvaEngineAlloc) =", l)]
    if len(preamble) != 2:
        print("kExeTimestamp / kRvaEngineAlloc not found in the full header", file=sys.stderr)
        return 1
    start = next(i for i, l in enumerate(lines) if l.startswith("struct CmdSpec {"))
    end = next(i for i in range(start, len(lines)) if lines[i] == "};")
    preamble += [""] + lines[start:end + 1]
    first_ns = next(i for i, l in enumerate(lines) if re.match(r"namespace (glob|db|rt|vt|fn) \{", l))

    out = [
        "// SUBSET of the generated Stellaris SDK header, written by tools/extract_sdk.py. DO NOT EDIT.",
        "// The full header is produced from the installed stellaris.exe by tools/sdk_dumper (Stellaris MCP",
        "// repository); this file keeps only what src/ uses. Values are RVAs / offsets for the stellaris.exe",
        "// whose PE TimeDateStamp is sdk::kExeTimestamp.",
        "#pragma once",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace sdk {",
    ] + preamble + [""]

    ident = re.compile(r"^\s*inline constexpr\s+[\w:<> ]+?\s+(\w+)\s*=")
    found = {ns: set() for ns in NAMESPACES}
    body = {n: [] for n in NAMESPACES}
    ns = None
    for l in lines[first_ns:]:
        m = re.match(r"namespace (\w+) \{", l)
        if m and m.group(1) in NAMESPACES:
            ns = m.group(1)
            continue
        if ns and l.startswith("}  // namespace " + ns):
            ns = None
            continue
        if ns:
            m = ident.match(l)
            if m and m.group(1) in used[ns]:
                body[ns].append(l)
                found[ns].add(m.group(1))
    missing = [f"sdk::{n}::{s}" for n in NAMESPACES for s in sorted(used[n] - found[n])]
    for n in NAMESPACES:
        if body[n]:
            out += [f"namespace {n} {{"] + body[n] + [f"}}  // namespace {n}", ""]

    # commands: whole `namespace <name> { ... }` blocks inside `namespace cmd`
    cmd_body, in_cmd, cur, keep = {}, False, None, False
    for l in lines:
        if l.startswith("namespace cmd {"):
            in_cmd = True
            continue
        if in_cmd and l.startswith("}  // namespace cmd"):
            break
        if not in_cmd:
            continue
        m = re.match(r"namespace (\w+) \{", l)
        if m:
            cur, keep = m.group(1), m.group(1) in cmds
            if keep:
                cmd_body[cur] = [l]
            continue
        if keep and cur:
            cmd_body[cur].append(l)
            if l.startswith("}"):
                keep = False
    missing += [f"sdk::cmd::{c}" for c in sorted(cmds - set(cmd_body))]
    if cmd_body:
        out.append("namespace cmd {")
        for c in sorted(cmd_body):
            out += cmd_body[c]
        out += ["}  // namespace cmd", ""]

    # entity fields: namespace ent { namespace <Class> { ... } }
    ent_found = {c: set() for c in ent_used}
    ent_body = {c: [] for c in ent_used}
    in_ent, cls = False, None
    for l in lines:
        if l.startswith("namespace ent {"):
            in_ent = True
            continue
        if in_ent and l.startswith("}  // namespace ent"):
            break
        if not in_ent:
            continue
        m = re.match(r"namespace (\w+) \{", l)
        if m:
            cls = m.group(1) if m.group(1) in ent_used else None
            continue
        if l.startswith("}"):
            cls = None
            continue
        if cls:
            m = ident.match(l)
            if m and m.group(1) in ent_used[cls]:
                ent_body[cls].append(l)
                ent_found[cls].add(m.group(1))
    missing += [f"sdk::ent::{c}::{s}" for c in ent_used for s in sorted(ent_used[c] - ent_found[c])]
    if ent_used:
        out.append("namespace ent {")
        for c in sorted(ent_used):
            out += [f"namespace {c} {{"] + ent_body[c] + ["}"]
        out += ["}  // namespace ent", ""]

    out += ["}  // namespace sdk", ""]
    # top-level names must be in the preamble
    text = "\n".join(preamble)
    missing += [f"sdk::{t}" for t in sorted(top) if t not in text and t not in NAMESPACES and t not in ("cmd", "ent")]
    if missing:
        print("missing from the full header:", *missing, sep="\n  ", file=sys.stderr)
        return 1
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    n_syms = sum(len(v) for v in found.values()) + len(cmd_body) + sum(len(v) for v in ent_found.values())
    print(f"wrote {args.out}: {n_syms} symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
