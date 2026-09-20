#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Finishes the v1.9-lu6jmf webservice.cpp changes that never actually landed
# from the earlier add_tcp_kiss_server.py run (config.h/main.cpp/webservice.h
# got their pieces, but webservice.cpp did not - confirmed by re-fetching the
# pushed commit and finding no "TCP KISS" text and no commitTCPKISS handler
# anywhere in handle_mod(), and the MOD-tab buffer still at 22000).
#
# This script, run from the project root (same folder as src/webservice.cpp):
#   1) Bumps handle_mod()'s HTML buffer 22000 -> 40000 bytes (same fix as
#      before, reapplied since it wasn't present in the pushed file).
#   2) Inserts the "TCP KISS Server Modify" panel right after the External
#      TNC panel's closing tags.
#   3) Inserts the commitTCPKISS POST handler right before commitONEWIRE.
#   4) Rewrites the About page's "Version:" line to say v1.9-lu6jmf, as a
#      fixed string - decoupled from the VERSION/VERSION_BUILD macros so the
#      "Firmware Version" field (which also uses those macros) is untouched.
#
# Every anchor below was verified against the real pushed file (commit
# 156aef3) via the GitHub raw content before writing this script.
import sys

PATH = "src/webservice.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

changes_made = []

# --- Scope to handle_mod() only, to avoid touching handle_tracker()'s
#     identical allocateStringMemory(22000) line ---
fn_anchor = "void handle_mod(AsyncWebServerRequest *request)"
fn_start = c.find(fn_anchor)
if fn_start == -1:
    print("ERRO: nao encontrei 'void handle_mod(AsyncWebServerRequest *request)'. Nada foi alterado.")
    sys.exit(1)
if c.count(fn_anchor) > 1:
    print(f"ERRO: '{fn_anchor}' aparece mais de uma vez. Nada foi alterado.")
    sys.exit(1)

next_fn_idx = c.find("\nvoid handle_", fn_start + 10)
if next_fn_idx == -1:
    print("ERRO: nao encontrei o fim de handle_mod() (proxima funcao). Nada foi alterado.")
    sys.exit(1)

mod_body = c[fn_start:next_fn_idx]

# --- 1) Buffer fix ---
buf_anchor = 'char *html = allocateStringMemory(22000);'
buf_count = mod_body.count(buf_anchor)
if buf_count == 1:
    mod_body = mod_body.replace(buf_anchor, 'char *html = allocateStringMemory(40000);', 1)
    changes_made.append("buffer 22000->40000")
elif buf_count == 0 and 'allocateStringMemory(40000)' in mod_body:
    changes_made.append("buffer ja estava em 40000 - ok")
else:
    print(f"ERRO: esperava 1 ocorrencia de '{buf_anchor}' dentro de handle_mod(), encontrei {buf_count}. Nada foi alterado.")
    sys.exit(1)

