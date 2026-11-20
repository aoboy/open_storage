#!/usr/bin/env python3
"""Compare two C declaration surfaces for the Rust seam (plan W6.5).

The gate is the ABI, not the formatting: comments, wrapping, tag names and
parameter identifiers are irrelevant. This compares, in order:

  * functions: return type + name + parameter TYPES (names stripped)
  * typedefs:  every public typedef name exists on both sides
  * structs:   field types and order for the value structs
  * enums:     constant names and values

Usage:
    check_rust_seam.py <c-header> <generated-header>
    check_rust_seam.py --selftest

Exit 0 when the declaration surfaces agree, 1 with a unified diff otherwise.
"""

from __future__ import annotations

import difflib
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Surface:
    functions: dict[str, str] = field(default_factory=dict)
    typedefs: set[str] = field(default_factory=set)
    structs: dict[str, list[str]] = field(default_factory=dict)
    enums: dict[str, list[str]] = field(default_factory=dict)
    fnptrs: dict[str, str] = field(default_factory=dict)

    def canonical(self) -> list[str]:
        lines: list[str] = []
        for name in sorted(self.functions):
            lines.append(f"fn {name}: {self.functions[name]}")
        for name in sorted(self.typedefs):
            lines.append(f"typedef {name}")
        for name in sorted(self.structs):
            lines.append(f"struct {name} {{ {'; '.join(self.structs[name])} }}")
        for name in sorted(self.enums):
            lines.append(f"enum {name} {{ {'; '.join(self.enums[name])} }}")
        for name in sorted(self.fnptrs):
            lines.append(f"fnptr {name}: {self.fnptrs[name]}")
        return lines


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", " ", text)


def strip_wrappers(text: str) -> str:
    """Drop preprocessor lines and the extern "C" wrapper's outer braces."""
    lines = [ln for ln in text.splitlines() if not ln.lstrip().startswith("#")]
    joined = "\n".join(lines)
    had_extern_c = re.search(r'extern\s+"C"\s*\{', joined) is not None
    joined = re.sub(r'extern\s+"C"\s*\{', "", joined)
    if had_extern_c:
        # remove the wrapper's closing brace (the last bare brace line)
        matches = list(re.finditer(r"^\s*\}\s*$", joined, flags=re.MULTILINE))
        if matches:
            last = matches[-1]
            joined = joined[: last.start()] + joined[last.end() :]
    return joined


def split_decls(text: str) -> list[str]:
    """Split on semicolons at brace depth zero (struct bodies stay whole)."""
    decls: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in text:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        cur.append(ch)
        if ch == ";" and depth == 0:
            decl = "".join(cur).strip()
            if decl:
                decls.append(decl)
            cur = []
    return decls


def norm_type(t: str) -> str:
    t = re.sub(r"\s+", " ", t.strip())
    t = re.sub(r"\s*\*\s*", " *", t)
    t = re.sub(r"\bstruct\s+", "", t)
    # cbindgen spelling that is ABI-identical to the C header's
    t = t.replace("uintptr_t", "size_t")
    t = t.replace("int32_t", "int")
    return t.strip()


