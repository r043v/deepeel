#!/usr/bin/env python3
"""Reference coverage for a LADX-Disassembly build (information only, not a coverage source).

Build a ROM in the LADX-Disassembly repo (e.g. `make` for azle.gbc), keep the
.sym file, then:
    python3 tools/ladx_disasm_ref.py <ladx-repo> [--revision azle] \
        [--map out.map] [--rom azle.gbc] [--stats out.txt]
The per-ROM-byte classes come from the asm sources, walked in assembly order
with the .sym addresses as anchors: an instruction line is code, db/dw/dl/
incbin is data, ds is padding.  Labels resync the walk at every symbol (43k
anchors on azle), so length-model errors stay local to one label region;
line-start addresses from the trailing ";; bank:addr" comments give exact
spans where present.  A label region with no code at all is all data (dialog,
music, tables); unknown gaps inside code regions stay unknown, except between
two same-class neighbors.  Output: a deepeel .map text consumable by
    deepeel maprender <rom> ref.map ref.map.png
Observation of what the disassembly knows; not a hardware measure."""
import argparse, hashlib, re, sys
from collections import defaultdict
from pathlib import Path

# ---------------------------------------------------------------------------
# SM83 instruction lengths (RGBDS syntax).  The trailing ";; bank:addr bytes"
# comments are the primary length oracle; this model only fills the gaps.
R8 = {"a", "b", "c", "d", "e", "h", "l", "[hl]"}
CC = {"nz", "z", "nc", "c"}
CB2 = {"rlc", "rrc", "rl", "rr", "sla", "sra", "srl", "swap", "bit", "res", "set"}
ALU = {"adc", "add", "and", "cp", "or", "sbc", "sub", "xor"}
MNEMONICS = ALU | {"bit", "call", "ccf", "cpl", "daa", "dec", "di", "ei", "halt",
                   "inc", "jp", "jr", "ld", "ldd", "ldh", "ldi", "ldio", "nop",
                   "pop", "push", "res", "ret", "reti", "rl", "rla", "rlc",
                   "rlca", "rr", "rra", "rrc", "rrca", "rst", "sbc", "scf",
                   "set", "sla", "sra", "srl", "stop", "swap", "xor"} | CB2


def op_span(s):
    """Top-level comma split of an operand string, quotes/parens aware."""
    out, depth, q, cur = [], 0, False, ""
    i = 0
    while i < len(s):
        ch = s[i]
        if q:
            cur += ch
            if ch == "\\" and i + 1 < len(s):
                cur += s[i + 1]; i += 1
            elif ch == '"':
                q = False
        elif ch == '"':
            q = True; cur += ch
        elif ch in "([{":
            depth += 1; cur += ch
        elif ch in ")]}":
            depth -= 1; cur += ch
        elif ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


def norm(s):
    return re.sub(r"\s+", "", s).lower()


def instr_len(mn, rest):
    """Byte length of one instruction line, RGBDS syntax; None = unknown."""
    ops = op_span(rest) if rest else []
    n = [norm(o) for o in ops]
    if mn == "ld":
        if len(n) == 2:
            d, s = n
            if d == "hl" and s.startswith("sp+"):
                return 2                       # ld hl,sp+r8
            if d == "sp":
                return 1 if s == "hl" else 3   # ld sp,hl | ld sp,n16
            if d in ("bc", "de", "hl"):
                return 3                       # ld rr,n16
            if d in R8:
                if s in {"[bc]", "[de]", "[hl]", "[hli]", "[hld]", "[c]",
                         "[hl+]"} or re.fullmatch(r"\[\$ff00\+c\]", s):
                    return 1
                if re.fullmatch(r"\[[^]]*\]", s):
                    return 3                   # ld r8,[n16]
                return 2                       # ld r8,n8
            if d in {"[bc]", "[de]", "[hl]", "[hli]", "[hld]"}:
                return 1
            if re.fullmatch(r"\[\$ff00\+c\]", d):
                return 1
            if re.fullmatch(r"\[[^]]*\]", d):
                return 3                       # ld [n16],a / [n16],sp
        return None
    if mn in ("ldh", "ldio"):
        return 2 if len(n) == 2 else None
    if mn in ("ldd", "ldi"):
        return 1
    if mn == "jp":
        return 1 if (n and n[0] in ("hl", "[hl]")) else 3
    if mn == "jr":
        return 2
    if mn == "call":
        return 3
    if mn in ("ret", "reti", "rst", "push", "pop", "inc", "dec", "stop",
              "halt", "di", "ei", "nop", "daa", "cpl", "scf", "ccf",
              "rlca", "rrca", "rla", "rra"):
        return 1
    if mn in ALU:
        if len(n) == 2:
            if n[0] == "sp":
                return 2                       # add sp,e8
            if n[0] == "a" and n[1] not in R8:
                return 2                       # add a,n8
            return 1
        return 1
    if mn in CB2:
        return 2
    return None


# ---------------------------------------------------------------------------
class ExprError(Exception):
    pass


CLS_CODE, CLS_DATA, CLS_PAD = 1, 2, 3
CLS_NAME = {CLS_CODE: "code-static", CLS_DATA: "data-static",
            CLS_PAD: "padding"}
NUMTOK = r"(?:\$[0-9A-Fa-f]+|&[0-7]+|%[01]+|\d+)"
SYMTOK = r"[A-Za-z_@.][A-Za-z0-9_@.]*"