# --- 2) TCP KISS Server HTML panel, inserted right after the External TNC
#        panel's full closing block, before the AT-COMMAND table starts ---
panel_anchor = (
    '\t\tstrcat(html, "</td></tr></table>\\n");\n'
    '\t\tstrcat(html, "</form>\\n");\n'
    '\t\tstrcat(html, "</td></tr></table>\\n");\n'
    '\t\tstrcat(html, "<br />\\n");\n'
    '\n'
    '\t\tstrcat(html, "<table style=\\"text-align:unset;border-width:0px;background:unset\\"><tr style=\\"background:unset;vertical-align:top\\"><td width=\\"50%\\" style=\\"border:unset;vertical-align:top\\">\\n");\n'
    '\n'
    '\t\t/************************ AT-COMMAND **************************/\n'
)
panel_count = mod_body.count(panel_anchor)
if panel_count == 1 and "TCP KISS Server Modify" not in mod_body:
    tcp_panel = (
        '\t\t/************* TCP KISS Server Modify *************/\n'
        '\t\tstrcat(html, "<table style=\\"text-align:unset;border-width:0px;background:unset\\"><tr style=\\"background:unset;vertical-align:top\\"><td width=\\"100%\\" style=\\"border:unset;vertical-align:top\\">\\n");\n'
        '\t\tstrcat(html, "<form accept-charset=\\"UTF-8\\" action=\\"#\\" class=\\"form-horizontal\\" id=\\"formTCPKISS\\" method=\\"post\\">\\n");\n'
        '\t\tstrcat(html, "<table>\\n");\n'
        '\t\tstrcat(html, "<th colspan=\\"2\\"><span><b>TCP KISS Server Modify</b></span></th>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr>\\n");\n'
        '\t\tstrcpy(enFlage, "");\n'
        '\t\tif (config.tcp_kiss_enable)\n'
        '\t\t\tstrcpy(enFlage, "checked");\n'
        '\t\tstrcat(html, "<td align=\\"right\\"><b>Enable</b></td>\\n");\n'
        '\t\tstrcat(html, "<td style=\\"text-align: left;\\"><label class=\\"switch\\"><input type=\\"checkbox\\" name=\\"Enable\\" value=\\"OK\\" ");\n'
        '\t\tstrcat(html, enFlage);\n'
        '\t\tstrcat(html, "><span class=\\"slider round\\"></span></label></td>\\n");\n'
        '\t\tstrcat(html, "</tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr>\\n");\n'
        '\t\tstrcat(html, "<td align=\\"right\\"><b>Port 1:</b></td>\\n");\n'
        '\t\tstrcat(html, "<td style=\\"text-align: left;\\"><input type=\\"text\\" name=\\"port1\\" maxlength=\\"5\\" size=\\"6\\" value=\\"");\n'
        '\t\t{\n'
        '\t\t\tchar *kissPort1Str = intToString(config.tcp_kiss_port1);\n'
        '\t\t\tstrcat(html, kissPort1Str);\n'
        '\t\t\tfree(kissPort1Str);\n'
        '\t\t}\n'
        '\t\tstrcat(html, "\\"/></td>\\n");\n'
        '\t\tstrcat(html, "</tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr>\\n");\n'
        '\t\tstrcat(html, "<td align=\\"right\\"><b>Port 2:</b></td>\\n");\n'
        '\t\tstrcat(html, "<td style=\\"text-align: left;\\"><input type=\\"text\\" name=\\"port2\\" maxlength=\\"5\\" size=\\"6\\" value=\\"");\n'
        '\t\t{\n'
        '\t\t\tchar *kissPort2Str = intToString(config.tcp_kiss_port2);\n'
        '\t\t\tstrcat(html, kissPort2Str);\n'
        '\t\t\tfree(kissPort2Str);\n'
        '\t\t}\n'
        '\t\tstrcat(html, "\\"/></td>\\n");\n'
        '\t\tstrcat(html, "</tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr>\\n");\n'
        '\t\tstrcat(html, "<td align=\\"right\\"><b>Status:</b></td>\\n");\n'
        '\t\t{\n'
        '\t\t\tchar tcpKissStatus1[100];\n'
        '\t\t\tif (tcpKissClient1.connected())\n'
        '\t\t\t\tsnprintf(tcpKissStatus1, sizeof(tcpKissStatus1), "Port 1: connected (%s) RX:%u TX:%u", tcpKissClient1.remoteIP().toString().c_str(), tcpKissRxCount[0], tcpKissTxCount[0]);\n'
        '\t\t\telse\n'
        '\t\t\t\tsnprintf(tcpKissStatus1, sizeof(tcpKissStatus1), "Port 1: idle");\n'
        '\t\t\tstrcat(html, "<td style=\\"text-align: left;\\">");\n'
        '\t\t\tstrcat(html, tcpKissStatus1);\n'
        '\t\t\tstrcat(html, "</td>\\n");\n'
        '\t\t}\n'
        '\t\tstrcat(html, "</tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr>\\n");\n'
        '\t\tstrcat(html, "<td align=\\"right\\"></td>\\n");\n'
        '\t\t{\n'
        '\t\t\tchar tcpKissStatus2[100];\n'
        '\t\t\tif (tcpKissClient2.connected())\n'
        '\t\t\t\tsnprintf(tcpKissStatus2, sizeof(tcpKissStatus2), "Port 2: connected (%s) RX:%u TX:%u", tcpKissClient2.remoteIP().toString().c_str(), tcpKissRxCount[1], tcpKissTxCount[1]);\n'
        '\t\t\telse\n'
        '\t\t\t\tsnprintf(tcpKissStatus2, sizeof(tcpKissStatus2), "Port 2: idle");\n'
        '\t\t\tstrcat(html, "<td style=\\"text-align: left;\\">");\n'
        '\t\t\tstrcat(html, tcpKissStatus2);\n'
        '\t\t\tstrcat(html, "</td>\\n");\n'
        '\t\t}\n'
        '\t\tstrcat(html, "</tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr><td colspan=\\"2\\" style=\\"text-align:left;font-size:small;color:#555;\\">No login/password: any device on this WiFi network can transmit through the radio via these ports while enabled. Changing the port numbers needs a reboot to take effect.</td></tr>\\n");\n'
        '\n'
        '\t\tstrcat(html, "<tr><td colspan=\\"2\\" align=\\"right\\">\\n");\n'
        '\t\tstrcat(html, "<input class=\\"button\\" id=\\"submitTCPKISS\\" name=\\"commitTCPKISS\\" type=\\"submit\\" value=\\"Apply\\" maxlength=\\"80\\"/>\\n");\n'
        '\t\tstrcat(html, "<input type=\\"hidden\\" name=\\"commitTCPKISS\\"/>\\n");\n'
        '\t\tstrcat(html, "</td></tr></table>\\n");\n'
        '\t\tstrcat(html, "</form>\\n");\n'
        '\t\tstrcat(html, "</td></tr></table>\\n");\n'
        '\t\tstrcat(html, "<br />\\n");\n'
        '\n'
    )
    mod_body = mod_body.replace(panel_anchor, tcp_panel + panel_anchor, 1)
    changes_made.append("painel TCP KISS Server Modify inserido")
