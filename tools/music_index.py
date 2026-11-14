#!/usr/bin/env python3
"""Unified local index of music-rip knowledge (BAB databases + rip collections).

Merges the independent sources of "where is the music engine in a ROM" into one
SQLite database built for fast search and fast exploitation by deepeel:

- BAB suite (output/bab/db/*.tsv, fetched by bab_fetch.py): per-game ROM-space
  annotations keyed by ROM CRC — engine load/init/play, timer, song table
  address (S_ADDR), copy chunks `C rom,len,file-offset`, patch ranges, product
  codes on the Sega/MSX columns.
- rip collections (output/gbs/**, or any directory of rip files): every header
  format of the game-music-emu family is parsed generically (GBS, NSF, NSFE,
  KSS, HES, SPC, VGM/VGZ, AY, SAP, GYM) into the same normalized projection;
  ROM-space coordinates come later, from deepeel --gbs runtime verification
  (the `verify` table).

Nothing is dropped: every source row is stored verbatim (`raw` + `columns`
JSON with the original header names) alongside the parsed projection.  Every
record carries self-contained provenance (`origin` URI + per-source chain in
`source`: site URL, archive SHA-256, TSV SHA-256, extraction path).

Schema highlights:
  entry     one row per source record (both sources), parsed + verbatim
  source    one row per input DB/collection + its full provenance chain
  cross_ref precomputed links (crc / product_code / title_norm)
  verify    runtime results appended by deepeel --gbs sessions (starts empty)

Usage:
  music_index.py build [--bab DIR] [--rip-dirs DIR...] [--db FILE]
  music_index.py query [--db FILE] NAME|--crc CRC|--code CODE [--source X] [-v]
  music_index.py validate [--db FILE] --roms DIR...   (real-ROM validation)
  music_index.py stats [--db FILE]
  music_index.py export [--db FILE] [--out FILE.json]
"""

import argparse
import hashlib
import json
import re
import sqlite3
import struct
import sys
import zlib
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 2
INDEX_VERSION = "music_index/2.0"

BAB_SOURCES = [
    # (db file stem, source name, platform, format)
    ("GB_gbs2", "bab_gbs2", "GB", "gbs2"),
    ("GB_gbs2_op", "bab_gbs2_op", "GB", "gbs2"),
    ("GB_gbs2010", "bab_gbs2010", "GB", "gbs2010"),
    ("NES_nsf_cart", "bab_nes_cart", "NES", "nsf"),
    ("NES_fds_v2", "bab_nes_fds", "NES", "nsf"),
    ("NES_fds_v21", "bab_nes_fds_v21", "NES", "nsf"),
    ("NES_fds_hacker", "bab_nes_fds_hacker", "NES", "nsf"),
    ("PCE_hes", "bab_pce_hes", "PCE", "hes"),
    ("MSX_kss", "bab_msx_kss", "MSX", "kss"),
    ("GG_kss_k", "bab_gg_kss", "GG", "kss"),
    ("GG_sgc_s", "bab_gg_sgc", "GG", "sgc"),
    ("MK3_kss_k", "bab_mk3_kss", "MK3", "kss"),
    ("MK3_sgc_s", "bab_mk3_sgc", "MK3", "sgc"),
    ("SCSG_kss_k", "bab_scsg_kss", "SCSG", "kss"),
    ("SCSG_sgc_s", "bab_scsg_sgc", "SCSG", "sgc"),
]

BAB_ORIGIN = "bab:mrchecker.web.fc2.com"  # provenance root of every BAB DB

INT_COLS = {
    "load": ("$06-07_LOAD", "LOAD", "ROAD", "$18-1B LOAD_ADD", "$18-1B LOAD"),
    "size": ("SIZE", "INIT_SIZE", "FILE_SIZE"),
    "init": ("$08-09_INIT", "INIT", "$06-07 INIT"),
    "play": ("PLAY", "$0A-0B_TIMER", "TIMER"),
    "stack": ("STACK", "$0C-0D_STAK"),
    "tma": ("$0E_TMA", "TMA"),
    "tac": ("$0F_TMC", "TMC", "TAC"),
    "songs": ("曲数", "$04_曲数", "$04 曲数"),
    "bgm": ("BGM数", "ＢＧＭ", "BGM"),
    "se": ("SE", "効果音"),
    "fsize": ("F-SIZE", "FILE_SIZE", "$14-17 L_SIZE"),
    "s_addr": ("S_ADDR", "S-ADDR", "$1C-1D SONG_ADD"),
}
CODE_COLS = ("型番", "品番")
TITLE_COLS = ("タイトル",)
YEAR_COLS = ("発売年", "発売日", "年", "製造年")
MAKER_COLS = ("メーカー",)

# ---------------------------------------------------------------- helpers


def h2i(s):
    """Hex (or decimal) column text -> int, None when absent/placeholder."""
    s = (s or "").strip().rstrip("H")
    if not re.fullmatch(r"[0-9A-Fa-f]+", s or ""):
        return None
    return int(s, 16)


def norm_name(s):
    """Same normalization as dp_gbs_norm_name: lowercase alphanumeric run."""
    return "".join(c for c in (s or "").lower() if c.isalnum())


def parse_chunks(text):
    """`C rom,dest,len` triplets -> [[rom, dest, len], ...] as ints.

    Semantics validated against real ROMs: dest+len == F-SIZE (chunks tile the
    rip file body after the ripper's own stub), the file body loads linearly at
    LOAD, so rip address A = LOAD + dest-of-the-chunk-containing-(A-LOAD); and
    rom is a dp_rom_index (bank*0x4000 + offset)."""
    if not text:
        return []
    return [[int(a, 16), int(b, 16), int(c, 16)]
            for a, b, c in re.findall(r"C\s+([0-9A-Fa-f]+),([0-9A-Fa-f]+),([0-9A-Fa-f]+)", text)]
    if not text:
        return []
    return [[int(a, 16), int(b, 16), int(c, 16)]
            for a, b, c in re.findall(r"C\s+([0-9A-Fa-f]+),([0-9A-Fa-f]+),([0-9A-Fa-f]+)", text)]


def parse_patches(text):
    """`start-end[hex bytes]` ranges -> [[start, end, "hex"], ...] (rip file space)."""
    if not text:
        return []
    return [[int(a, 16), int(b, 16), re.sub(r"[^0-9A-Fa-f]", "", hx)]
            for a, b, hx in re.findall(r"([0-9A-Fa-f]+)-([0-9A-Fa-f]+)\[([0-9A-Fa-f ]*)\]", text)]


