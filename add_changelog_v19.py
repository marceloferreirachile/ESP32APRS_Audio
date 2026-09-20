#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
# Adds the missing About-page changelog bullets for the TCP KISS Server
# feature and the icon style change (nakhonthai -> aprs.fi), so the on-device
# "Changes:" list reflects what's actually in this build.
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

anchor = 'strcat(webString, "- Dashboard LAST HEARD icons now work without internet: loads from the internet first, falls back to a local copy already included on the device, then to a simple drawn icon (never blank)<br />\\n");\n'
if c.count(anchor) != 1:
    print(f"ERRO: nao encontrei a linha esperada do changelog de icones (encontrei {c.count(anchor)}x). Nada foi alterado.")
    sys.exit(1)

if "New: TCP KISS Server" in c:
    print("Changelog do TNC ja existe - pulando essa parte.")
    new_lines = ""
else:
    new_lines = (
        '\tstrcat(webString, "- New: TCP KISS Server (MOD tab) lets PC software (Xastir, APRSIS32, etc) use this device as a network TNC over WiFi, 2 ports, no cable needed. Off by default<br />\\n");\n'
    )

if "now match the aprs.fi" in c:
    print("Changelog dos icones aprs.fi ja existe - pulando essa parte.")
    icon_note = ""
else:
    icon_note = (
        '\tstrcat(webString, "- Dashboard icons now match the aprs.fi style (previously used a different icon set)<br />\\n");\n'
    )

if not new_lines and not icon_note:
    print("Nada para adicionar - changelog ja estava completo.")
    sys.exit(0)

c = c.replace(anchor, anchor + icon_note + new_lines, 1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: changelog do About atualizado (TCP KISS Server + estilo de icones aprs.fi).")