def split_top_level(args: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    cur: list[str] = []
    for ch in args:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return parts


def param_type(param: str) -> str:
    p = norm_type(param)
    if p in ("void", ""):
        return "void"
    # remove a trailing parameter identifier; pointer declarators glue the
    # name to the star ("void *ctx"), value types keep a space ("size_t len")
    p = re.sub(r"\*\s*[A-Za-z_]\w*$", "*", p)
    p = re.sub(r"\s+[A-Za-z_]\w*$", "", p)
    return p.strip()


def parse_fields(body: str) -> list[str]:
    fields: list[str] = []
    for raw in body.split(";"):
        raw = norm_type(raw)
        if not raw:
            continue
        # "double p50, p95, p99;" shares one base type across declarators
        base = ""
        for part in split_top_level(raw):
            part = part.strip()
            if not part:
                continue
            m = re.match(r"^(.*?)\s*([A-Za-z_]\w*)$", part)
            if not m:
                fields.append(part)
                continue
            if m.group(1).strip():
                base = m.group(1).strip()
            if base:
                fields.append(f"{base} {m.group(2)}")
            else:
                fields.append(part)
    return fields


def parse(text: str) -> Surface:
    text = strip_wrappers(strip_comments(text))
    surface = Surface()
    for decl in split_decls(text):
        d = re.sub(r"\s+", " ", decl).strip().rstrip(";").strip()
        if not d:
            continue

        m = re.match(
            r"^typedef\s+struct(?:\s+([A-Za-z_]\w*))?\s*\{(.*)\}\s*([A-Za-z_]\w*)$",
            d,
            flags=re.DOTALL,
        )
        if m:
            name = m.group(3)
            fields = parse_fields(m.group(2))
            # cbindgen's zero-length body is the opaque-handle device; the
            # ABI sees only the alias, never the placeholder field.
            if len(fields) == 1 and "_private[0]" in fields[0]:
                continue
            surface.typedefs.add(name)
            surface.structs[name] = fields
            continue

        m = re.match(
            r"^typedef\s+struct\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)$", d
        )
        if m:
            # opaque handle: typedef struct tag alias;
            surface.typedefs.add(m.group(2))
            continue

        m = re.match(r"^typedef\s+enum(?:\s+[A-Za-z_]\w*)?\s*\{(.*)\}\s*([A-Za-z_]\w*)$", d, flags=re.DOTALL)
        if m:
            name = m.group(2)
            surface.typedefs.add(name)
            consts = []
            for item in m.group(1).split(","):
                item = item.strip()
                if item:
                    consts.append(re.sub(r"\s*=\s*", "=", item))
            surface.enums[name] = consts
            continue

        # function-pointer typedef: typedef int (*cb)(void *, size_t);
        m = re.match(
            r"^typedef\s+(.+?)\s*\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*\((.*)\)$",
            d,
            flags=re.DOTALL,
        )
        if m:
            ret = norm_type(m.group(1))
            name = m.group(2)
            args = ", ".join(param_type(p) for p in split_top_level(m.group(3)))
            surface.typedefs.add(name)
            surface.fnptrs[name] = f"{ret} ({args})"
            continue

        d = re.sub(r"^extern\s+", "", d)
        m = re.match(r"^(.+?)\s+([A-Za-z_]\w*)\s*\((.*)\)$", d, flags=re.DOTALL)
        if m:
            ret = norm_type(m.group(1))
            name = m.group(2)
            args = ", ".join(param_type(p) for p in split_top_level(m.group(3)))
            surface.functions[name] = f"{ret} ({args})"
            continue

        # a bare typedef alias (not used by the current header, kept honest)
        m = re.match(r"^typedef\s+(.+?)\s+([A-Za-z_]\w*)$", d)
        if m:
            surface.typedefs.add(m.group(2))
            continue

        raise SystemExit(f"check_rust_seam: unparsed declaration: {d[:120]}")

    return surface


def diff_surfaces(
    a: Surface,
    b: Surface,
    fromfile: str = "include/sdf/fountain.h",
    tofile: str = "cbindgen-generated header",
) -> list[str]:
    return list(
        difflib.unified_diff(
            a.canonical(),
            b.canonical(),
            fromfile=fromfile,
            tofile=tofile,
            lineterm="",
        )
    )


def selftest() -> int:
    c_header = """
        #ifdef __cplusplus
        extern "C" {
        #endif
        typedef enum {
            SDF_FOUNTAIN_LT = 0,
            SDF_FOUNTAIN_NANORQ = 1,
        } sdf_fountain_codec_t;
        typedef struct {
            size_t block_len;
            uint32_t k;
        } fe_cfg_t;
        typedef struct fountain_enc fountain_enc_t;
        int fe_init_in(fountain_enc_t *e, const fe_cfg_t *cfg, const uint8_t *block);
        bool fd_ready(const fountain_dec_t *d);
        #ifdef __cplusplus
        }
        #endif
    """
    generated = """
        typedef enum {
          SDF_FOUNTAIN_LT = 0,
          SDF_FOUNTAIN_NANORQ = 1,
        } sdf_fountain_codec_t;
        typedef struct {
          size_t block_len;
          uint32_t k;
        } fe_cfg_t;
        typedef struct {
          uint8_t _private[0];
        } fountain_enc;
        typedef fountain_enc fountain_enc_t;
        extern int fe_init_in(fountain_enc_t *e,
                              const fe_cfg_t *cfg,
                              const uint8_t *block);
        extern bool fd_ready(const fountain_dec_t *d);
    """
    a = parse(c_header)
    b = parse(generated)
    if diff_surfaces(a, b):
        print("selftest FAIL: equivalent surfaces reported a diff", file=sys.stderr)
        return 1

    mutated = generated.replace("uint32_t k;", "uint16_t k;")
    if not diff_surfaces(a, parse(mutated)):
        print("selftest FAIL: a field-width change went undetected", file=sys.stderr)
        return 1

    broken = generated.replace(
        "extern int fe_init_in(fountain_enc_t *e,", "extern int fe_init_in(void *e,"
    )
    if not diff_surfaces(a, parse(broken)):
        print("selftest FAIL: an opaque-type loss went undetected", file=sys.stderr)
        return 1

    print("check_rust_seam: selftest ok (equivalent passes; field/type changes fail)")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) == 2 and argv[1] == "--selftest":
        return selftest()
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    a = parse(Path(argv[1]).read_text(encoding="utf-8"))
    b = parse(Path(argv[2]).read_text(encoding="utf-8"))
    diff = diff_surfaces(a, b, argv[1], argv[2])
    if diff:
        print("\n".join(diff), file=sys.stderr)
        print(
            "check_rust_seam: HEADER DIFF — the Rust view diverged. "
            "Intentional? Ride the ADR and bump golden_vectors (ADR-0001).",
            file=sys.stderr,
        )
        return 1
    print(
        "check_rust_seam: OK "
        f"({len(a.functions)} functions, {len(a.typedefs)} typedefs, "
        f"{len(a.structs)} structs, {len(a.enums)} enums, "
        f"{len(a.fnptrs)} function pointers identical)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