def parse_2010_copies(text):
    """2010 `romStart:destStart-romEnd:destEnd` -> [[rom0, dest0, rom1, dest1]]
    (delta validated equal on both sides on real ROMs)."""
    if not text:
        return []
    return [[int(a, 16), int(b, 16), int(c, 16), int(d, 16)]
            for a, b, c, d in
            re.findall(r"([0-9A-Fa-f]+):([0-9A-Fa-f]+)-([0-9A-Fa-f]+):([0-9A-Fa-f]+)", text)]


def parse_2010_patches(text):
    """2010 `romIndex:byte` 1-byte patches -> [[rom_index, byte], ...]."""
    if not text:
        return []
    return [[int(a, 16), int(b, 16)]
            for a, b in re.findall(r"([0-9A-Fa-f]{4,8}):([0-9A-Fa-f]{2})", text)]


def prod_code(text):
    """Sega/MSX product codes (T-106017, OM-G008) as uppercase key."""
    m = re.search(r"\b([A-Z]{1,3}-\d{3,6}|\d?[A-Z]{2}-\w{2,6})\b", (text or "").upper())
    return m.group(1) if m else None


def gbs_prod_code(filename):
    """Game Boy product code from a rip file name (DMG-MLA, CGB-BXXX)."""
    m = re.search(r"\b([Dd][Mm][Gg]|[Mm][Gg][Bb]|[Cc][Gg][Bb])-([A-Za-z0-9]{2,4})", filename)
    return f"{m.group(1).upper()}-{m.group(2).upper()}" if m else None


# ---------------------------------------------- generic rip header parsers
# One normalized dict per rip file, covering every header format of the
# game-music-emu family; anything not modeled stays in `extras` and the full
# first 128 header bytes stay in `raw_hex`.


def _tags16(b, off):
    return b[off:off + 32].split(b"\0")[0].decode("latin1", "replace").strip()


def _rip(fmt, b, **kw):
    kw["format"] = fmt
    kw["raw_hex"] = b[:128].hex()
    kw.setdefault("extras", {})
    return kw


def _p_gbs(b):
    load, init, play, stack = struct.unpack_from("<HHHH", b, 6)
    return _rip("gbs", b, load=load, init=init, play=play, stack=stack,
                tma=b[14], tac=b[15], songs=b[4], first=b[5],
                title=_tags16(b, 16), author=_tags16(b, 48), copyright=_tags16(b, 80))


def _p_nsf(b):
    return _rip("nsf", b, load=struct.unpack_from("<H", b, 7)[0],
                init=struct.unpack_from("<H", b, 9)[0],
                play=struct.unpack_from("<H", b, 11)[0],
                songs=b[5], first=b[6],
                tma=None, tac=None, stack=None,
                title=_tags16(b, 14), author=_tags16(b, 46), copyright=_tags16(b, 78),
                extras={"pal_ntsc": b[122], "expansion": b[123]})


def _p_nsfe(b):
    # chunked; the INFO chunk carries the entry points
    p = 4
    while p + 8 <= len(b):
        ln, name = struct.unpack_from("<I4s", b, p)
        if name == b"INFO" and ln >= 8:
            load, init, play, songs, first = struct.unpack_from("<HHHHH", b, p + 8)
            return _rip("nsfe", b, load=load, init=init, play=play,
                        songs=songs, first=first, tma=None, tac=None, stack=None)
        p += 8 + ln
    return _rip("nsfe", b, tma=None, tac=None, stack=None)


def _p_kss(b):
    return _rip("kss", b, load=struct.unpack_from("<H", b, 6)[0],
                init=struct.unpack_from("<H", b, 8)[0],
                play=None, stack=None, tma=None, tac=None,
                extras={"device": b[10] if len(b) > 10 else None})


def _p_hes(b):
    vecs = list(struct.unpack_from("<4H", b, 8)) if len(b) >= 16 else []
    return _rip("hes", b, load=None, init=vecs[0] if vecs else None,
                play=None, stack=None, tma=None, tac=None,
                extras={"vectors": vecs})


def _p_spc(b):
    return _rip("spc", b, pc=struct.unpack_from("<H", b, 0x22)[0],
                songs=1, load=None, init=None, play=None, stack=None, tma=None, tac=None,
                title=_tags16(b, 0x2E), author=_tags16(b, 0xD0),
                extras={"game": _tags16(b, 0x4E)})


def _p_vgm(b):
    return _rip("vgm", b, load=None, init=None, play=None, stack=None, tma=None, tac=None,
                extras={"version": struct.unpack_from("<I", b, 8)[0],
                        "gd3_offset": struct.unpack_from("<I", b, 0x14)[0],
                        "total_samples": struct.unpack_from("<I", b, 0x18)[0]})


def _p_ay(b):
    return _rip("ay", b, player=struct.unpack_from("<H", b, 8)[0],
                init=struct.unpack_from("<H", b, 10)[0],
                songs=b[12], load=None, play=None, stack=None, tma=None, tac=None)


def _p_sap(b):
    d = {"INIT": None, "PLAYER": None, "SONGS": None}
    tags = {}
    for line in b[:512].split(b"\r\n"):
        try:
            t = line.decode("latin1").strip()
        except Exception:
            continue
        m = re.match(r"(INIT|PLAYER)\s+([0-9A-Fa-f]{4})$", t)
        if m:
            d[m.group(1)] = int(m.group(2), 16)
        elif re.match(r"SONGS\s+\d+$", t):
            d["SONGS"] = int(t.split()[1])
        elif " " in t and re.fullmatch(r"[A-Z]+ .{0,80}", t):
            k, v = t.split(" ", 1)
            tags[k] = v.strip()
        if line == b"\xff":
            break
    return _rip("sap", b, load=None, init=d["INIT"], play=d["PLAYER"],
                songs=d["SONGS"], stack=None, tma=None, tac=None,
                title=tags.get("NAME"), author=tags.get("AUTHOR"), extras=tags)


def _p_gym(b):
    return _rip("gym", b, load=None, init=None, play=None, stack=None, tma=None, tac=None,
                extras={"gd3": b"Gd3 " in b[-1024:]})


MAGICS = [
    (b"GBS", _p_gbs), (b"NESM\x1a", _p_nsf), (b"NSFE", _p_nsfe), (b"KSSX", _p_kss),
    (b"HESM", _p_hes), (b"SNES-SPC700", _p_spc), (b"Vgm ", _p_vgm),
    (b"ZXAYEMUL", _p_ay), (b"SAP ", _p_sap),
]
RIP_EXTS = {".gbs", ".nsf", ".nsfe", ".kss", ".hes", ".spc", ".vgm", ".vgz",
            ".ay", ".sap", ".gym"}


