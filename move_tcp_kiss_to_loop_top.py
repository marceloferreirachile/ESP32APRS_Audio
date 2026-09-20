#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Diagnostic test: config.tcp_kiss_enable is confirmed true (via the debug
# line on the MOD page) but tcpKissServersStarted stays false, meaning the
# code that calls tcpKissServer1.begin()/.begin() never successfully runs -
# even though it's structurally unconditional (no #ifdef, no early return
# blocks it, confirmed by direct inspection of the pushed source). This
# moves the whole TCP KISS block from its current spot (deep in loop(),
# after a lot of sensor/AT-command handling code) to the very first thing
# in loop(), right after the opening brace - to test whether something
# earlier in loop() is silently preventing execution from reaching it.
import sys

PATH = "src/main.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

block_start_marker = '// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n        // Enable toggle starts/stops the servers live.'
block_end_marker = '\n\n        if (config.ext_tnc_enable && (config.ext_tnc_mode > 0 && config.ext_tnc_mode < 5))'

start_idx = c.find(block_start_marker)
if start_idx == -1:
    print("ERRO: nao encontrei o inicio do bloco TCP KISS em loop(). Nada foi alterado.")
    sys.exit(1)
if c.count(block_start_marker) != 1:
    print(f"ERRO: marcador de inicio aparece {c.count(block_start_marker)} vezes, esperava 1. Nada foi alterado.")
    sys.exit(1)

end_idx = c.find(block_end_marker, start_idx)
if end_idx == -1:
    print("ERRO: nao encontrei o fim do bloco TCP KISS. Nada foi alterado.")
    sys.exit(1)

block = c[start_idx:end_idx]

# Remove the block from its current location (leave the surrounding blank
# lines tidy - the marker starts right after "\n\n        " so just cut the
# block text itself)
c_without_block = c[:start_idx] + c[end_idx:]

# Find "void loop()\n{" to insert right after its opening brace
loop_anchor = "void loop()\n{"
loop_idx = c_without_block.find(loop_anchor)
if loop_idx == -1:
    print("ERRO: nao encontrei 'void loop()\\n{'. Nada foi alterado.")
    sys.exit(1)
if c_without_block.count(loop_anchor) != 1:
    print(f"ERRO: 'void loop()\\n{{' aparece {c_without_block.count(loop_anchor)} vezes, esperava 1. Nada foi alterado.")
    sys.exit(1)

insert_pos = loop_idx + len(loop_anchor)
new_c = (
    c_without_block[:insert_pos]
    + "\n        // --- MOVED HERE TEMPORARILY FOR DEBUGGING (was further down in loop()) ---\n        "
    + block.strip()
    + "\n        // --- END MOVED BLOCK ---\n"
    + c_without_block[insert_pos:]
)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(new_c)

print("OK: bloco TCP KISS movido para o topo de loop() (teste de diagnostico).")
