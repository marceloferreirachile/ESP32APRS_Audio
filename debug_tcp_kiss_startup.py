#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Temporary debug aid: nc reports "Connection refused" on ports 8001/8002
# even with TCP KISS Server enabled and saved. The code that calls
# tcpKissServer1.begin()/tcpKissServer2.begin() uses log_i(), which compiles
# to a no-op because platformio.ini sets CONFIG_LOG_MAXIMUM_LEVEL=0 for all
# environments - so we can't tell from the serial monitor whether that code
# path is even running. This swaps those two log_i() calls for Serial.printf
# (which always prints, regardless of log level) so we can see it live.
#
# This is meant to be reverted once we find the real bug - it's a diagnostic
# step, not a permanent change.
import sys

PATH = "src/main.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

start_anchor = 'log_i("TCP KISS Server started on ports %u and %u", config.tcp_kiss_port1, config.tcp_kiss_port2);'
stop_anchor = 'log_i("TCP KISS Server stopped");'

if c.count(start_anchor) != 1:
    print(f"ERRO: esperava 1 ocorrencia da linha de log de start, encontrei {c.count(start_anchor)}. Nada foi alterado.")
    sys.exit(1)
if c.count(stop_anchor) != 1:
    print(f"ERRO: esperava 1 ocorrencia da linha de log de stop, encontrei {c.count(stop_anchor)}. Nada foi alterado.")
    sys.exit(1)

c = c.replace(
    start_anchor,
    'Serial.printf("[TCP KISS] Servers started on ports %u and %u\\n", config.tcp_kiss_port1, config.tcp_kiss_port2);'
)
c = c.replace(
    stop_anchor,
    'Serial.println("[TCP KISS] Servers stopped");'
)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: trocado log_i por Serial.printf/println nos 2 pontos (start/stop) do TCP KISS Server - temporario, so para debug.")