def rip_parse(path):
    """Normalized rip record for any game-music-emu-family file; None when unknown."""
    b = path.read_bytes()
    r = None
    for magic, p in MAGICS:
        if b[:len(magic)] == magic:
            r = p(b)
            break
    if r is None and path.suffix.lower() == ".gym":
        r = _p_gym(b)
    if r is None:
        return None
    r["data_size"] = len(b)
    r["sha256"] = hashlib.sha256(b).hexdigest()
    return r


# ------------------------------------------------------------------ schema

SCHEMA = """
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE source(name TEXT PRIMARY KEY, platform TEXT, format TEXT, file TEXT,
                    version_stamp TEXT, rows INTEGER, raw_header TEXT,
                    origin TEXT, archive_sha256 TEXT, file_sha256 TEXT,
                    extractor TEXT, license TEXT);
CREATE TABLE entry(
  id INTEGER PRIMARY KEY,
  source TEXT NOT NULL,           -- bab_gbs2 | joshw_gbs | ...
  platform TEXT NOT NULL,         -- GB | GG | MK3 | SCSG | MSX | PCE | NES
  format TEXT NOT NULL,           -- gbs2 | kss | sgc | gbs | nsf | 2010 | hes | ...
  row_no INTEGER,                 -- 1-based line number in the source TSV
  title TEXT, title_norm TEXT, game_norm TEXT, year TEXT, maker TEXT,
  product_code TEXT,
  crcs TEXT,                      -- JSON list of the verbatim CRC columns
  crc1 TEXT,                      -- first CRC, normalized lookup key
  songs INTEGER, bgm INTEGER, se INTEGER,
  load INTEGER, size INTEGER, fsize INTEGER,
  init INTEGER, play INTEGER, stack INTEGER,
  tma INTEGER, tac INTEGER,
  init_rom INTEGER, play_rom INTEGER,  -- entry points projected into ROM space
                                  -- through the chunks (precomputed; None when
                                  -- the address falls in the ripper's own stub
                                  -- or no chunk covers it)
  s_addr INTEGER,                 -- song table, verbatim hex text -> int
  s_addr_hi INTEGER, s_addr_lo INTEGER,  -- candidate splits of the 6+ digit text
                                  -- (hi=leading 2 digits as bank, lo=last 4 as offset);
                                  -- the exact convention is not settled: do not
                                  -- interpret, use the raw value (columns JSON)
  chunks TEXT,                    -- JSON [[rom, dest, len], ...] of `C rom,dest,len`
                                  -- (semantics ROM-validated: dest+len == F-SIZE,
                                  -- file body loads linearly at LOAD; see below)
  patches TEXT,                   -- JSON [[start, end, "hexbytes"], ...] (rip file space)
  pcm TEXT, fm TEXT, irq TEXT,    -- platform-specific flags kept visible
  data_sha256 TEXT, data_size INTEGER,  -- rip file identity
  rip_json TEXT,                  -- full parsed rip header + tags (JSON)
  origin TEXT NOT NULL,           -- self-contained provenance URI
  game_key INTEGER,               -- cross-region/cross-dump identity: connected
                                  -- component over crc/code/title links; one game
                                  -- = one key, whatever the dump or format
  raw TEXT NOT NULL,              -- the verbatim source line (or file name for rips)
  columns TEXT NOT NULL           -- JSON {original column name: verbatim value}
);
CREATE INDEX idx_entry_crc1 ON entry(crc1);
CREATE INDEX idx_entry_norm ON entry(title_norm);
CREATE INDEX idx_entry_code ON entry(product_code);
CREATE INDEX idx_entry_src ON entry(source);
CREATE INDEX idx_entry_platform ON entry(platform);
CREATE INDEX idx_entry_game ON entry(game_key);
CREATE TABLE cross_ref(
  a INTEGER NOT NULL,             -- entry.id
  b INTEGER NOT NULL,             -- entry.id
  kind TEXT NOT NULL,             -- crc | product_code | title_norm
  PRIMARY KEY(a, b, kind)
);
CREATE TABLE verify(             -- runtime results of deepeel --gbs (starts empty)
  entry_id INTEGER NOT NULL, rom_crc32 TEXT NOT NULL, rom_sha256 TEXT NOT NULL,
  method TEXT,                    -- bank | fragment
  init_bank INTEGER, init_addr INTEGER, play_bank INTEGER, play_addr INTEGER,
  banks_matched INTEGER, frag_total INTEGER, frag_matched INTEGER,
  frag_dom_delta INTEGER, frag_dom_votes INTEGER,
  init_votes INTEGER, play_votes INTEGER, best_ratio REAL,
  deepeel_version TEXT, at TEXT DEFAULT (datetime('now')),
  PRIMARY KEY(entry_id, rom_sha256, rom_crc32)
);
"""


def connect(db):
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    return con


def pick(cols, names):
    for n in names:
        for k, v in cols.items():
            if k.strip() == n:
                return v
    return None


