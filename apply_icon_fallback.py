#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
# Applies the icon internet-first / vector-fallback JS to src/webservice.cpp
# Run from the project root: python3 apply_icon_fallback.py

import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    content = f.read()

if "function iconFallback" in content:
    print("iconFallback ja existe no arquivo - nada a fazer (ja aplicado).")
    sys.exit(0)

ANCHOR = 'strcat(webString, "function printLastHeard(data) {\\n");'

if content.count(ANCHOR) != 1:
    print(f"ERRO: esperava encontrar 1 ocorrencia do anchor, encontrei {content.count(ANCHOR)}. Abortando sem alterar nada.")
    sys.exit(1)

FUNCTIONS = '''strcat(webString, "function iconFallback(imgEl, iconFile, initial) {\\n");
strcat(webString, "  imgEl.style.display='none';\\n");
strcat(webString, "  var span = imgEl.nextElementSibling;\\n");
strcat(webString, "  if (!span) return;\\n");
strcat(webString, "  span.style.display='inline-flex';\\n");
strcat(webString, "  span.style.alignItems='center';\\n");
strcat(webString, "  span.style.justifyContent='center';\\n");
strcat(webString, "  var code = parseInt(iconFile.split('-')[0], 10);\\n");
strcat(webString, "  var shapes = {\\n");
strcat(webString, "    62: \\"<svg viewBox='0 0 24 24' width='18' height='18'><rect x='2' y='10' width='20' height='8' rx='2' fill='#1565c0'/><circle cx='7' cy='19' r='2' fill='#333'/><circle cx='17' cy='19' r='2' fill='#333'/></svg>\\",\\n");
strcat(webString, "    45: \\"<svg viewBox='0 0 24 24' width='18' height='18'><polygon points='12,3 22,12 19,12 19,21 5,21 5,12 2,12' fill='#8d6e63'/></svg>\\",\\n");
strcat(webString, "    35: \\"<svg viewBox='0 0 24 24' width='18' height='18'><polygon points='12,2 15,9 22,9 16,14 18,21 12,17 6,21 8,14 2,9 9,9' fill='#fbc02d'/></svg>\\",\\n");
strcat(webString, "    95: \\"<svg viewBox='0 0 24 24' width='18' height='18'><ellipse cx='12' cy='13' rx='9' ry='6' fill='#90a4ae'/></svg>\\"\\n");
strcat(webString, "  };\\n");
strcat(webString, "  if (shapes[code]) {\\n");
strcat(webString, "    span.innerHTML = shapes[code];\\n");
strcat(webString, "  } else {\\n");
strcat(webString, "    span.style.width='18px'; span.style.height='18px'; span.style.borderRadius='50%';\\n");
strcat(webString, "    span.style.background='#607d8b'; span.style.color='#fff'; span.style.fontSize='11px'; span.style.fontWeight='bold';\\n");
strcat(webString, "    span.style.display='inline-flex'; span.style.alignItems='center'; span.style.justifyContent='center';\\n");
strcat(webString, "    span.textContent = (initial || '?').toUpperCase();\\n");
strcat(webString, "  }\\n");
strcat(webString, "}\\n\\n");

strcat(webString, "function iconError(imgEl, iconFile, initial) {\\n");
strcat(webString, "  if (!imgEl.dataset.stage) {\\n");
strcat(webString, "    imgEl.dataset.stage = 'local';\\n");
strcat(webString, "    imgEl.src = '/symbols/icons/' + iconFile;\\n");
strcat(webString, "  } else {\\n");
strcat(webString, "    iconFallback(imgEl, iconFile, initial);\\n");
strcat(webString, "  }\\n");
strcat(webString, "}\\n\\n");

'''

content = content.replace(ANCHOR, FUNCTIONS + ANCHOR, 1)

OLD_IMG = 'strcat(webString, "<td><img src=\\"http://aprs.nakhonthai.net/symbols/icons/${row.icon}\\"></td>\\n");'
NEW_IMG = 'strcat(webString, "<td><img src=\\"http://aprs.nakhonthai.net/symbols/icons/${row.icon}\\" style=\\"width:20px;height:20px;\\" onerror=\\"iconError(this, \'${row.icon}\', \'${(row.callsign||\'?\').charAt(0)}\');\\"><span style=\\"display:none;width:20px;height:20px;\\"></span></td>\\n");'

if content.count(OLD_IMG) != 1:
    print(f"ERRO: esperava 1 ocorrencia da linha do <img>, encontrei {content.count(OLD_IMG)}. Abortando sem salvar (funcoes JS nao foram gravadas).")
    sys.exit(1)

content = content.replace(OLD_IMG, NEW_IMG, 1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(content)

print("OK: funcoes iconFallback/iconError inseridas e <td><img> atualizado com onerror.")
