#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Packs data/symbols/icons/*.png into a single data/symbols/icons.dat file
# (header + index + concatenated bytes), because LittleFS allocates a full
# 4KB block per file and our 128KB partition only has 32 blocks - not
# enough for 189 separate tiny files. Packed into one file, the whole set
# uses ~15 blocks instead of ~190.
#
# Format (little-endian):
#   4 bytes  magic "ICPK"
#   2 bytes  uint16 count
#   count *  { 12 bytes name (null-padded), 4 bytes uint32 offset, 4 bytes uint32 length }
#   then the concatenated raw file bytes (offset is relative to the start of this data section)
#
# Run from the project root (same folder as data/symbols/icons/).
import os
import struct
import sys
import shutil

ICONS_DIR = "data/symbols/icons"
OUT_FILE = "data/symbols/icons.dat"
BACKUP_DIR = "tools/icon_sources"

if not os.path.isdir(ICONS_DIR):
    print(f"ERRO: pasta {ICONS_DIR} nao encontrada. Rode a partir da raiz do projeto.")
    sys.exit(1)

files = sorted(f for f in os.listdir(ICONS_DIR) if f.endswith(".png"))
if not files:
    print(f"ERRO: nenhum .png encontrado em {ICONS_DIR}.")
    sys.exit(1)

if len(files) > 65535:
    print("ERRO: mais de 65535 arquivos, formato nao suporta.")
    sys.exit(1)

for f in files:
    if len(f.encode("ascii")) > 11:
        print(f"ERRO: nome de arquivo muito longo (max 11 chars): {f}")
        sys.exit(1)

entries = []
blob = bytearray()
for f in files:
    path = os.path.join(ICONS_DIR, f)
    with open(path, "rb") as fh:
        data = fh.read()
    entries.append((f, len(blob), len(data)))
    blob += data

header = bytearray()
header += b"ICPK"
header += struct.pack("<H", len(entries))
for name, offset, length in entries:
    name_bytes = name.encode("ascii")
    name_padded = name_bytes + b"\x00" * (12 - len(name_bytes))
    header += name_padded
    header += struct.pack("<II", offset, length)

os.makedirs(os.path.dirname(OUT_FILE), exist_ok=True)
with open(OUT_FILE, "wb") as fh:
    fh.write(header)
    fh.write(blob)

total_size = len(header) + len(blob)
print(f"OK: {len(entries)} icones empacotados em {OUT_FILE} ({total_size} bytes: {len(header)} header + {len(blob)} dados)")

# Move the original individual files out of data/ so they are NOT included
# in the LittleFS image build (they'd blow the block budget again).
os.makedirs(BACKUP_DIR, exist_ok=True)
dest = os.path.join(BACKUP_DIR, "icons")
if os.path.exists(dest):
    shutil.rmtree(dest)
shutil.move(ICONS_DIR, dest)
print(f"OK: arquivos originais movidos para {dest}/ (fora de data/, nao entram mais na imagem do filesystem)")
print("Se precisar re-gerar o pacote no futuro (novos icones, etc), mova a pasta de volta para data/symbols/icons e rode este script de novo.")