def make_projection(source, platform, fmt, row_no, cols, raw, origin):
    """Common projection of one source row; everything else stays in `columns`."""
    def i(*names):
        v = pick(cols, names)
        return h2i(v) if v not in (None, "") else None
    title = pick(cols, TITLE_COLS)
    crcs = [v.strip() for k, v in cols.items()
            if re.fullmatch(r"CRC\d*", k.strip()) and v.strip() not in ("", "-")]
    code = prod_code(pick(cols, CODE_COLS) or "")
    s_addr = i("S_ADDR", "S-ADDR", "$1C-1D SONG_ADD")
    if fmt == "gbs2010":  # 2010 columns use different names
        s_addr = i("Song_tab_開始アド") or s_addr
    chunks = parse_2010_copies(pick(cols, ("Copy_アド",)) or "") if fmt == "gbs2010" \
        else parse_chunks(pick(cols, ("ROM_ADD",)) or "")
    patches = parse_2010_patches(pick(cols, ("1Byte_change",)) or "") if fmt == "gbs2010" \
        else parse_patches(pick(cols, ("PACH_ADD/DAT", "PACH_ADD/DAT ")) or "")
    # S_ADDR text splits: the source uses a 6+ digit notation whose convention
    # (rom index? bank:offset in the rip?) is not settled — store both halves
    hi2 = lo4 = None
    sa_txt = (pick(cols, ("S_ADDR", "S-ADDR", "$1C-1D SONG_ADD")) or "").strip()
    if fmt == "gbs2010":
        sa_txt = (pick(cols, ("Song_tab_開始アド",)) or "").strip()
    if re.fullmatch(r"[0-9A-Fa-f]{6,}", sa_txt):
        hi2, lo4 = int(sa_txt[:-4], 16), int(sa_txt[-4:], 16)
    # version-stripped identity: [V1.0], (V1.1), V1.2… suffixes separate revisions
    # of one game; the game_norm link groups them under one game_key
    game_norm = norm_name(re.sub(r"[\[(（]?\s*[Vv⁻^]?\d+(\.\d+)*\s*[\])）]?", "", title or ""))
    return (
        source, platform, fmt, row_no,
        title, norm_name(title), game_norm, pick(cols, YEAR_COLS), pick(cols, MAKER_COLS),
        code, json.dumps(crcs, ensure_ascii=False), crcs[0] if crcs else None,
        i("曲数", "$04_曲数", "$04 曲数", "全曲数", "連番") if fmt != "gbs2010" else i("$04_曲数"),
        i("BGM数", "ＢＧＭ", "BGM") if fmt != "gbs2010" else i("BGM数"),
        i("SE", "効果音") if fmt != "gbs2010" else i("SE"),
        i("$06-07_LOAD", "LOAD", "ROAD", "$18-1B LOAD_ADD", "$18-1B LOAD"),
        i("SIZE", "FILE_SIZE", "INIT_SIZE"),
        i("F-SIZE", "FILE_SIZE", "$14-17 L_SIZE", "$14-17 L_SIZE "),
        i("$08-09_INIT", "INIT", "$06-07 INIT", "INIT "),
        i("PLAY", "$0A-0B_TIMER", "TIMER"),
        i("STACK", "$0C-0D_STAK", "STACK "),
        i("$0E_TMA", "TMA"), i("$0F_TMC", "TMC", "TAC"),
        s_addr, hi2, lo4,
        json.dumps(chunks), json.dumps(patches),
        pick(cols, ("PCM",)), pick(cols, ("ＦＭ", "FM")), pick(cols, ("IRQ",)),
        None, None, None, origin, raw, json.dumps(cols, ensure_ascii=False),
    )


ENTRY_COLS = ("source,platform,format,row_no,title,title_norm,game_norm,year,maker,product_code,"
              "crcs,crc1,songs,bgm,se,load,size,fsize,init,play,stack,tma,tac,"
              "init_rom,play_rom,"
              "s_addr,s_addr_hi,s_addr_lo,chunks,patches,pcm,fm,irq,"
              "data_sha256,data_size,rip_json,origin,raw,columns").split(",")


def _read_verified(path, load, data_size):
    """Parse a `.verified` sidecar (deepeel --gbs byte-verify output): one line
    per verified dump, `dump crc=HEX load=N data_size=N banks=r0,r1,... [k=v]...
    rom=NAME` (rom last: dump names carry spaces; banks are the ROM bank of each
    rip bank in order, -1 when that rip bank did not match).  Lines whose
    load/data_size disagree with the parsed rip are refused (a sidecar belongs
    to exactly one rip file).  init_rom/play_rom are dp_rom_index values."""
    out = []
    if not path.exists():
        return out
    for ln in path.read_text(encoding="utf-8").splitlines():
        f = ln.split()
        if not f or f[0] != "dump":
            continue
        d = {}
        rom = None
        for kv in f[1:]:
            k, _, v = kv.partition("=")
            if k == "rom":
                rom = ln.split(" rom=", 1)[1] if " rom=" in ln else v
                break
            d[k] = v
        try:
            blk = int(d.get("data_size", -1))
            # the verifier's data_size excludes the 112-byte GBS header that
            # rip_parse counts; both conventions name the same file
            if int(d.get("load", -1)) != load or blk not in (data_size, data_size - 112):
                print(f"verified: {path.name}: load/data_size mismatch, line refused")
                continue
            banks = [None if b == "-1" else int(b) for b in d.get("banks", "").split(",") if b != ""]
            rec = {"crc": int(d["crc"], 16), "banks": banks}
            if "init_rom" in d:
                rec["init_rom"] = int(d["init_rom"])
            if "play_rom" in d:
                rec["play_rom"] = int(d["play_rom"])
            if rom:
                rec["rom"] = rom
            out.append(rec)
        except (KeyError, ValueError):
            print(f"verified: {path.name}: malformed line refused")
    return out


def _rom_entry_points(proj):
    """Project init/play into ROM space through the chunks.

    The rip file body loads linearly at `load` (ROM-validated): rip address A
    lives at data offset A - load.  Two chunk shapes:
    - 2022 `C rom,dest,len` (3 fields): the chunk with dest <= foff < dest+len
      maps it to rom_index = rom + (foff - dest).  Direct ranges only: the
      multi-chunk rows copy scattered pieces, so there is no single linear map.
    - 2010 `romStart:destStart-romEnd:destEnd` (4 fields, inclusive ends, data
      space: 516/531 play cross-checks exact against the 2022 siblings): same
      direct mapping first; then, when every chunk of the row shares one base
      (rom0 - dest0, one contiguous extract), a play address the ranges do not
      cover (the ripper's stub region before the extract) projects by that
      base — cross-validated 241/259 on the 2022 siblings, so it is a play-only
      fallback, never allowed to override a direct range and never applied to
      init (1/14: the init wrapper is stub-local, projecting it would seed 93%
      wrong addresses)."""
    # indices in the make_projection tuple (before the rom columns are inserted)
    load, init, play = proj[15], proj[18], proj[19]
    chunks = json.loads(proj[26]) if isinstance(proj[26], str) else (proj[26] or [])
    if load is None or not chunks:
        return [None, None]

    def proj_addr(a, allow_base):
        if a is None or a < load:
            return None
        foff = a - load
        bases = set()
        hit = None
        for t in chunks:
            if len(t) == 3:
                rom, dest, ln = t
                if dest <= foff < dest + ln:
                    return rom + (foff - dest)
            elif len(t) == 4:   # 2010 romStart:destStart-romEnd:destEnd
                rom0, d0, rom1, d1 = t
                bases.add(rom0 - d0)
                if d0 <= foff <= d1:
                    hit = rom0 + (foff - d0)
        if hit is not None:
            return hit
        # base fallback, play only: the play wrapper is stable across ripper
        # generations (241/259 exact against the 2022 siblings) while the init
        # wrapper is stub-local (1/14 — projecting it would seed 93% wrong
        # addresses), so init outside every range stays None
        if allow_base and len(bases) == 1:   # one contiguous extract
            return next(iter(bases)) + foff
        return None
    return [proj_addr(init, False), proj_addr(play, True)]


