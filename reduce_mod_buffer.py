#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Diagnostic step: the MOD page is coming back with corrupted heap memory
# mixed into the HTML (confirmed via curl - garbage bytes starting around
# offset 5614, in the pre-existing 1-Wire Bus section, long before any of
# our own TCP KISS code runs). Real content only needs ~23KB total, but we
# bumped the 'html' buffer to 40000 bytes, held for the whole function -
# that leaves less free heap for the ~40 small per-field allocations the
# (pre-existing, upstream) code does throughout the function. Reducing the
# buffer to something closer to the real need frees up that heap headroom
# as a test of whether that's the cause.
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

fn_anchor = "void handle_mod(AsyncWebServerRequest *request)"
fn_start = c.find(fn_anchor)
if fn_start == -1:
    print("ERRO: nao encontrei handle_mod(). Nada foi alterado.")
    sys.exit(1)

next_fn_idx = c.find("\nvoid handle_", fn_start + 10)
mod_body = c[fn_start:next_fn_idx]

anchor = 'char *html = allocateStringMemory(40000);'
if mod_body.count(anchor) != 1:
    print(f"ERRO: esperava 1 ocorrencia de '{anchor}' dentro de handle_mod(), encontrei {mod_body.count(anchor)}. Nada foi alterado.")
    sys.exit(1)

mod_body = mod_body.replace(anchor, 'char *html = allocateStringMemory(30000);', 1)
c = c[:fn_start] + mod_body + c[next_fn_idx:]

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: buffer da aba MOD reduzido de 40000 para 30000 bytes (teste de pressao de heap).")
