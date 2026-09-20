#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Replaces our icon set (currently sourced from aprs.nakhonthai.net) with the
# official aprs.fi symbol graphics (github.com/hessu/aprs-symbols, by Heikki
# Hannikainen OH7LZB - aprs.fi's creator - free to reuse with attribution,
# see COPYRIGHT.md in that repo), so the club sees the same icon style they're
# used to from the aprs.fi website.
#
# How this works:
# - aprs.fi publishes 2 sprite sheets (not individual files): one for the
#   "primary" symbol table (codes starting with "/") and one for the
#   "secondary" table (codes starting with "\"), each an 8-row x 16-column
#   grid of icons ordered by ASCII code (confirmed against their own
#   Illustrator export script and their symbols.csv index).
# - Our firmware names icon files "<ascii-code-of-symbol-char>-<1 or 2>.png"
#   (1=primary/"/" table, 2=secondary/"\" table) - confirmed by reading
#   src/webservice.cpp's icon-filename logic directly.
# - So: grid cell for ASCII code C is row=(C-33)//16, col=(C-33)%16, and we
#   save it as "<C>-<table>.png" to match what the firmware requests.
# - "dot.png" (the one fallback file that isn't part of the symbol table) is
#   left untouched - aprs.fi doesn't publish an equivalent, so we keep the
#   existing one.
#
# Run from the project root (same folder as data/symbols/, pack_icons.py):
#   python3 rebuild_icons_aprsfi.py
# Needs Pillow: pip3 install pillow --break-system-packages   (if not installed)
import os
import shutil
import sys
import urllib.request

try:
    from PIL import Image
except ImportError:
    print("ERRO: falta o Pillow. Rode: pip3 install pillow --break-system-packages")
    sys.exit(1)

ICONS_DIR = "data/symbols/icons"
BACKUP_OLD_DIR = "tools/icon_sources/icons_nakhonthai_backup"
# Previous runs of pack_icons.py moved the old per-file icons here - dot.png
# (the fallback, not part of the aprs.fi symbol table sprite) lives here now.
PREVIOUS_ICONS_DIR = "tools/icon_sources/icons"
SPRITE_URLS = {
    1: "https://raw.githubusercontent.com/hessu/aprs-symbols/master/png/aprs-symbols-24-0.png",  # primary table "/"
    2: "https://raw.githubusercontent.com/hessu/aprs-symbols/master/png/aprs-symbols-24-1.png",  # secondary table "\"
}
CELL_SIZE = 24
GRID_COLS = 16
# Rows are computed from the actual downloaded image size below - don't
# hardcode it, the sprite sheet turned out to be 6 rows (144px / 24px),
# not 8 as a leftover variable in aprs.fi's export script suggested.

if not os.path.isdir("data/symbols"):
    print("ERRO: rode este script a partir da raiz do projeto (pasta que contem data/symbols).")
    sys.exit(1)

# --- 1) Back up the current (nakhonthai-sourced) icons before replacing them ---
if os.path.isdir(ICONS_DIR):
    os.makedirs(os.path.dirname(BACKUP_OLD_DIR), exist_ok=True)
    if os.path.exists(BACKUP_OLD_DIR):
        shutil.rmtree(BACKUP_OLD_DIR)
    shutil.copytree(ICONS_DIR, BACKUP_OLD_DIR)
    print(f"OK: backup dos icones antigos (nakhonthai) salvo em {BACKUP_OLD_DIR}/")
    old_dot = os.path.join(ICONS_DIR, "dot.png")
    have_old_dot = os.path.exists(old_dot)
else:
    os.makedirs(ICONS_DIR, exist_ok=True)
    have_old_dot = False
    old_dot = None

# dot.png might not be in ICONS_DIR right now (a previous pack_icons.py run
# moves everything out of data/ after packing) - check the previous-run
# location too, and copy it back in before we start deleting/recreating files.
if not have_old_dot:
    prev_dot = os.path.join(PREVIOUS_ICONS_DIR, "dot.png")
    if os.path.exists(prev_dot):
        shutil.copy(prev_dot, os.path.join(ICONS_DIR, "dot.png"))
        have_old_dot = True
        print(f"OK: dot.png recuperado de {prev_dot} (ficou la da ultima vez que rodamos o pack_icons.py).")

# --- 2) Download the two aprs.fi sprite sheets ---
sprites = {}
for table, url in SPRITE_URLS.items():
    print(f"Baixando folha de simbolos (tabela {table}) de {url} ...")
    tmp_path = f"/tmp/aprs-symbols-{table}.png"
    try:
        urllib.request.urlretrieve(url, tmp_path)
    except Exception as e:
        print(f"ERRO: falha ao baixar {url}: {e}")
        sys.exit(1)
    img = Image.open(tmp_path).convert("RGBA")
    expected_w = CELL_SIZE * GRID_COLS
    if img.size[0] != expected_w:
        print(f"ERRO: largura inesperada da folha (tabela {table}): {img.size}, esperava largura {expected_w}. Abortando sem alterar nada.")
        sys.exit(1)
    if img.size[1] % CELL_SIZE != 0:
        print(f"ERRO: altura da folha (tabela {table}) nao e multiplo de {CELL_SIZE}px: {img.size}. Abortando sem alterar nada.")
        sys.exit(1)
    rows = img.size[1] // CELL_SIZE
    sprites[table] = (img, rows)
    print(f"OK: folha da tabela {table} baixada ({img.size[0]}x{img.size[1]}, {rows} linhas x {GRID_COLS} colunas).")

# --- 3) Slice out one PNG per ASCII code (33-126) per table, skip the old ones first ---
# Remove old per-file icons (keep dot.png for now, handled separately below)
for f in os.listdir(ICONS_DIR):
    if f != "dot.png":
        os.remove(os.path.join(ICONS_DIR, f))

extracted = 0
blank_count = 0
skipped_out_of_range = 0
for table, (sheet, rows) in sprites.items():
    for code in range(33, 127):
        if code == 124:  # "|" is not a real symbol slot in the aprs.fi index - skip
            continue
        idx = code - 33
        row = idx // GRID_COLS
        col = idx % GRID_COLS
        if row >= rows:
            skipped_out_of_range += 1
            continue
        x0 = col * CELL_SIZE
        y0 = row * CELL_SIZE
        cell = sheet.crop((x0, y0, x0 + CELL_SIZE, y0 + CELL_SIZE))
        # crude "is this cell empty" check via alpha channel sum, just for a sanity count printed below
        alpha_sum = sum(cell.getchannel("A").getdata())
        if alpha_sum == 0:
            blank_count += 1
        out_name = f"{code}-{table}.png"
        cell.save(os.path.join(ICONS_DIR, out_name))
        extracted += 1

print(f"OK: {extracted} icones recortados e salvos em {ICONS_DIR}/ ({blank_count} vieram totalmente transparentes, {skipped_out_of_range} ficaram fora da grade - ambos normais, nem todo codigo ASCII tem simbolo definido).")

# --- 4) Keep the existing dot.png fallback (aprs.fi doesn't publish an equivalent) ---
if have_old_dot:
    print("OK: dot.png (fallback) mantido como estava - o aprs.fi nao publica um equivalente.")
else:
    print("AVISO: nao havia dot.png anterior para manter. O firmware usa esse nome como ultimo fallback - considere adicionar um manualmente se notar icones faltando.")

print("\nProximo passo: rode 'python3 pack_icons.py' para empacotar esses novos icones em data/symbols/icons.dat")
print("(isso tambem move os PNGs individuais para fora de data/, como da ultima vez).")