def build(bab_dir, rip_dirs, dbfile, alias_files=()):
    con = connect(dbfile)
    con.executescript("PRAGMA journal_mode=WAL;")
    con.executescript(SCHEMA)
    counts = {}
    man = {}
    man_path = Path(bab_dir).parent / "manifest.json"
    if man_path.exists():
        for k, v in json.loads(man_path.read_text()).get("dbs", {}).items():
            man[v.get("source", "").split("::")[0]] = v

    # ---- BAB TSVs -------------------------------------------------------
    for stem, source, platform, fmt in BAB_SOURCES:
        path = Path(bab_dir) / f"{stem}.tsv"
        if not path.exists():
            continue
        tsv_sha = hashlib.sha256(path.read_bytes()).hexdigest()
        origin = (f"{BAB_ORIGIN}/download/gb_ongaku_v20220606.zip"
                  if fmt == "gbs2" else BAB_ORIGIN) + f"!{stem}.tsv[sha256:{tsv_sha[:16]}]"
        lines = path.read_text(encoding="utf-8").splitlines()
        # some TSVs carry a version-stamp banner line before the column header
        hi = next((k for k, l in enumerate(lines)
                   if l.split("\t")[0].strip() in ("No", "RIP", "RIPING", "No ")
                   or re.match(r"^No\b", l)), 0)
        header = lines[hi].split("\t")
        stamp = lines[0].split("\t")[0].strip()
        n = 0
        for row_no, line in enumerate(lines[hi + 1:], 1):
            if not re.match(r"^\d+\t", line):
                continue  # sub-headers, blanks
            vals = line.split("\t")
            vals += [""] * (len(header) - len(vals))
            cols = dict(zip([h.strip() for h in header], vals))
            proj = list(make_projection(source, platform, fmt, row_no, cols, line, origin))
            proj[23:23] = _rom_entry_points(proj)   # init_rom, play_rom after tma/tac
            con.execute(
                f"INSERT INTO entry({','.join(ENTRY_COLS)}) "
                f"VALUES({','.join('?' * len(ENTRY_COLS))})", proj)
            n += 1
        zsha = man.get(source, {}).get("sha256")
        con.execute("INSERT INTO source VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                    (source, platform, fmt, str(path), stamp, n, lines[hi],
                     f"{BAB_ORIGIN} (freeware, usage personnel)",
                     zsha, tsv_sha, f"bab_fetch.py + music_index.py (tsv {stem})",
                     "redistribution interdite; oracle local uniquement"))
        counts[source] = n

    # ---- generic rip collections (libgme-family formats) -----------------
    n = 0
    for root, origin_root in rip_dirs:
        root = Path(root)
        if not root.exists():
            continue
        for path in sorted(p for p in root.rglob("*")
                           if p.is_file() and p.suffix.lower() in RIP_EXTS):
            g = rip_parse(path)
            if not g:
                continue
            title = g.get("title")
            rel = path.relative_to(root)
            is_joshw = "joshw" in str(origin_root)
            origin = (f"joshw:gbs.joshw.info!{rel}" if is_joshw
                      else f"local:{origin_root}!{rel}")
            # verified (rip, dump) pairs — the `.verified` sidecar deepeel --gbs
            # writes when its byte-verify maps the rip into a dump (dump CRC32,
            # rip bank -> ROM bank, init/play in ROM space).  This is the same
            # knowledge the BAB rows carry, discovered by the tool instead of a
            # database: it turns the crc tier on for the verified dumps.  Only a
            # sidecar whose load/data_size match the parsed rip is trusted.
            ver = _read_verified(Path(str(path) + ".verified"), g.get("load"), g["data_size"])
            vcrcs = [f"{d['crc']:08x}" for d in ver]   # hex strings, like the BAB rows
            v0 = ver[0] if ver else None
            vchunks = []
            if v0:
                load = g.get("load") or 0
                for i, rb in enumerate(v0["banks"]):
                    if rb is None:
                        continue
                    s = max(0, i * 0x4000 - load)
                    e = min(g["data_size"], (i + 1) * 0x4000 - load)
                    if e > s:
                        vchunks.append([rb * 0x4000 + load - i * 0x4000, s, e - s])
            proj = ("joshw_gbs" if is_joshw else "local_rips",
                    "GB" if g["format"] == "gbs" else "",
                    g["format"], None,
                    title, norm_name(path.parent.name) or norm_name(title),
                    norm_name(re.sub(r"[\[(（]?\s*[Vv]?\d+(\.\d+)*\s*[\])）]?", "", title or "")),
                    None, None, gbs_prod_code(path.name),
                    json.dumps(vcrcs) if vcrcs else None, vcrcs[0] if vcrcs else None,
                    g.get("songs"), None, None,
                    g.get("load"), None, None, g.get("init"), g.get("play"), g.get("stack"),
                    g.get("tma"), g.get("tac"),
                    v0["init_rom"] if v0 else None, v0["play_rom"] if v0 else None,
                    None, None, None,
                    json.dumps(vchunks) if vchunks else None, None, None, None, None,
                    g["sha256"], g["data_size"], json.dumps(g, ensure_ascii=False),
                    origin, path.name, json.dumps({"dir": str(path.parent), "file": path.name}))
            con.execute(
                f"INSERT INTO entry({','.join(ENTRY_COLS)}) "
                f"VALUES({','.join('?' * len(ENTRY_COLS))})", proj)
            n += 1
    con.execute("INSERT INTO source VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                ("rips", "", "gbs/nsf/kss/...", str([str(r) for r, _ in rip_dirs]),
                 None, n, None,
                 "joshw:gbs.joshw.info + local", None, None,
                 "deepeel --gbs-fetch + music_index.py",
                 "rips de la communauté hcs64 (JoshW) — la musique reste © ses éditeurs "
                 "d'origine, aucune licence de redistribution : bibliothèque locale "
                 "uniquement (output/), hors dépôt, comme les bases BAB"))
    counts["rips"] = n

    # ---- precomputed cross-source links ---------------------------------
    a = con.execute(
        "SELECT e1.id a, e2.id b FROM entry e1 JOIN entry e2 "
        "ON e1.product_code = e2.product_code AND e1.product_code IS NOT NULL "
        "AND e1.source LIKE 'bab%' AND e2.source NOT LIKE 'bab%'").fetchall()
    con.executemany("INSERT OR IGNORE INTO cross_ref VALUES(?,?,'product_code')", a)
    b = con.execute(
        "SELECT e1.id a, e2.id b FROM entry e1 JOIN entry e2 "
        "ON e1.title_norm = e2.title_norm AND length(e1.title_norm) >= 4 "
        "AND e1.source LIKE 'bab%' AND e2.source NOT LIKE 'bab%'").fetchall()
    con.executemany("INSERT OR IGNORE INTO cross_ref VALUES(?,?,'title_norm')", b)
    g = con.execute(
        "SELECT e1.id a, e2.id b FROM entry e1 JOIN entry e2 "
        "ON e1.game_norm = e2.game_norm AND length(e1.game_norm) >= 4 "
        "AND e1.id < e2.id").fetchall()
    con.executemany("INSERT OR IGNORE INTO cross_ref VALUES(?,?,'game')", g)
    # within BAB: the same game appears in several DBs (kss vs sgc, 2010 vs 2022)
    # sharing the same first CRC -> precomputed cross-format links
    c = con.execute(
        "SELECT e1.id a, e2.id b FROM entry e1 JOIN entry e2 "
        "ON e1.crc1 = e2.crc1 AND e1.crc1 IS NOT NULL AND e1.source < e2.source").fetchall()
    con.executemany("INSERT OR IGNORE INTO cross_ref VALUES(?,?,'crc')", c)

    # ---- game identity: connected components over every link -------------
    # CRC keys one dump; a game spans dumps (regions, revisions) and formats.
    # Links: precomputed cross_ref (crc / product_code / title_norm) + optional
    # alias file (--alias: "link KEY_A KEY_B" lines or "alias KEY crc=HEX").
    parent = {}

    def find(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)

    for r in con.execute("SELECT a, b FROM cross_ref"):
        union(r["a"], r["b"])
    for path in alias_files:
        for ln in Path(path).read_text(encoding="utf-8").splitlines():
            f = ln.split()
            if not f or f[0].startswith("#"):
                continue
            if f[0] == "link":          # link ID_A ID_B (entry ids)
                union(int(f[1]), int(f[2]))
            elif f[0] == "alias":       # alias NAME crc=HEX [crc=HEX...]
                raise SystemExit("alias: resolve keys to ids first (not supported)")
    for r in con.execute("SELECT id FROM entry"):
        find(r["id"])
    for eid, root in parent.items():
        con.execute("UPDATE entry SET game_key=? WHERE id=?", (root, eid))

    con.execute("INSERT INTO meta VALUES('schema_version',?)", (str(SCHEMA_VERSION),))
    con.execute("INSERT INTO meta VALUES('index_version',?)", (INDEX_VERSION,))
    con.execute("INSERT INTO meta VALUES('built_at',?)",
                (datetime.now(timezone.utc).isoformat(timespec="seconds"),))
    con.execute("INSERT INTO meta VALUES('bab_manifest',?)",
                (json.dumps(man, ensure_ascii=False),))
    con.execute("INSERT INTO meta VALUES('license',?)",
                ("BAB: freeware, usage personnel, redistribution interdite. "
                 "Oracle de validation local uniquement; toute connaissance entrant "
                 "dans le depot doit etre re-derivee par deepeel.",))
    con.commit()
    return counts