elif "TCP KISS Server Modify" in mod_body:
    changes_made.append("painel TCP KISS ja existia - ok")
else:
    print(f"ERRO: esperava 1 ocorrencia do anchor do painel External TNC, encontrei {panel_count}. Nada foi alterado.")
    sys.exit(1)

# --- 3) commitTCPKISS POST handler, inserted right before commitONEWIRE ---
handler_anchor = (
    '\t\tconfig.ext_tnc_enable = En;\n'
    '\t\tsaveConfiguration("/default.cfg", config);\n'
    '\t\tString html = "OK";\n'
    '\t\trequest->send(200, "text/html", html);\n'
    '\t}\n'
    '\telse if (request->hasArg("commitONEWIRE"))\n'
)
handler_count = mod_body.count(handler_anchor)
if handler_count == 1 and "commitTCPKISS" not in mod_body:
    tcp_handler = (
        '\telse if (request->hasArg("commitTCPKISS"))\n'
        '\t{\n'
        '\t\tbool En = false;\n'
        '\t\tfor (uint8_t i = 0; i < request->args(); i++)\n'
        '\t\t{\n'
        '\t\t\tif (request->argName(i) == "Enable")\n'
        '\t\t\t{\n'
        '\t\t\t\tif (request->arg(i) != "")\n'
        '\t\t\t\t{\n'
        '\t\t\t\t\tif (String(request->arg(i)) == "OK")\n'
        '\t\t\t\t\t\tEn = true;\n'
        '\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t\tif (request->argName(i) == "port1")\n'
        '\t\t\t{\n'
        '\t\t\t\tif (isValidNumber(request->arg(i)))\n'
        '\t\t\t\t{\n'
        '\t\t\t\t\tint p = request->arg(i).toInt();\n'
        '\t\t\t\t\tif (p > 0 && p < 65536)\n'
        '\t\t\t\t\t\tconfig.tcp_kiss_port1 = p;\n'
        '\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t\tif (request->argName(i) == "port2")\n'
        '\t\t\t{\n'
        '\t\t\t\tif (isValidNumber(request->arg(i)))\n'
        '\t\t\t\t{\n'
        '\t\t\t\t\tint p = request->arg(i).toInt();\n'
        '\t\t\t\t\tif (p > 0 && p < 65536)\n'
        '\t\t\t\t\t\tconfig.tcp_kiss_port2 = p;\n'
        '\t\t\t\t}\n'
        '\t\t\t}\n'
        '\t\t}\n'
        '\t\tconfig.tcp_kiss_enable = En;\n'
        '\t\tsaveConfiguration("/default.cfg", config);\n'
        '\t\tString html = "OK";\n'
        '\t\trequest->send(200, "text/html", html);\n'
        '\t}\n'
    )
    mod_body = mod_body.replace(handler_anchor, handler_anchor[:-len('\telse if (request->hasArg("commitONEWIRE"))\n')] + tcp_handler + 'else if (request->hasArg("commitONEWIRE"))\n', 1)
    changes_made.append("handler commitTCPKISS inserido")
elif "commitTCPKISS" in mod_body:
    changes_made.append("handler commitTCPKISS ja existia - ok")
else:
    print(f"ERRO: esperava 1 ocorrencia do anchor antes de commitONEWIRE, encontrei {handler_count}. Nada foi alterado.")
    sys.exit(1)

# Write handle_mod() changes back into the full file
c = c[:fn_start] + mod_body + c[next_fn_idx:]

# --- 4) Version line on the About page ---
version_anchor = (
    '\tstrcat(webString, "<tr><td align=\\"right\\"><b>Version: </b></td><td align=\\"left\\">Custom mods on top of V");\n'
    '\tstrcat(webString, VERSION);\n'
    '\tstrcat(webString, VERSION_BUILD);\n'
    '\tstrcat(webString, "</td></tr>\\n");\n'
)
version_count = c.count(version_anchor)
if version_count == 1:
    version_new = '\tstrcat(webString, "<tr><td align=\\"right\\"><b>Version: </b></td><td align=\\"left\\">v1.9-lu6jmf (custom mods on top of V1.8a)</td></tr>\\n");\n'
    c = c.replace(version_anchor, version_new, 1)
    changes_made.append("linha 'Version:' atualizada para v1.9-lu6jmf")
elif 'v1.9-lu6jmf' in c:
    changes_made.append("linha de versao ja atualizada - ok")
else:
    print(f"ERRO: esperava 1 ocorrencia do anchor da linha Version, encontrei {version_count}. Nada foi alterado.")
    sys.exit(1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK:")
for ch in changes_made:
    print(f"  - {ch}")