class Walker:
    TOK = re.compile(r'\s*("(?:[^"\\]|\\.)*"|\$[0-9A-Fa-f]+|&[0-7]+|%[01]+|'
                     r'\d+\.?\d*|#[A-Za-z_][A-Za-z0-9_.]*|' + SYMTOK +
                     r'|<<|>>|&&|\|\||<=|>=|===?|!==|[-+*/%~^&|!<>()\'",=])')
    ADDR_CMT = re.compile(r";;\s*([0-9A-Fa-f]{2}):([0-9A-Fa-f]{4})([^;]*)$")
    LABEL = re.compile(r"^([A-Za-z_.][A-Za-z0-9_.]*)::?\s*(.*)$")
    SEC = re.compile(r'^\s*section\s+(?:(union|fragment)\s+)?"([^"]*)"\s*,'
                     r'\s*([a-z0-9]+)\s*(.*)$', re.I)

    def __init__(self, root: Path, sym: dict, stats: dict, charmap: dict):
        self.root = root
        self.sym = sym
        self.stats = stats
        self.charmap = charmap
        self.vars = {}
        self.strings = {}
        self.macros = {}
        self.mac_stack = []          # while capturing a macro definition
        self.cond = []               # (branch_active, chain_taken) stack
        self.bank = 0
        self.addr = 0x0000           # CPU view: bank 0 = $0000-$3FFF
        self.rom = True              # False inside wram/vram/sram/hram
        self.cur_global = ""
        self.macro_uid = 0
        self.file_stack = []
        self.narg = []               # _NARG stack during macro expansion
        self.region_log = []         # every region start, walk order
        self.section_pending = []    # no-code regions closed by a section switch
        self.vec = bytearray(64 * 0x4000)
        self.rom_bytes = None        # set by main(): live stale-comment filter
        self.spans = []              # (idx, n, cls, expected_bytes|None)
        self.region_start = 0        # byte index where the current label region began
        self.region_code = False     # the region emitted any instruction span
        self.region_ok = False       # a label anchored the region start

    # ---- position ---------------------------------------------------------
    def pos(self):
        return (self.bank, self.addr)

    def byte_index(self):
        return self.bank * 0x4000 + (self.addr & 0x3FFF)

    def advance(self, n):
        self.addr += n
        if self.bank == 0:
            if self.addr > 0x3FFF:
                self.stats["bank0_overflow"] += 1
        elif self.addr > 0x7FFF:
            self.addr = 0x4000 + (self.addr - 0x8000)
            self.bank += 1

    def resync(self, name, where):
        cands = self.sym.get(name)
        if not cands:
            self.stats["label_no_sym"] += 1
            return False
        want = self.pos()
        if want in cands:
            self.close_region()
            self.region_start = self.byte_index()
            self.region_code = False
            self.region_ok = True
            self.region_log.append(self.region_start)
            self.stats["labels_matched"] += 1
            return True
        near = [c for c in cands if c[0] == want[0]]
        pick = near[0] if near else cands[0]
        self.stats["label_resync"] += 1
        if near and pick[0] == want[0]:
            self.stats["resync_same_bank"] += 1
        else:
            self.stats["resync_bank_jump"] += 1
        if len(self.stats["mismatch_samples"]) < 40:
            self.stats["mismatch_samples"].append(
                f"{where}: {name}: predicted {want[0]:02X}:{want[1]:04X} "
                f"sym {pick[0]:02X}:{pick[1]:04X}")
        # the label address bounds the previous region in both directions
        self.close_region(pick[0] * 0x4000 + (pick[1] & 0x3FFF))
        self.bank, self.addr = pick
        self.region_start = self.byte_index()
        self.region_code = False
        self.region_ok = True
        self.region_log.append(self.region_start)
        return True

    # ---- region handling (the homogeneous fallback) ------------------------
    def close_region(self, end_idx=None):
        """Fill still-unknown bytes of the current label region: a region with
        no code at all is all data; inside code regions, only same-class
        neighbor gaps fill.  end_idx bounds the region when the walk position
        never advanced (fully opaque regions)."""
        lo = self.region_start
        hi = self.byte_index() if end_idx is None else max(end_idx, lo)
        if hi <= lo:
            return
        if not self.region_ok:
            return
        if not self.region_code:
            self.fill(lo, hi, CLS_DATA, "region-nocode")
            return
        i = lo
        while i < hi:
            if self.vec[i]:
                i += 1
                continue
            j = i
            while j < hi and not self.vec[j]:
                j += 1
            left = self.vec[i - 1] if i > 0 else None
            right = self.vec[j] if j < hi else None
            if left == CLS_DATA and (right in (CLS_DATA, CLS_PAD, None)):
                self.fill(i, j, CLS_DATA, "gap-data")
            elif left == CLS_CODE and right == CLS_CODE:
                self.fill(i, j, CLS_CODE, "gap-code")
            i = j

    def fill(self, lo, hi, cls, why):
        for k in range(lo, hi):
            if not self.vec[k]:
                self.vec[k] = cls
                self.stats[f"fill_{why}"] = self.stats.get(f"fill_{why}", 0) + 1

    # ---- expression evaluator ---------------------------------------------
    def get(self, name):
        if name == "@":
            return self.addr
        if name == "_NARG" and self.narg:
            return self.narg[-1]
        if name in self.strings:
            return self.strings[name]
        if name in self.vars:
            return self.vars[name]
        raise ExprError(f"unknown symbol {name}")

    def eval(self, text, where):
        s = text.strip()
        toks, i = [], 0
        while i < len(s):
            m = self.TOK.match(s, i)
            if not m:
                raise ExprError(f"bad expr {s!r}")
            toks.append(m.group(1)); i = m.end()
        pos = [0]

        def peek():
            return toks[pos[0]] if pos[0] < len(toks) else None

        def eat(t=None):
            v = peek()
            if t is not None and v != t:
                raise ExprError(f"expected {t}, got {v} in {s!r}")
            pos[0] += 1
            return v

        def prim():
            v = peek()
            if v is None:
                raise ExprError(f"unexpected end in {s!r}")
            if v == "(":
                eat(); r = lor(); eat(")")
                return r
            if v[0] == '"':
                eat()
                return re.sub(r'\\(.)', r"\1", v[1:-1])
            if v.startswith("#") and len(v) > 1:
                eat()
                return self.get(v[1:])
            if v == "-":
                eat(); return -prim()
            if v == "+":
                eat(); return prim()
            if v == "~":
                eat(); return ~prim()
            if v == "!":
                eat(); return 0 if prim() else 1
            if re.fullmatch(NUMTOK, v):
                eat()
                return int(v[1:], 16) if v[0] == "$" else \
                    int(v[1:], 8) if v[0] == "&" else \
                    int(v[1:], 2) if v[0] == "%" else int(v)
            if v[0] == "'":
                eat()
                return ord(v[1])
            if v == "DEF":
                eat(); eat("("); nm = eat(); eat(")")
                return 1 if nm in self.vars or nm in self.strings else 0
            if v in ("BANK", "LOW", "HIGH", "STRLEN", "CHARLEN"):
                fn = v; eat(); eat("(")
                if fn in ("STRLEN", "CHARLEN"):
                    arg = eat()
                    eat(")")
                    if not isinstance(arg, str):
                        raise ExprError(f"{fn} of non-string")
                    return self.strlen(arg, fn == "CHARLEN")
                arg = lor()
                eat(")")
                if fn == "BANK":
                    if isinstance(arg, str):
                        nm = arg.lstrip("#")
                        if nm == "@":
                            return self.bank
                        cands = self.sym.get(nm)
                        return cands[0][0] if cands else 0
                    return self.bank
                if fn == "LOW":
                    return arg & 0xFF
                return (arg >> 8) & 0xFF
            if re.fullmatch(SYMTOK, v):
                eat()
                val = self.get(v)
                if isinstance(val, str):
                    try:               # EQUS of a number expands to it
                        return int(val, 0)
                    except ValueError:
                        return val
                return val
            raise ExprError(f"unexpected token {v!r} in {s!r}")

        def mul():
            r = prim()
            while peek() in ("*", "/", "%"):
                op = eat()
                rhs = prim()
                r = {"*": lambda: r * rhs,
                     "/": lambda: r // rhs if rhs else 0,
                     "%": lambda: r % rhs if rhs else 0}[op]()
            return r

        def add():
            r = mul()
            while peek() in ("+", "-"):
                op = eat()
                r = r + mul() if op == "+" else r - mul()
            return r

        def shift():
            r = add()
            while peek() in ("<<", ">>"):
                op = eat()
                r = r << add() if op == "<<" else r >> add()
            return r

        def andx():
            r = shift()
            while peek() == "&":
                eat(); r &= shift()
            return r

        def xorx():
            r = andx()
            while peek() == "^":
                eat(); r ^= andx()
            return r

        def orx():
            r = xorx()
            while peek() == "|":
                eat(); r |= xorx()
            return r

        def cmpx():
            r = orx()
            while peek() in ("==", "!=", "<", ">", "<=", ">=", "==="):
                op = eat()
                rhs = orx()
                # EQUS of a numeric literal expands to that number in context
                if isinstance(r, str) and isinstance(rhs, int):
                    try:
                        r = int(r, 0)
                    except ValueError:
                        pass
                if isinstance(rhs, str) and isinstance(r, int):
                    try:
                        rhs = int(rhs, 0)
                    except ValueError:
                        pass
                if isinstance(r, str) or isinstance(rhs, str):
                    a, b = str(r), str(rhs)
                    r = {"==": a == b, "!=": a != b, "===": a == b,
                         "<": a < b, ">": a > b, "<=": a <= b,
                         ">=": a >= b}[op]
                else:
                    r = {"==": r == rhs, "!=": r != rhs, "===": r == rhs,
                         "<": r < rhs, ">": r > rhs, "<=": r <= rhs,
                         ">=": r >= rhs}[op]
                r = int(bool(r))
            return r

        def notx():
            if peek() == "!":
                eat(); return 0 if notx() else 1
            return cmpx()

        def land():
            r = notx()
            while peek() == "&&":
                eat(); rhs = notx()      # no short-circuit: consume everything
                r = int(bool(r) and bool(rhs))
            return r

        def lor():
            r = land()
            while peek() == "||":
                eat(); rhs = land()
                r = int(bool(r) or bool(rhs))
            return r

        v = lor()
        if pos[0] != len(toks):
            raise ExprError(f"trailing tokens in {s!r}")
        return v

    def strlen(self, s, chars=False):
        total, i = 0, 0
        while i < len(s):
            for ln in range(min(8, len(s) - i), 0, -1):
                if s[i:i + ln] in self.charmap:
                    total += self.charmap[s[i:i + ln]]
                    i += ln
                    break
            else:
                total += 1
                i += 1
        return total

    # ---- conditionals ------------------------------------------------------
    def skipping(self):
        return any(not a for a, _ in self.cond)

    def outer_skipping(self, idx):
        return any(not a for a, _ in self.cond[:idx])

    def conditional(self, kw, rest, where):
        st = self.stats
        if kw == "if":
            if self.skipping():
                self.cond.append((False, False))
                return
            try:
                v = bool(self.eval(rest, where))
            except ExprError as e:
                v = False
                st["cond_unevaluable"] += 1
                self.stats.setdefault("cond_samples", []).append(
                    f"{where}: {rest.strip()[:60]!r}: {e}")
            self.cond.append((v, v))
        elif kw == "elif":
            if not self.cond or self.outer_skipping(len(self.cond) - 1):
                return
            active, taken = self.cond[-1]
            if taken:
                self.cond[-1] = (False, True)
                return
            try:
                v = bool(self.eval(rest, where))
            except ExprError:
                v = False
                st["cond_unevaluable"] += 1
            self.cond[-1] = (v, taken or v)
        elif kw == "else":
            if not self.cond or self.outer_skipping(len(self.cond) - 1):
                return
            active, taken = self.cond[-1]
            self.cond[-1] = (not taken, True)
        elif kw == "endc":
            if self.cond:
                self.cond.pop()

    # ---- files -------------------------------------------------------------
    def resolve(self, name):
        cands = []
        if self.file_stack:
            cands.append(self.file_stack[-1].parent / name)
        cands += [self.root / "src" / name, self.root / name, Path(name)]
        for p in cands:
            if p.exists():
                return p
        return None

    def include_file(self, path: Path, tag):
        self.file_stack.append(path)
        try:
            with open(path, errors="replace") as f:
                for no, ln in enumerate(f, 1):
                    self.line(ln, f"{tag}:{no}")
        finally:
            self.file_stack.pop()

    def include(self, name, where):
        p = self.resolve(name)
        if p is None:
            self.stats["include_missing"] += 1
            return
        self.stats["files"] += 1
        self.include_file(p, p.name)

    # ---- labels ------------------------------------------------------------
    def label(self, name, where):
        if self.skipping():
            return
        if name.startswith("."):
            full = f"{self.cur_global}{name}" if self.cur_global else name
        else:
            full = name
            self.cur_global = name
        if not self.rom:
            return
        self.resync(full, where)

    # ---- byte emission -----------------------------------------------------
    def anchor(self, cmt_addr, where):
        b, a = cmt_addr
        w = self.pos()
        if (b, a) != w:
            self.stats["anchor_drift"] += 1
            if len(self.stats["drift_samples"]) < 30:
                self.stats["drift_samples"].append(
                    f"{where}: comment {b:02X}:{a:04X} vs walk {w[0]:02X}:{w[1]:04X}")
            self.bank, self.addr = b, a

    def span(self, n, cls, where, cmt_addr=None, expect=None):
        st = self.stats
        if n <= 0:
            return
        if cmt_addr is not None:
            self.anchor(cmt_addr, where)
        idx = self.byte_index()
        self.spans.append((idx, n, cls, expect))
        if idx + n > len(self.vec):
            st["vec_overflow"] += 1
            self.advance(n)
            return
        # a comment whose bytes do not match the ROM at that address is stale:
        # its span is not written, only the walk position moves on
        proven = (expect is not None and self.rom_bytes is not None and
                  self.rom_bytes[idx:idx + n] == expect)
        if expect is not None and not proven:
            st["stale_anchor"] += 1
            self.advance(n)
            return
        for k in range(n):
            i = idx + k
            prev = self.vec[i]
            if prev and prev != cls:
                if proven:
                    st["overwrites"] += 1
                    self.vec[i] = cls
                else:
                    st["conflicts"] += 1
                    if len(st["conflict_samples"]) < 20:
                        st["conflict_samples"].append(
                            f"{where}: byte {idx + k:06X} "
                            f"{CLS_NAME.get(prev, prev)} vs {CLS_NAME[cls]}")
            elif not prev:
                self.vec[i] = cls
        self.advance(n)
        st["spans_written"] += 1

    # ---- one source line ---------------------------------------------------
    def line(self, raw, where):
        st = self.stats
        code, trail = raw, ""
        q = False
        for i, ch in enumerate(raw):
            if q:
                if ch == '"':
                    q = False
            elif ch == '"':
                q = True
            elif ch == ";":
                code, trail = raw[:i], raw[i:]
                break
        s = code.strip()
        if not s:
            return
        maddr = None
        for m in self.ADDR_CMT.finditer(trail):
            maddr = m
        cmt_addr, cmt_bytes, expect = None, None, None
        if maddr:
            cmt_addr = (int(maddr.group(1), 16), int(maddr.group(2), 16))
            bl = re.findall(r"\$[0-9A-Fa-f]{2}", maddr.group(3))
            if bl:
                cmt_bytes = len(bl)
                expect = bytes(int(x[1:], 16) for x in bl)
        # macro definition capture (raw body; resolved at expansion)
        md = re.match(r"^\s*macro\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*,\s*(.*))?$",
                      s, re.I)
        if self.mac_stack:
            if re.match(r"^\s*endm\s*$", s, re.I):
                name, params, body = self.mac_stack.pop()
                self.macros[name] = ([p.strip() for p in params.split(",")]
                                     if params else [], body)
            else:
                self.mac_stack[-1][2].append(raw.rstrip("\n"))
            return
        if md:
            self.mac_stack.append([md.group(1), md.group(2) or "", []])
            return
        # conditionals first: they shape what is assembled
        mc = re.match(r"^\s*(if|elif|else|endc)\b\s*(.*)$", s, re.I)
        if mc:
            self.conditional(mc.group(1).lower(), mc.group(2), where)
            return
        if self.skipping():
            return
        # rgbasm 1.0 anonymous label marker, possibly followed by a statement
        if s.startswith(":") and not s.startswith("::"):
            s2 = s[1:].strip()
            if s2:
                self.body(s2, cmt_addr, cmt_bytes, expect, where)
                return
        # labels (possibly followed by a statement)
        rest = s
        while True:
            ml = self.LABEL.match(rest)
            if not ml:
                break
            self.label(ml.group(1), where)
            rest = ml.group(2).strip()
            if not rest:
                return
        # rgbasm also accepts ".local <statement>" (colonless local label
        # followed by its statement on the same line)
        mdot = re.match(r"^(\.[A-Za-z_][A-Za-z0-9_.]*)\s+(.+)$", rest)
        if mdot:
            self.label(mdot.group(1), where)
            if not self.skipping():
                self.body(mdot.group(2).strip(), cmt_addr, cmt_bytes, expect,
                          where)
            return
        if rest:
            self.body(rest, cmt_addr, cmt_bytes, expect, where)

    def body(self, s, cmt_addr, cmt_bytes, expect, where):
        st = self.stats
        m = self.SEC.match(s)
        if m:
            self.section(m, where)
            return
        mi = re.match(r'^\s*include\s+"([^"]+)"', s, re.I)
        if mi:
            self.include(mi.group(1), where)
            return
        mm = re.match(r'^\s*incbin\s+"([^"]+)"\s*(?:,\s*(.*))?$', s, re.I)
        if mm:
            path = self.resolve(mm.group(1))
            size = path.stat().st_size if path else 0
            start, ln = 0, None
            if mm.group(2):
                args = op_span(mm.group(2))
                try:
                    start = self.eval(args[0], where)
                    if len(args) > 1:
                        ln = self.eval(args[1], where)
                except ExprError:
                    ln = None
                    st["len_unknown"] += 1
            n = (size - start) if ln is None else min(ln, size - start)
            if self.rom:
                self.span(n, CLS_DATA, where, cmt_addr)
            return
        md = re.match(r"^\s*(db|dw|dl|ds|dsb|dsw)\b\s*(.*)$", s, re.I)
        if md:
            d, rest = md.group(1).lower(), md.group(2)
            if d in ("ds", "dsb", "dsw"):
                args = op_span(rest) if rest.strip() else []
                try:
                    n = self.eval(args[0], where) if args else 0
                except ExprError:
                    n = None
                if n is None:
                    st["len_unknown"] += 1
                    return
                if self.rom:
                    self.span(n, CLS_PAD, where, cmt_addr)
                return
            cnt = 0
            for item in op_span(rest) if rest.strip() else []:
                if d == "db" and item.startswith('"'):
                    cnt += self.strlen(re.sub(r'\\(.)', r"\1", item[1:-1]))
                else:
                    cnt += {"db": 1, "dw": 2, "dl": 4}[d]
            if self.rom:
                self.span(cnt, CLS_DATA, where, cmt_addr)
            return
        tok = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$", s)
        if tok and tok.group(1) in self.macros:
            self.expand(tok.group(1), tok.group(2), where)
            return
        me = re.match(r"^\s*(?:def\s+)?([A-Za-z_][A-Za-z0-9_]*)\s+(equ|equs)\s+(.+)$",
                      s, re.I)
        if me:
            nm, kind, val = me.group(1), me.group(2).lower(), me.group(3).strip()
            if kind == "equs":
                q = re.match(r'^"((?:[^"\\]|\\.)*)"$', val)
                self.strings[nm] = q.group(1) if q else val
            else:
                try:
                    self.vars[nm] = self.eval(val, where)
                except ExprError:
                    st["def_failed"] += 1
            return
        mdef = re.match(r"^\s*def\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", s, re.I)
        mdef2 = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+)$", s)
        for mp in (mdef, mdef2):
            if mp:
                try:
                    self.vars[mp.group(1)] = self.eval(mp.group(2), where)
                except ExprError:
                    st["def_failed"] += 1
                return
        mpurge = re.match(r"^\s*purge\s+(.+)$", s, re.I)
        if mpurge:
            for nm in [x.strip() for x in mpurge.group(1).split(",")]:
                self.vars.pop(nm, None)
                self.strings.pop(nm, None)
            return
        if re.match(r"^\s*shift\b", s, re.I):
            st["shift_seen"] += 1
            return
        mrep = re.match(r"^\s*(rept|for)\s+(.*)$", s, re.I)
        if mrep:
            self.stats["loop_unevaluable"] += 1
            self.stats.setdefault("loop_samples", []).append(f"{where}: {s[:60]!r}")
            return
        if re.match(r"^\s*(endr|break|continue)\b", s, re.I):
            return
        if re.match(r"^\s*(assert|printt|println|printf|print\b|warn|fail|"
                    r"export\s|global\s|charmap|opt\b|pusho|popo|pushc|popc|"
                    r"pushs|pops|rsset|rsreset|rsvar|rbc|dbc|def\b|redef\b|"
                    r"union|nextu|endu|load\b|endl\b|newc|setc)", s, re.I):
            return
        if re.match(r"^\s*:\s*$", s):     # anonymous label marker (rgbasm 1.0)
            return
        mi2 = re.match(r"^\s*([a-z][a-z]*)\b\s*(.*)$", s, re.I)
        if mi2 and mi2.group(1).lower() in MNEMONICS:
            mn, ops = mi2.group(1).lower(), mi2.group(2)
            if cmt_bytes is not None:
                n = cmt_bytes
            else:
                n = instr_len(mn, ops)
                if n is None:
                    n = 1
                    st["model_fail"] += 1
                    st.setdefault("model_fail_samples", []).append(
                        f"{where}: {mn} {ops[:40]!r}")
                else:
                    st["len_model"] += 1
            if self.rom:
                self.span(n, CLS_CODE, where, cmt_addr, expect)
            return
        # rgbasm allows labels without a colon; a bare identifier is one
        # (mnemonics and macros never reach this point)
        if re.fullmatch(SYMTOK, s):
            self.label(s, where)
            return
        st["line_unknown"] += 1
        if len(st["unknown_samples"]) < 40:
            st["unknown_samples"].append(f"{where}: {s[:70]!r}")

    def section(self, m, where):
        st = self.stats
        _mod, _name, typ, rest = m.group(1), m.group(2), m.group(3).lower(), m.group(4)
        if typ not in ("rom0", "romx"):
            self.rom = False
            return
        self.rom = True
        mb = re.search(r"bank\s*\[([^\]]*)\]", rest, re.I)
        ma = re.search(r"\[([^\]]*)\]", rest)
        try:
            bank = self.eval(mb.group(1), where) if mb else 0
            addr = self.eval(ma.group(1), where) if ma else None
        except ExprError:
            st["section_unevaluable"] += 1
            return
        if addr is None:
            st["section_noaddr"] += 1
            return
        if self.rom:
            if self.region_ok and not self.region_code:
                # defer: bound at the next region start (walk end), not the
                # walk position that opaque lines never advanced
                self.section_pending.append(self.region_start)
            else:
                self.close_region()
        self.bank, self.addr = bank, addr
        self.region_start = self.byte_index()
        self.region_code = False
        self.region_ok = False
        self.region_log.append(self.region_start)

    # ---- macros ------------------------------------------------------------
    OPAQUE_MACROS = {"dialog_string_wrapping", "dialog_text", "dialog_ask_line",
                     "CHECKNAME", "MAKE_PRINT_ERROR", "sgb_data_send_cmd"}

    def expand(self, name, argstr, where):
        st = self.stats
        if name in self.OPAQUE_MACROS:
            st["opaque_macro"] += 1
            return
        params, body = self.macros[name]
        args = op_span(argstr) if argstr else []
        self.macro_uid += 1
        self.narg.append(len(args))
        subst = {}
        for i, p in enumerate(params):
            subst[p] = args[i] if i < len(args) else ""
        for i, a in enumerate(args[:9], 1):
            subst[str(i)] = a
        subst["@"] = str(self.macro_uid)
        # join continuation lines
        joined, buf = [], ""
        for ln in body:
            if buf:
                ln = buf + " " + ln.lstrip()
                buf = ""
            if ln.rstrip().endswith("\\") and ";" not in ln:
                buf = ln.rstrip()[:-1]
                continue
            joined.append(ln)
        if buf:
            joined.append(buf)
        self.emit_lines(joined, subst, args, where, name)
        self.narg.pop()

    def emit_lines(self, lines, subst, args, where, mname):
        """Process expanded macro body lines; REPT loops with SHIFT rotate
        the argument list per iteration (the `note` macro idiom)."""
        st = self.stats
        i = 0
        n = len(lines)
        while i < n:
            ln = lines[i]
            ml = re.match(r"^\s*(rept)\s+(.+?)\s*$", ln, re.I)
            if ml:
                # capture the loop body (single nesting level is enough here)
                depth, j = 1, i + 1
                while j < n and depth:
                    if re.match(r"^\s*rept\b", lines[j], re.I):
                        depth += 1
                    elif re.match(r"^\s*endr\b", lines[j], re.I):
                        depth -= 1
                    j += 1
                loop_body = lines[i + 1:j - 1] if depth == 0 else lines[i + 1:n]
                # evaluate the count against the current args
                cnt_src = re.sub(r"\\([0-9@])|\\([A-Za-z_][A-Za-z0-9_]*)",
                                 lambda m2: subst.get(m2.group(1) or
                                                      m2.group(2), m2.group(0)),
                                 ml.group(2))
                try:
                    cnt = self.eval(cnt_src, f"{where}>{mname}")
                except ExprError:
                    cnt = None
                if cnt is None:
                    st["loop_unevaluable"] += 1
                    i = j
                    continue
                cur = list(args)
                for _ in range(min(cnt, 256)):
                    sub2 = dict(subst)
                    for k, a in enumerate(cur[:9], 1):
                        sub2[str(k)] = a
                    self.narg[-1] = len(cur)
                    self.emit_lines(loop_body, sub2, list(cur), where, mname)
                    # SHIFT inside the body rotates for the next iteration
                    if any(re.match(r"^\s*shift\b", b, re.I) for b in loop_body):
                        if cur:
                            cur.pop(0)
                self.narg[-1] = len(args)
                i = j
                continue
            if re.match(r"^\s*shift\b", ln, re.I):
                if args:
                    args.pop(0)
                self.narg[-1] = max(len(args), 0)
                i += 1
                continue
            sub = re.sub(r"\\([0-9@])|\\([A-Za-z_][A-Za-z0-9_]*)",
                         lambda m2: subst.get(m2.group(1) or m2.group(2),
                                              m2.group(0)), ln)
            self.line(sub, f"{where}>{mname}")
            i += 1

    # ---- top ---------------------------------------------------------------
    def walk(self, defines):
        # rgbasm builtin version symbols (rgbdscheck.asm / hardware.inc)
        self.vars["__RGBDS_MAJOR__"] = 1
        self.vars["__RGBDS_MINOR__"] = 0
        self.vars["__RGBDS_PATCH__"] = 4
        for d in defines:
            nm, _, val = d.partition("=")
            nm, val = nm.strip(), (val.strip() if _ else "1")
            self.vars[nm] = val
        self.include_file(self.root / "src" / "main.asm", "main.asm")
        self.close_region()
        # no-code regions a section switch interrupted: bound at the next
        # region start in the same bank, else the end of the bank
        for idx in self.section_pending:
            bank = idx // 0x4000
            nxt = [r for r in self.region_log
                   if r > idx and r // 0x4000 == bank]
            hi = min(nxt) if nxt else (bank + 1) * 0x4000
            self.fill(idx, hi, CLS_DATA, "region-nocode")


def parse_sym(path: Path):
    sym = {}
    for ln in open(path, errors="replace"):
        ln = ln.strip()
        if not ln or ln.startswith(";"):
            continue
        m = re.match(r"^([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+(\S+)$", ln)
        if m:
            sym.setdefault(m.group(3), []).append(
                (int(m.group(1), 16), int(m.group(2), 16)))
    return sym


def parse_charmap(root: Path):
    ch = {}
    for f in (root / "src").rglob("*.asm"):
        try:
            txt = f.read_text(errors="replace")
        except OSError:
            continue
        if not re.search(r"^\s*charmap\b", txt, re.M | re.I):
            continue
        for ln in txt.splitlines():
            m = re.match(r'^\s*charmap\s+"((?:[^"\\]|\\.)*)"\s*,\s*(\S+)', ln, re.I)
            if m:
                seq = m.group(1)
                ch[seq] = len(re.findall(r"\$[0-9A-Fa-f]{2}|[0-9]+", m.group(2)))
    return ch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("repo", type=Path)
    ap.add_argument("--revision", default="azle")
    ap.add_argument("--map", required=True)
    ap.add_argument("--rom")
    ap.add_argument("--stats")
    a = ap.parse_args()

    root = a.repo.resolve()
    symf = root / f"{a.revision}.sym"
    if not symf.exists():
        print(f"missing {symf}; build the ROM first (make {a.revision})",
              file=sys.stderr)
        return 1
    sym = parse_sym(symf)
    charmap = parse_charmap(root)
    keys = ["label_resync", "resync_same_bank", "resync_bank_jump",
            "labels_matched", "label_no_sym", "anchor_drift", "len_unknown",
            "len_model", "model_fail", "line_unknown", "opaque_macro",
            "cond_unevaluable", "def_failed", "files", "spans", "spans_written",
            "conflicts", "overwrites", "stale_anchor", "vec_overflow",
            "bank0_overflow", "include_missing", "fill_region-nocode",
            "fill_gap-data", "fill_gap-code", "shift_seen"]
    stats = defaultdict(int, {k: 0 for k in keys})
    stats["mismatch_samples"], stats["unknown_samples"] = [], []
    stats["conflict_samples"], stats["drift_samples"] = [], []
    stats["cond_samples"], stats["model_fail_samples"] = [], []
    stats["loop_samples"], stats["def_failed_samples"] = [], []
    w = Walker(root, sym, stats, charmap)
    if a.rom:
        w.rom_bytes = (root / a.rom).read_bytes()
    defines = [f"LANG={a.revision[3].upper()}", "VERSION=0"] \
        if a.revision.startswith("azl") else []
    w.walk(defines)

    # whole banks left entirely empty by the disassembly (rgblink zero-fill)
    rom = w.rom_bytes
    zero_banks = 0
    pad_filler = 0
    if rom:
        for b in range(64):
            base = b * 0x4000
            if not any(w.vec[base:base + 0x4000]) and \
                    rom[base:base + 0x4000] == bytes(0x4000):
                for k in range(base, base + 0x4000):
                    w.vec[k] = CLS_PAD
                zero_banks += 1
        # uniform filler runs the disassembly never defined (rgblink gaps):
        # all-00 or all-FF unknown runs are padding
        i = 0
        while i < len(w.vec):
            if w.vec[i]:
                i += 1
                continue
            j = i
            while j < len(w.vec) and not w.vec[j]:
                j += 1
            seg = rom[i:j]
            if seg and (seg == bytes(len(seg)) or seg == b"\xff" * len(seg)):
                for k in range(i, j):
                    w.vec[k] = CLS_PAD
                pad_filler += j - i
            i = j
        # a uniform 00/FF suffix running to the exact bank end is linker pad,
        # even where a no-code region fill claimed it as data
        for b in range(64):
            end = (b + 1) * 0x4000
            k = end - 1
            fill = w.vec[end - 1]
            if fill not in (0, CLS_DATA):
                continue
            byte = rom[end - 1]
            while k >= b * 0x4000 and w.vec[k] == fill and rom[k] == byte:
                k -= 1
            if end - 1 - k >= 8 and byte in (0x00, 0xFF):
                for q in range(k + 1, end):
                    w.vec[q] = CLS_PAD
                pad_filler += end - 1 - k

    # .map text: runs over the class vector (CPU-view windows per bank)
    n = len(w.vec)
    out = [f"# LADX-Disassembly reference classes, revision {a.revision}",
           "# tool: memscan/tools/ladx_disasm_ref.py (code/data/padding from the asm walk)",
           "# labels resync every region; comment bytes verified against the ROM"]
    cur, start = None, 0
    for i in range(n + 1):
        v = w.vec[i] if i < n else -1
        # a run never crosses a bank boundary: maprender's lines are
        # bank-local (CPU-view windows), a cross-bank end would read as
        # "end < start" and the whole run would be dropped
        if v != cur or (i and (i & 0x3FFF) == 0 and cur is not None):
            if cur is not None and cur != -1:
                b, o = divmod(start, 0x4000)
                e = i - 1
                win = 0x4000 if b else 0
                name = CLS_NAME.get(cur, "unknown")
                out.append(f"{b} {o + win:x} {(e & 0x3FFF) + win:x} {name}")
            cur, start = v, i
    Path(a.map).write_text("\n".join(out) + "\n")

    # ROM byte verification of the comment spans (end-to-end check)
    rom = w.rom_bytes
    ver_ok = ver_bad = ver_total = 0
    bad_samples = []
    if rom:
        for idx, ln, _cls, expect in w.spans:
            if expect is None:
                continue
            ver_total += ln
            seg = rom[idx:idx + ln]
            if seg == expect:
                ver_ok += ln
            else:
                ver_bad += ln
                if len(bad_samples) < 15:
                    bad_samples.append(
                        f"{idx:06X} want {expect.hex()} got {seg.hex()}")
    counts = {c: w.vec.count(c) for c in set(w.vec)}
    unk_bank = [(w.vec[b * 0x4000:(b + 1) * 0x4000].count(0), b)
                for b in range(64) if w.vec[b * 0x4000:(b + 1) * 0x4000].count(0)]
    unk_bank.sort(reverse=True)
    rep = [f"labels: {len(sym)}   files walked: {stats['files']}",
           f"spans: {len(w.spans)} written {stats['spans_written']}  "
           f"conflicts {stats['conflicts']}  overwrites {stats['overwrites']}  "
           f"stale anchors {stats['stale_anchor']}",
           f"classed bytes: code {counts.get(1, 0)}  data {counts.get(2, 0)}  "
           f"pad {counts.get(3, 0)}  "
           f"region-fill {stats.get('fill_region-nocode', 0)}  "
           f"gap-fill {stats.get('fill_gap-data', 0) + stats.get('fill_gap-code', 0)}",
           f"vector: {n} bytes, unknown {counts.get(0, 0)} "
           f"({100.0 * counts.get(0, 0) / n:.2f}%)",
           f"zero-filled banks marked padding: {zero_banks}  "
           f"uniform filler runs: {pad_filler}",
           f"labels matched: {stats['labels_matched']}  resynced: "
           f"{stats['label_resync']} (same-bank {stats['resync_same_bank']}, "
           f"bank-jump {stats['resync_bank_jump']})  no-sym: {stats['label_no_sym']}",
           f"anchor drifts snapped: {stats['anchor_drift']}",
           f"lengths from model: {stats['len_model']}  model fails: "
           f"{stats['model_fail']}  unknown lines: {stats['line_unknown']}  "
           f"opaque macro calls: {stats['opaque_macro']}",
           f"unevaluable conditions: {stats['cond_unevaluable']}  "
           f"defs failed: {stats['def_failed']}"]
    if unk_bank:
        rep.insert(4, "banks with most unknown: " +
                   ", ".join(f"{b:02X}:{u}" for u, b in unk_bank[:16]))
    if rom:
        rep.append(f"ROM md5: {hashlib.md5(rom).hexdigest()}  size {len(rom)}")
        rep.append(f"comment bytes verified against ROM: {ver_ok}/{ver_total} "
                   f"(mismatch {ver_bad})")
    text = "\n".join(rep) + "\n"
    if a.stats:
        Path(a.stats).write_text(text)
    print(text)
    for k in ("mismatch_samples", "conflict_samples", "unknown_samples",
              "model_fail_samples", "cond_samples", "drift_samples",
              "def_failed_samples"):
        for s in stats[k][:12]:
            print(f"  {k[:-9]}: {s}")
    for s in bad_samples[:8]:
        print(f"  bad-span: {s}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