# ------------------------------------------------------------------ output


def show(con, rows, verbose=False):
    for r in rows:
        print(f"[{r['id']}] {r['source']} {r['platform']}/{r['format']}  "
              f"{r['title']!r}  {r['year'] or ''} {r['maker'] or ''}")
        keys = []
        if r["crcs"] and r["crcs"] != "null":
            keys.append(f"crc={r['crcs']}")
        if r["product_code"]:
            keys.append(f"code={r['product_code']}")
        if r["title_norm"]:
            keys.append(f"norm={r['title_norm']}")
        print(f"     {' '.join(keys)}")
        eng = []
        for k in ("load", "init", "play", "stack", "tma", "tac", "songs", "s_addr"):
            if r[k] is not None:
                eng.append(f"{k}=${r[k]:X}" if k != "songs" else f"{k}={r[k]}")
        if r["s_addr_hi"] is not None:
            eng.append(f"(s_addr brut ${r['s_addr']:X} = hi ${r['s_addr_hi']:X} / "
                       f"lo ${r['s_addr_lo']:X})")
        print(f"     moteur: {' '.join(eng)}")
        ch = json.loads(r["chunks"] or "[]")
        if ch:
            print(f"     chunks: {' '.join(
            f'C ${t[0]:X},${t[1]:X},${t[2]:X} (rom,dest,len)' if len(t) == 3 else
            f'ROM ${t[0]:X}->${t[1]:X} - ${t[2]:X}->${t[3]:X} (2010)' for t in ch)}")
        pa = json.loads(r["patches"] or "[]")
        if pa:
            print(f"     patches: {len(pa)} plage(s)")
        if r["init_rom"] is not None or r["play_rom"] is not None:
            print(f"     rom: init=${r['init_rom'] or 0:X} play=${r['play_rom'] or 0:X}"
                  f" (projeté par chunks)" if r["init_rom"] is not None else "")
        if r["origin"]:
            print(f"     provenance: {r['origin']}")
        v = con.execute("SELECT * FROM verify WHERE entry_id=?", (r["id"],)).fetchall()
        for vv in v:
            print(f"     verify: {vv['method']} init=bank{vv['init_bank']} "
                  f"${vv['init_addr']:04X} play=bank{vv['play_bank']} "
                  f"${vv['play_addr']:04X} frag {vv['frag_matched']}/{vv['frag_total']} "
                  f"rom_crc={vv['rom_crc32']}")
        if verbose:
            print(f"     raw: {r['raw'][:200]}")


# -------------------------------------------------------------- validation


