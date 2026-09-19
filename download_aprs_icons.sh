#!/bin/bash
# Baixa o conjunto de icones APRS usados pelo Dashboard (aprs.nakhonthai.net)
# para dentro da pasta data/symbols/icons/ do projeto, para embutir no LittleFS
# e o Dashboard funcionar sem depender de internet.
#
# Uso: rode este script dentro da pasta do projeto (~/dev/ESP32APRS_Audio)
#   chmod +x download_aprs_icons.sh
#   ./download_aprs_icons.sh

set -e
DEST="data/symbols/icons"
mkdir -p "$DEST"

echo "Baixando icones para $DEST ..."
count=0
fail=0
for code in $(seq 33 126); do
  for table in 1 2; do
    fn="${code}-${table}.png"
    url="http://aprs.nakhonthai.net/symbols/icons/${fn}"
    if curl -sSf -o "$DEST/$fn" "$url"; then
      count=$((count+1))
    else
      fail=$((fail+1))
      rm -f "$DEST/$fn"
    fi
  done
done

# icone generico "dot.png" usado como fallback pelo proprio firmware
curl -sSf -o "$DEST/dot.png" "http://aprs.nakhonthai.net/symbols/icons/dot.png" && count=$((count+1)) || fail=$((fail+1))

echo "Concluido: $count arquivos baixados, $fail falharam."
du -sh "$DEST"
