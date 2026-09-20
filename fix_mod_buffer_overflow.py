#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Fixes a crash on the MOD tab: handle_mod() builds its whole HTML response
# in a fixed-size buffer (allocateStringMemory(22000)). Just the literal
# strcat() text in that function already totals ~22457 bytes - before even
# counting the dynamic values (IPs, ports, status strings) - so it was
# already right at the edge, and the new TCP KISS Server panel we added
# pushed it over, corrupting heap memory and freezing the device whenever
# the MOD tab was opened.
#
# Fix: bump the buffer to 40000 bytes. Plenty of headroom, cheap on a
# device with ~300KB RAM, no other logic changes.
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

# This exact buffer-allocation line also appears in handle_tracker() - only
# handle_mod() is overflowing (it's the one that grew with the new TCP KISS
# panel), so anchor on the enclosing function to patch just that one.
fn_anchor = "void handle_mod(AsyncWebServerRequest *request)"
fn_start = c.find(fn_anchor)
if fn_start == -1:
    print("ERRO: nao encontrei 'void handle_mod(AsyncWebServerRequest *request)'. Nada foi alterado.")
    sys.exit(1)
if c.count(fn_anchor) > 1:
    print(f"ERRO: '{fn_anchor}' aparece mais de uma vez. Nada foi alterado.")
    sys.exit(1)

anchor = 'char *html = allocateStringMemory(22000);'
# scope the search to within handle_mod's body only (the buffer decl sits
# ~18000 chars into the function, after all the commitXXX POST-handling
# blocks; use a generous 25000-char window to stay well clear of the next
# function while excluding handle_tracker()'s copy of the same line)
WINDOW_SIZE = 25000
window = c[fn_start:fn_start + WINDOW_SIZE]
if window.count(anchor) != 1:
    print(f"ERRO: esperava 1 ocorrencia de '{anchor}' dentro de handle_mod(), encontrei {window.count(anchor)}. Nada foi alterado.")
    sys.exit(1)

new_window = window.replace(anchor, 'char *html = allocateStringMemory(40000);', 1)
c = c[:fn_start] + new_window + c[fn_start + WINDOW_SIZE:]

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: buffer da aba MOD aumentado de 22000 para 40000 bytes.")