def validate(db, rom_dirs):
    con = connect(db)
    roms = []
    for d in rom_dirs:
        roms += [p for p in Path(d).rglob("*") if p.suffix.lower() in (".gb", ".gbc")]
    hits = miss = 0
    print(f"== {len(roms)} ROM(s) GB/GBC ==")
    for p in sorted(roms):
        data = p.read_bytes()
        if len(data) < 0x150:
            continue
        crc = f"{zlib.crc32(data):08X}"
        rows = con.execute("SELECT * FROM entry WHERE crc1=?", (crc,)).fetchall()
        if not rows:
            miss += 1
            print(f"  ABSENT {crc} {p.name[:52]}")
            continue
        hits += 1
        rom_title = data[0x134:0x144].decode("ascii", "replace").strip().strip("\x00")
        for r in rows:
            verdict = "titre?"
            rn, tn = norm_name(rom_title), norm_name(r["title"])
            if rn and tn and (rn in tn or tn in rn):
                verdict = "titre ok"
            # structural checks against the real ROM
            ch = json.loads(r["chunks"] or "[]")
            bounds = all(a + b <= len(data) for a, b, *_ in ch) if ch else None
            # validated invariant: dest+len tiles the rip file (== fsize when full)
            fs = r["fsize"]
            tiles = all(d + l <= fs for _, d, l in ch) if ch and fs else None
            print(f"  TROUVE {crc} {p.name[:40]} -> [{r['source']}] {r['title'][:28]!r} "
                  f"{verdict}{'' if bounds is None else (' chunks ok' if bounds else ' CHUNKS HORS ROM')}"
                  f"{'' if tiles is None else (' tuiles ok' if tiles else ' TUILES HORS F-SIZE')}")
    # cross-source agreement of the entry points on linked entries (crc kind)
    agree = disagree = 0
    for r in con.execute(
            "SELECT e1.source s1, e2.source s2, e1.init i1, e2.init i2, e1.play p1, e2.play p2 "
            "FROM cross_ref x JOIN entry e1 ON e1.id=x.a JOIN entry e2 ON e2.id=x.b "
            "WHERE x.kind='crc' AND e1.play IS NOT NULL AND e2.play IS NOT NULL"):
        agree += r["p1"] == r["p2"]
        disagree += r["p1"] != r["p2"]
    print(f"== accords inter-sources sur play (paires CRC): {agree} d'accord, {disagree} en désaccord")
    print(f"\n== cohérence interne Σchunks=F-SIZE (tout l'index): {ok}/{tot}")
    print(f"== ROMs: {hits} touchées, {miss} absentes (dumps US/EU hors CRC japonais: attendu)")
    return 0


