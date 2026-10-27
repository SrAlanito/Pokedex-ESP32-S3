#!/usr/bin/env python3
"""Pack generations 1-4 for the offline Pokédex, and the Wonder Mail tables.

Network is used only when data/cache is missing. The device never calls out.
"""

import json
import re
import struct
import time
import unicodedata
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

try:
    Import("env")  # noqa: F821  PlatformIO injects this when the script is a pre-build step.
    ROOT = Path(env["PROJECT_DIR"])  # noqa: F821
    _FROM_SCONS = True
except Exception:
    ROOT = Path(__file__).resolve().parent.parent
    _FROM_SCONS = False
CACHE = ROOT / "data" / "cache"
REF = ROOT / "reference"
COUNT = 493
SIDE = 216
JPEG_Q = 88
PAPER = (244, 240, 232)
KEY = 0xF81F
UA = {"User-Agent": "pokedex-esp32-prepare"}

TYPE_ID = {
    "normal": 1,
    "fire": 2,
    "water": 3,
    "electric": 4,
    "grass": 5,
    "ice": 6,
    "fighting": 7,
    "poison": 8,
    "ground": 9,
    "flying": 10,
    "psychic": 11,
    "bug": 12,
    "rock": 13,
    "ghost": 14,
    "dragon": 15,
    "dark": 16,
    "steel": 17,
}


def fetch(url, dest):
    if dest.exists() and dest.stat().st_size > 0:
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers=UA)
    last = None
    for attempt in range(6):
        try:
            with urllib.request.urlopen(req, timeout=40) as resp:
                dest.write_bytes(resp.read())
            return True
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                return False
            last = exc
        except Exception as exc:
            last = exc
        time.sleep(1.2 * (attempt + 1))
    raise SystemExit("failed %s (%s)" % (url, last))


def ascii_name(text):
    text = (
        text.replace("♀", "-H")
        .replace("♂", "-M")
        .replace("\u2019", "'")
        .replace("\u2018", "'")
        .replace("é", "e")
        .replace("É", "E")
    )
    text = unicodedata.normalize("NFKD", text)
    out = []
    for ch in text:
        if unicodedata.combining(ch):
            continue
        if 32 <= ord(ch) < 127:
            out.append(ch)
    cleaned = "".join(out).strip()
    return cleaned or "?"


def fold(text):
    return re.sub(r"[^a-z0-9]", "", ascii_name(text).lower())


def js_strings(path, var_name):
    text = path.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"var\s+%s\s*=\s*\[(.*?)\];" % var_name, text, re.S)
    if not match:
        raise SystemExit("missing var %s in %s" % (var_name, path))
    return re.findall(r'"([^"]*)"', match.group(1))


def hex_bytes(text):
    return bytes(int(text[i : i + 2], 16) for i in range(0, len(text), 2))


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_number(name):
    return {
        "generation-i": 1,
        "generation-ii": 2,
        "generation-iii": 3,
        "generation-iv": 4,
        "generation-v": 5,
        "generation-vi": 6,
        "generation-vii": 7,
        "generation-viii": 8,
        "generation-ix": 9,
    }[name]


def types_for_gen4(data):
    chosen = data["types"]
    covering = []
    for row in data.get("past_types") or []:
        gen = gen_number(row["generation"]["name"])
        if gen >= 4:
            covering.append((gen, row["types"]))
    if covering:
        covering.sort(key=lambda item: item[0])
        chosen = covering[0][1]
    ordered = sorted(chosen, key=lambda row: row["slot"])
    ids = [TYPE_ID[row["type"]["name"]] for row in ordered]
    return ids[0], ids[1] if len(ids) > 1 else 0


def rgb565(r, g, b):
    value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    if value == KEY:
        value = KEY - 1
    return value


