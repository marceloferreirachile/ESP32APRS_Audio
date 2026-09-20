#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Removes the "Install Icons" internet-download feature entirely (button,
# backend HTTPS/OTA download task, log endpoints). Decision: icons.dat now
# ships pre-packed inside data/symbols/, so every normal filesystem build/
# upload already includes it - no separate download-and-flash step needed,
# and no risk of that step wiping /default.cfg (since it did a FULL
# filesystem-partition overwrite via Update.begin(..., U_SPIFFS), which
# silently wiped saved WiFi/APRS config because default.cfg isn't part of
# the repo's data/ folder).
#
# Keeps: the safe, read-only icon-pack SERVING code (ICON_PACK_PATH,
# loadIconPackIndex, serveIconFromPack, the notFound() hook) - that part
# only reads from LittleFS to answer /symbols/icons/<name> requests and is
# unrelated to the risky download feature.
import re
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    content = f.read()

original_len = len(content)
removed_something = False

# --- 1) Remove backend: ICONS_FS_URL define through handle_icon_install_log() ---
start_marker = "#define ICONS_FS_URL"
end_marker_re = re.compile(
    r'void handle_icon_install_log\(AsyncWebServerRequest \*request\)\s*\{\s*request->send\(200, "text/plain", iconInstallLog\);\s*\}\s*\n?'
)

start_idx = content.find(start_marker)
if start_idx == -1:
    print("AVISO: nao encontrei '#define ICONS_FS_URL' - talvez ja tenha sido removido. Pulando essa parte.")
else:
    # walk back to include any full-line comments immediately preceding, up to a blank line boundary
    line_start = content.rfind("\n", 0, start_idx) + 1
    block_start = line_start
    # expand backward over consecutive comment lines (// ...)
    while True:
        prev_nl = content.rfind("\n", 0, block_start - 1)
        prev_line = content[prev_nl + 1:block_start - 1]
        if prev_line.strip().startswith("//") or prev_line.strip() == "":
            block_start = prev_nl + 1
        else:
            break

    m = end_marker_re.search(content, start_idx)
    if not m:
        print("ERRO: encontrei o inicio (ICONS_FS_URL) mas nao o fim (handle_icon_install_log). Abortando sem alterar nada.")
        sys.exit(1)
    block_end = m.end()

    removed_block = content[block_start:block_end]
    # sanity check: this block must NOT contain the safe serving code we want to KEEP
    if "loadIconPackIndex" in removed_block or "serveIconFromPack" in removed_block:
        print("ERRO: o bloco a remover contem codigo que deveria ficar (serveIconFromPack/loadIconPackIndex). Abortando sem alterar nada.")
        sys.exit(1)

    content = content[:block_start] + content[block_end:]
    removed_something = True
    print(f"OK: removido bloco backend de download (ICONS_FS_URL..handle_icon_install_log), {len(removed_block)} bytes.")

# --- 2) Remove route registrations for /update_icons and /icon_install_log ---
route_pattern = re.compile(
    r'\tasync_server\.on\("/update_icons", HTTP_GET, \[\]\(AsyncWebServerRequest \*request\)\s*'
    r'\{\s*handle_update_icons\(request\);\s*\}\);\s*\n'
    r'\tasync_server\.on\("/icon_install_log", HTTP_GET, \[\]\(AsyncWebServerRequest \*request\)\s*'
    r'\{\s*handle_icon_install_log\(request\);\s*\}\);\s*\n?'
)
new_content, n = route_pattern.subn("", content)
if n == 1:
    content = new_content
    removed_something = True
    print("OK: removidas as rotas /update_icons e /icon_install_log.")
elif n == 0:
    print("AVISO: nao encontrei as rotas /update_icons e /icon_install_log juntas - tentando individualmente.")
    for single_pat in [
        r'\tasync_server\.on\("/update_icons", HTTP_GET, \[\]\(AsyncWebServerRequest \*request\)\s*\{\s*handle_update_icons\(request\);\s*\}\);\s*\n?',
        r'\tasync_server\.on\("/icon_install_log", HTTP_GET, \[\]\(AsyncWebServerRequest \*request\)\s*\{\s*handle_icon_install_log\(request\);\s*\}\);\s*\n?',
    ]:
        content, n2 = re.subn(single_pat, "", content)
        if n2 == 1:
            removed_something = True
            print(f"OK: removida rota (padrao individual), {n2}x.")
else:
    print(f"ERRO: padrao das rotas encontrado {n}x (esperava 1). Abortando sem alterar nada.")
    sys.exit(1)

# --- 3) Remove the About-page UI block (title/status/text/button/log/script) ---
title_re = re.compile(r'strcat\(webString, "<th colspan=\\"2\\"><span><b>(?:Install Icons \(for offline use\)|Instalar Icones \(para usar sem internet\))</b></span></th>\\n"\);\n')
title_m = title_re.search(content)
if not title_m:
    print("AVISO: nao encontrei o titulo 'Install Icons' no About - talvez ja removido. Pulando UI.")
else:
    # find the enclosing "<table>" strcat just before the title, to remove the whole table block
    table_open_idx = content.rfind('strcat(webString, "<table>");', 0, title_m.start())
    if table_open_idx == -1:
        print("ERRO: achei o titulo mas nao o '<table>' de abertura correspondente. Abortando sem alterar o About.")
        sys.exit(1)

    # find end: the script block containing updateIconsFromInternet, ending at its </script> close
    script_idx = content.find("updateIconsFromInternet", title_m.end())
    if script_idx == -1:
        print("ERRO: achei o titulo mas nao o script 'updateIconsFromInternet' depois dele. Abortando sem alterar o About.")
        sys.exit(1)
    script_close = content.find('</script>");', script_idx)
    if script_close == -1:
        print("ERRO: achei o script mas nao seu fechamento '</script>\");'. Abortando sem alterar o About.")
        sys.exit(1)
    block_end = content.find("\n", script_close) + 1

    removed_ui = content[table_open_idx:block_end]
    content = content[:table_open_idx] + content[block_end:]
    removed_something = True
    print(f"OK: removida a secao 'Install Icons' do About (UI + script), {len(removed_ui)} bytes.")

# --- 4) Update the changelog bullet to reflect icons ship pre-installed ---
old_bullets = [
    'strcat(webString, "- Dashboard LAST HEARD icons now work without internet: loads from the internet first, falls back to a local copy stored on the device, then to a simple drawn icon (never blank). See the \\"Install Icons\\" button below to install the local copy<br />\\n");',
    'strcat(webString, "- Dashboard LAST HEARD icons now work without internet: loads from the internet first, falls back to a local copy stored on the device, then to a simple drawn icon (never blank). See the \\"Instalar Icones\\" button below to install the local copy<br />\\n");',
]
new_bullet = 'strcat(webString, "- Dashboard LAST HEARD icons now work without internet: loads from the internet first, falls back to a local copy already included on the device, then to a simple drawn icon (never blank)<br />\\n");'
bullet_done = False
for old in old_bullets:
    if content.count(old) == 1:
        content = content.replace(old, new_bullet, 1)
        bullet_done = True
        removed_something = True
        print("OK: changelog atualizado (icones ja vem instalados, sem mencionar botao).")
        break
if not bullet_done:
    print("AVISO: nao encontrei a linha exata do changelog sobre icones para atualizar - revise manualmente se quiser.")

if not removed_something:
    print("Nada foi alterado (nada encontrado para remover). Verifique se o arquivo ja estava limpo.")
    sys.exit(0)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(content)

print(f"\nOK geral: arquivo salvo. Tamanho antes={original_len} depois={len(content)} bytes.")