def write_binary(con, path):
    """Sorted binary index for C consumers (deepeel).

    Layout (little-endian, packed):
      char[8]  "DPMDX002"
      u32      n_crc, n_entry, n_chunk, n_str, pool_bytes, n_title, n_game
      crc index, sorted by crc32: n_crc * { u32 crc, u32 first, u32 count }
      entries: n_entry * {
        u32 game_key;                             // 0xFFFFFFFF = unlinked
        u32 src_off, platform_off, format_off;    // string pool offsets
        u16 load, init, play, stack;              // 0xFFFF = absent
        u8 tma, tac; u16 songs;
        u32 init_rom, play_rom;                   // 0xFFFFFFFF = absent
        u32 s_addr;                               // 0xFFFFFFFF = absent
        u32 chunk_first, u32 chunk_count;         // into the flat chunk array
        u32 raw_off, u32 columns_off, u32 origin_off; }  // string pool offsets
      chunks: n_chunk * { u32 rom, u32 dest, u32 len }
      title index, strcmp-sorted by the pool string:
        n_title * { u32 str_off, u32 first, u32 count }   (title_norm, ASCII, may be absent)
        + refs: sum(count) * u32 entry indices
      game index, sorted by game_key:
        n_game * { u32 key, u32 first, u32 count }        (0xFFFFFFFF unlinked, excluded)
        + refs: sum(count) * u32 entry indices
      pool: n_str null-terminated utf-8 strings (0 = empty)
    All fields little-endian; a C loader is a single fread + pointer walks."""
    M = 0xFFFFFFFF
    # the crc index stores (first, count) ranges: the entry array must be sorted
    # by crc (numeric), then id; crc-less entries trail in id order
    rows = con.execute("SELECT * FROM entry ORDER BY id").fetchall()
    keyed = [r for r in rows if r["crc1"] and re.fullmatch(r"[0-9A-Fa-f]{8}", r["crc1"])]
    keyed.sort(key=lambda r: (int(r["crc1"], 16), r["id"]))
    entries = keyed + [r for r in rows if r not in keyed]
    str_ids, str_idx = [], {}

    def sid(t):
        t = t or ""
        if t not in str_idx:
            str_idx[t] = len(str_ids)
            str_ids.append(t)
        return str_idx[t]

    def u16(v):
        return 0xFFFF if v is None else (v & 0xFFFF)

    def u32s(v):
        return M if v is None else (v & 0xFFFFFFFF)

    # crc index
    bycrc = {}
    for i, r in enumerate(entries):
        if r["crc1"] and re.fullmatch(r"[0-9A-Fa-f]{8}", r["crc1"]):
            bycrc.setdefault(r["crc1"].upper(), []).append(i)
    crcs = sorted(bycrc, key=lambda c: int(c, 16))  # numeric order: C binary search
    crc_index, first = [], 0
    for c in crcs:
        crc_index.append((int(c, 16), first, len(bycrc[c])))
        first += len(bycrc[c])
    chunks = []
    ent_rec = []
    for r in entries:
        ch = json.loads(r["chunks"] or "[]")
        cfirst = len(chunks)
        for t in ch:
            if len(t) == 3:
                chunks.append(tuple(t))
        ent_rec.append((
            u32s(r["game_key"]), sid(con.execute("SELECT name FROM source WHERE name=?",
                                                (r["source"],)).fetchone() and r["source"]),
            sid(r["platform"]), sid(r["format"]), 0,
            u16(r["load"]), u16(r["init"]), u16(r["play"]), u16(r["stack"]),
            (0xFF if r["tma"] is None else r["tma"] & 0xFF),
            (0xFF if r["tac"] is None else r["tac"] & 0xFF), u16(r["songs"]),
            u32s(r["init_rom"]), u32s(r["play_rom"]), u32s(r["s_addr"]),
            cfirst, len(chunks) - cfirst,
            sid(r["raw"]), sid(r["columns"]), sid(r["origin"]),
        ))
    import struct as _s
    # title index: strcmp order of the pool string (C binary search); ASCII norms only
    bytitle = {}
    for i, r in enumerate(entries):
        t = (r["title_norm"] or "").strip()
        if t:
            sid(t)   # make sure the norm is in the pool (offs below needs it)
            bytitle.setdefault(t, []).append(i)
    titles = sorted(bytitle, key=lambda t: t.encode("utf-8"))
    # game index: identity expansion (one game = all its dumps/formats/regions)
    bygame = {}
    for i, r in enumerate(entries):
        if r["game_key"] is not None:
            bygame.setdefault(r["game_key"], []).append(i)
    games = sorted(bygame)
    pool = bytearray()
    offs = {}
    for t in str_ids:
        offs[t] = len(pool)
        pool += t.encode("utf-8") + b"\0"
    out = bytearray()
    out += b"DPMDX002"
    out += _s.pack("<IIIIIII", len(crcs), len(entries), len(chunks), len(str_ids), len(pool),
                   len(titles), len(games))

    for c, f, n in crc_index:
        out += _s.pack("<III", c, f, n)
    for e in ent_rec:
        out += _s.pack("<IIII", e[0],
                       offs[str_ids[e[1]]], offs[str_ids[e[2]]], offs[str_ids[e[3]]])
        out += _s.pack("<HHHH", e[5], e[6], e[7], e[8])
        out += _s.pack("<BBH", e[9], e[10], e[11])
        out += _s.pack("<III", e[12], e[13], e[14])
        out += _s.pack("<II", e[15], e[16])
        out += _s.pack("<III", offs[str_ids[e[17]]], offs[str_ids[e[18]]],
                       offs[str_ids[e[19]]])
    for rom, dest, ln in chunks:
        out += _s.pack("<III", rom, dest, ln)
    title_refs = []
    for t in titles:
        out += _s.pack("<III", offs[t], len(title_refs), len(bytitle[t]))
        title_refs += bytitle[t]
    for i in title_refs:
        out += _s.pack("<I", i)
    game_refs = []
    for k in games:
        out += _s.pack("<III", k, len(game_refs), len(bygame[k]))
        game_refs += bygame[k]
    for i in game_refs:
        out += _s.pack("<I", i)
    out += pool
    path.write_bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--bab", default="output/bab/db")
    b.add_argument("--rip-dirs", nargs="*", default=["output/gbs", "output/joshw"])
    b.add_argument("--db", default="output/music_index/music_index.sqlite3")
    b.add_argument("--alias", action="append", default=[],
                   help="alias file: 'link ID_A ID_B' lines joining entry ids")
    q = sub.add_parser("query")
    q.add_argument("name", nargs="?", help="name substring (normalized)")
    q.add_argument("--crc", help="first ROM CRC (hex)")
    q.add_argument("--code", help="product code")
    q.add_argument("--source", help="filter by source (bab_gbs2, joshw_gbs, ...)")
    q.add_argument("--db", default="output/music_index/music_index.sqlite3")
    q.add_argument("-v", "--verbose", action="store_true")
    v = sub.add_parser("validate")
    v.add_argument("--db", default="output/music_index/music_index.sqlite3")
    v.add_argument("--roms", nargs="+", required=True)
    s = sub.add_parser("stats")
    s.add_argument("--db", default="output/music_index/music_index.sqlite3")
    e = sub.add_parser("export")
    e.add_argument("--db", default="output/music_index/music_index.sqlite3")
    e.add_argument("--out", default="output/music_index/music_index.json")
    e.add_argument("--bin", dest="binout", help="also write the sorted binary index for C")
    args = ap.parse_args()

    if args.cmd == "build":
        out = Path(args.db)
        out.parent.mkdir(parents=True, exist_ok=True)
        if out.exists():
            out.unlink()
        rip_dirs = [(d, d) for d in args.rip_dirs]
        counts = build(args.bab, rip_dirs, out, args.alias)
        con = connect(out)
        con.execute("ANALYZE")
        con.commit()
        for k, v in sorted(counts.items()):
            print(f"  {k}: {v}")
        print(f"index: {out} ({out.stat().st_size // 1024} Kio)")
        return 0

    con = connect(args.db)
    if args.cmd == "stats":
        for r in con.execute("SELECT name, platform, format, rows FROM source ORDER BY name"):
            print(f"  {r['name']:<20} {r['platform']:<5} {r['format']:<12} {r['rows']}")
        n = con.execute("SELECT count(*) c FROM cross_ref").fetchone()["c"]
        v = con.execute("SELECT count(*) c FROM verify").fetchone()["c"]
        print(f"  cross_ref: {n}  verify: {v}")
        return 0

    if args.cmd == "validate":
        return validate(args.db, args.roms)

    if args.cmd == "export":
        data = {"schema_version": SCHEMA_VERSION, "index_version": INDEX_VERSION,
                "meta": {r["key"]: r["value"] for r in con.execute("SELECT * FROM meta")},
                "sources": [dict(r) for r in con.execute("SELECT * FROM source")],
                "entries": [dict(r) for r in con.execute("SELECT * FROM entry")],
                "cross_ref": [dict(r) for r in con.execute("SELECT * FROM cross_ref")],
                "verify": [dict(r) for r in con.execute("SELECT * FROM verify")]}
        out = Path(args.out)
        out.write_text(json.dumps(data, ensure_ascii=False, indent=1), encoding="utf-8")
        print(f"export: {out} ({out.stat().st_size // 1024} Kio)")
        if args.binout:
            bp = Path(args.binout)
            write_binary(con, bp)
            print(f"binaire: {bp} ({bp.stat().st_size // 1024} Kio)")
        return 0

    # query
    where, params = [], []
    if args.crc:
        where.append("crc1 LIKE ? OR crcs LIKE ?")
        params += [args.crc.upper(), f"%{args.crc.upper()}%"]
    if args.code:
        where.append("product_code = ?")
        params.append(args.code.upper())
    if args.source:
        where.append("source = ?")
        params.append(args.source)
    if args.name:
        where.append("(title_norm LIKE ? OR title LIKE ?)")
        params += [f"%{norm_name(args.name)}%", f"%{args.name}%"]
    if not where:
        ap.error("donnez un nom, --crc ou --code")
    sql = "SELECT * FROM entry WHERE " + " AND ".join(where) + " ORDER BY source, id"
    rows = con.execute(sql, params).fetchall()
    ids = [r["id"] for r in rows]
    xref = {}
    if ids:
        q = ",".join("?" * len(ids))
        for r in con.execute(
                f"SELECT x.*, e.source, e.title FROM cross_ref x JOIN entry e "
                f"ON e.id = CASE WHEN x.a IN ({q}) THEN x.b ELSE x.a END "
                f"WHERE x.a IN ({q}) OR x.b IN ({q})",
                ids + ids + ids):
            xref.setdefault(r["a"] if r["a"] in ids else r["b"], []).append(
                f"{r['source']}:{r['title']}")
    for r in rows:
        show(con, [r], args.verbose)
        for x in xref.get(r["id"], []):
            print(f"     croise: {x}")
    print(f"-- {len(rows)} entrée(s)")


if __name__ == "__main__":
    sys.exit(main())