def render_art(path):
    from io import BytesIO

    from PIL import Image

    img = Image.open(path).convert("RGBA")
    bbox = img.getchannel("A").point(lambda a: 255 if a > 12 else 0).getbbox()
    if bbox:
        img = img.crop(bbox)
    inner = SIDE - 8
    scale = inner / float(max(img.size))
    nw = max(1, int(round(img.size[0] * scale)))
    nh = max(1, int(round(img.size[1] * scale)))
    if img.size != (nw, nh):
        img = img.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (SIDE, SIDE), (0, 0, 0, 0))
    canvas.alpha_composite(img, ((SIDE - nw) // 2, (SIDE - nh) // 2))
    alpha = canvas.getchannel("A").tobytes()
    mask = bytearray(SIDE * SIDE // 8)
    for i, a in enumerate(alpha):
        if a > 28:
            mask[i >> 3] |= 0x80 >> (i & 7)
    paper = Image.new("RGBA", (SIDE, SIDE), PAPER + (255,))
    flat = Image.alpha_composite(paper, canvas).convert("RGB")
    buf = BytesIO()
    flat.save(buf, format="JPEG", quality=JPEG_Q, subsampling=0, optimize=True)
    return buf.getvalue(), bytes(mask)


def cached_art(n, src):
    folder = CACHE / ("art%d" % SIDE)
    jpg = folder / ("%d.jpg" % n)
    msk = folder / ("%d.msk" % n)
    mask_bytes = SIDE * SIDE // 8
    fresh = (
        jpg.exists()
        and msk.exists()
        and jpg.stat().st_size > 100
        and msk.stat().st_size == mask_bytes
        and jpg.stat().st_mtime >= src.stat().st_mtime
    )
    if fresh:
        return jpg.read_bytes(), msk.read_bytes()
    jpeg, mask = render_art(src)
    folder.mkdir(parents=True, exist_ok=True)
    jpg.write_bytes(jpeg)
    msk.write_bytes(mask)
    return jpeg, mask


def download_all():
    jobs = []
    for n in range(1, COUNT + 1):
        jobs.append(
            (
                "https://pokeapi.co/api/v2/pokemon/%d" % n,
                CACHE / "pokemon" / ("%d.json" % n),
            )
        )
        jobs.append(
            (
                "https://pokeapi.co/api/v2/pokemon-species/%d" % n,
                CACHE / "species" / ("%d.json" % n),
            )
        )
        jobs.append(
            (
                "https://raw.githubusercontent.com/PokeAPI/sprites/master/sprites/pokemon/other/official-artwork/%d.png" % n,
                CACHE / "official" / ("%d.png" % n),
            )
        )
    missing = [(url, dest) for url, dest in jobs if not (dest.exists() and dest.stat().st_size > 0)]
    print("cache hits %d, fetching %d" % (len(jobs) - len(missing), len(missing)), flush=True)
    if not missing:
        return
    done = 0
    with ThreadPoolExecutor(max_workers=6) as pool:
        futures = {pool.submit(fetch, url, dest): dest.name for url, dest in missing}
        for fut in as_completed(futures):
            fut.result()
            done += 1
            if done % 50 == 0 or done == len(missing):
                print("fetched %d/%d" % (done, len(missing)), flush=True)


def load_species_names(n):
    data = json.loads((CACHE / "species" / ("%d.json" % n)).read_text(encoding="utf-8"))
    names = {row["language"]["name"]: row["name"] for row in data["names"]}
    en = ascii_name(names.get("en") or data["name"])
    es = ascii_name(names.get("es") or en)
    return es, en


def load_mon(n):
    data = json.loads((CACHE / "pokemon" / ("%d.json" % n)).read_text(encoding="utf-8"))
    t1, t2 = types_for_gen4(data)
    by_name = {row["stat"]["name"]: row["base_stat"] for row in data["stats"]}
    order = ["hp", "attack", "defense", "special-attack", "special-defense", "speed"]
    stats = [min(255, int(by_name[name])) for name in order]
    es, en = load_species_names(n)
    official = CACHE / "official" / ("%d.png" % n)
    if not official.exists() or official.stat().st_size < 32:
        url = ((data.get("sprites") or {}).get("other") or {}).get("official-artwork", {})
        url = (url or {}).get("front_default")
        if not url or not fetch(url, official):
            official = CACHE / "front" / ("%d.png" % n)
            if not official.exists():
                raise SystemExit("no artwork for #%d" % n)
            print("front fallback #%d" % n, flush=True)
    jpeg, mask = cached_art(n, official)
    return es, en, t1, t2, stats, jpeg, mask


def write_dex(entries):
    names = bytearray()
    offs = []
    for es, en, *_ in entries:
        offs.append((len(names),))
        names += es.encode("ascii") + b"\0"
        offs[-1] = (offs[-1][0], len(names))
        names += en.encode("ascii") + b"\0"
    if len(names) & 1:
        names += b"\0"
    index = bytearray()
    payload = bytearray()
    mask_bytes = SIDE * SIDE // 8
    jpeg_total = 0
    for *_, jpeg, mask in entries:
        if len(mask) != mask_bytes:
            raise SystemExit("mask size %d" % len(mask))
        index += struct.pack("<II", len(payload), len(jpeg))
        payload += jpeg
        payload += mask
        jpeg_total += len(jpeg)
    blob = bytearray()
    blob += struct.pack("<IHHII", 0x32584544, COUNT, SIDE, len(names), mask_bytes)
    for i, (es_off, en_off) in enumerate(offs):
        _, _, t1, t2, stats, _, _ = entries[i]
        blob += struct.pack("<HHBB6B", es_off, en_off, t1, t2, *stats)
    blob += names
    blob += index
    blob += payload
    dest = ROOT / "data" / "dex.bin"
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(blob)
    print(
        "wrote %s (%d bytes, jpeg %.1f KB avg)"
        % (dest, len(blob), jpeg_total / COUNT / 1024.0),
        flush=True,
    )


def bad_set(text, width):
    raw = hex_bytes(text)
    out = set()
    step = 1 if width == 8 else 2
    for i in range(0, len(raw) - step + 1, step):
        if width == 8:
            out.add(raw[i])
        else:
            out.add(raw[i] | (raw[i + 1] << 8))
    return out


def species_alias(species):
    if species in (0x179, 0x17A, 0x17B):
        return 0x178
    if (0xCA <= species <= 0xE2) or species in (0x19F, 0x1A0):
        return 201
    if species in (0x1A1, 0x1A2, 0x1A3):
        return 0x19E
    if species == 0x1A7:
        return 0x19C
    return species


def write_mail(national_en, national_es):
    html_path = REF / "PMD RT Wondermail Generator.html"
    dest = ROOT / "src" / "mail_data.h"
    if not html_path.exists():
        if dest.exists():
            print("mail source absent, keeping %s" % dest, flush=True)
            return
        raise SystemExit("missing Wonder Mail source and %s" % dest)
    html = html_path.read_text(encoding="utf-8", errors="replace")

    def grab(name):
        match = re.search(
            r'var\s+%s\s*=\s*((?:"[0-9A-Fa-f]+"\s*\+?\s*)+)' % name,
            html,
        )
        if not match:
            raise SystemExit("missing %s" % name)
        return "".join(re.findall(r'"([0-9A-Fa-f]+)"', match.group(1)))

    bad_items = bad_set(grab("baditems"), 8)
    bad_dungeons = bad_set(grab("baddungeons"), 8)
    bad_pokemon = bad_set(grab("badpokemon"), 16)
    floors = hex_bytes(grab("floors"))
    items = js_strings(REF / "PMD RT Wondermail Generator_files" / "items-en.js", "items")
    dungeons = js_strings(REF / "PMD RT Wondermail Generator_files" / "items-en.js", "dungeons")
    pokemon = js_strings(REF / "PMD RT Wondermail Generator_files" / "pokemon-en.js", "pokemon")

    dun_rows = []
    for index, name in enumerate(dungeons):
        if index > 0x3F or index in bad_dungeons:
            continue
        if index >= len(floors) or floors[index] < 2:
            continue
        dun_rows.append((index, floors[index], ascii_name(name)))

    item_rows = []
    for index, name in enumerate(items):
        if index == 0 or index >= 0xF0 or index in bad_items:
            continue
        item_rows.append((index, ascii_name(name)))

    clients = []
    for index, name in enumerate(pokemon):
        if index == 0 or name.startswith("?"):
            continue
        if species_alias(index) != index or index in bad_pokemon:
            continue
        shown = ascii_name(name)
        if 1 <= index <= COUNT and fold(national_en.get(index, "")) == fold(name):
            shown = national_es.get(index, shown)
        clients.append((index, shown))

    if not dun_rows or not item_rows or not clients:
        raise SystemExit("wonder mail tables came out empty")

    lines = [
        "#pragma once",
        "#include <stdint.h>",
        "// Rescue Team dungeon, reward, and client tables. Generated by tools/prepare_dex.py.",
        "struct MailDun { uint8_t id; uint8_t floors; const char *name; };",
        "struct MailItem { uint16_t id; const char *name; };",
        "struct MailClient { uint16_t id; const char *name; };",
        "static const MailDun kMailDungeons[] = {",
    ]
    for did, count, name in dun_rows:
        lines.append("    {%d, %d, %s}," % (did, count, c_string(name)))
    lines.append("};")
    lines.append("static const int kMailDungeonCount = %d;" % len(dun_rows))
    lines.append("static const MailItem kMailItems[] = {")
    for iid, name in item_rows:
        lines.append("    {%d, %s}," % (iid, c_string(name)))
    lines.append("};")
    lines.append("static const int kMailItemCount = %d;" % len(item_rows))
    lines.append("static const MailClient kMailClients[] = {")
    for cid, name in clients:
        lines.append("    {%d, %s}," % (cid, c_string(name)))
    lines.append("};")
    lines.append("static const int kMailClientCount = %d;" % len(clients))
    lines.append("")
    dest.write_text("\n".join(lines), encoding="utf-8")
    print(
        "wrote %s (%d dungeons, %d items, %d clients)"
        % (dest, len(dun_rows), len(item_rows), len(clients)),
        flush=True,
    )


def main():
    download_all()
    entries = []
    national_en = {}
    national_es = {}
    for n in range(1, COUNT + 1):
        es, en, t1, t2, stats, jpeg, mask = load_mon(n)
        entries.append((es, en, t1, t2, stats, jpeg, mask))
        national_en[n] = en
        national_es[n] = es
        if n % 50 == 0 or n == COUNT:
            print("packed %d/%d" % (n, COUNT), flush=True)
    write_dex(entries)
    write_mail(national_en, national_es)


if _FROM_SCONS or __name__ == "__main__":
    main()
