#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# The serial monitor isn't reliably showing output after early boot on this
# setup, so we can't tell from Serial whether tcpKissServersStarted (the
# main.cpp loop() flag that gates calling tcpKissServer1.begin()/.begin())
# actually became true. This adds it directly to the MOD page's TCP KISS
# status block instead, so we can see it just by loading the page - no
# serial needed.
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

anchor = '\t\tstrcat(html, "<tr><td colspan=\\"2\\" style=\\"word-wrap:break-word;white-space:normal;\\"><p style=\\"font-size:9pt;margin:4px 0;\\">Lets PC software (Xastir, APRSIS32, etc) use this device as a KISS TNC over the local network - no cable needed. No password - anyone on this WiFi network can transmit through the radio via these ports while enabled. Port number changes need a reboot to take effect; the Enable switch does not.</p></td></tr>\\n");\n'
if c.count(anchor) != 1:
    print(f"ERRO: esperava 1 ocorrencia do anchor do texto de ajuda, encontrei {c.count(anchor)}. Nada foi alterado.")
    sys.exit(1)

debug_line = (
    '\t\tstrcat(html, "<tr><td align=\\"right\\">Debug:</td><td style=\\"text-align: left;\\">tcp_kiss_enable=");\n'
    '\t\tstrcat(html, config.tcp_kiss_enable ? "true" : "false");\n'
    '\t\tstrcat(html, " tcpKissServersStarted=");\n'
    '\t\textern bool tcpKissServersStarted;\n'
    '\t\tstrcat(html, tcpKissServersStarted ? "true" : "false");\n'
    '\t\tstrcat(html, "</td></tr>\\n");\n'
)

c = c.replace(anchor, debug_line + anchor, 1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: linha de debug (tcp_kiss_enable / tcpKissServersStarted) adicionada ao status do painel TCP KISS - temporario.")
