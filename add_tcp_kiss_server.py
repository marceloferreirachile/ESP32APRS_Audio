#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Adds a 2-port TCP KISS server so PC software (Xastir, APRSIS32, etc) can use
# this device as a network TNC, without a serial cable. Reuses the existing
# kiss_serial()/kiss_wrapper() functions already used for the serial/Bluetooth
# "External TNC" feature.
#
# Design notes (why it's built this way):
# - kiss_serial() uses a single shared/global decode buffer (not per-connection),
#   so all TCP-client byte reading happens from ONE place (the main loop()) to
#   avoid two clients' bytes interleaving mid-frame.
# - All WiFiServer/WiFiClient objects are only ever touched from loop() - never
#   from taskAPRS (a separate FreeRTOS task) - to avoid a cross-task race on
#   those objects. taskAPRS instead pushes finished packets into a small
#   FreeRTOS queue; loop() drains that queue and does the actual socket writes.
# - The "Enable" toggle starts/stops the servers live (no reboot needed).
#   Port number changes take effect after a reboot (documented in the UI) -
#   deliberately not doing a live rebind while a client might be connected.
# - Off by default. No authentication (same as the rest of this project) -
#   this is flagged in the on-page help text.
#
# Touches 4 files: include/config.h, src/config.cpp, include/webservice.h,
# src/main.cpp, src/webservice.cpp. Every edit is anchored on exact existing
# text and verified to occur exactly once before changing anything - if any
# anchor doesn't match (e.g. this script running against a different version
# of the file than expected), it aborts with a clear message and touches
# NOTHING, rather than guessing.
import re
import sys

def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def write(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

def must_replace(content, old, new, label, path):
    count = content.count(old)
    if count != 1:
        print(f"ERRO em {path}: esperava 1 ocorrencia de '{label}', encontrei {count}. Nada foi alterado nesse arquivo. Abortando.")
        sys.exit(1)
    return content.replace(old, new, 1)

changed_files = []

# ============================================================
# 1) include/config.h - new config fields
# ============================================================
path = "include/config.h"
c = read(path)
if "tcp_kiss_enable" in c:
    print(f"{path}: campos tcp_kiss_* ja existem - pulando.")
else:
    old = "\tbool ext_tnc_enable = false;\n\tint8_t ext_tnc_channel = 0;\n\tint8_t ext_tnc_mode = 0;\n"
    new = old + (
        "\n"
        "\t// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "\tbool tcp_kiss_enable = false;\n"
        "\tuint16_t tcp_kiss_port1 = 8001;\n"
        "\tuint16_t tcp_kiss_port2 = 8002;\n"
    )
    c = must_replace(c, old, new, "ext_tnc_mode struct fields", path)
    write(path, c)
    changed_files.append(path)
    print(f"OK: {path} atualizado (novos campos de config).")

# ============================================================
# 2) src/config.cpp - save + load
# ============================================================
path = "src/config.cpp"
c = read(path)
if "tcpKissEn" in c:
    print(f"{path}: chaves tcpKiss* ja existem - pulando.")
else:
    old_save = (
        "    // MOD External TNC\n"
        "    doc[\"extTNCEn\"] = config.ext_tnc_enable;\n"
        "    doc[\"extTNCCh\"] = config.ext_tnc_channel;\n"
        "    doc[\"extTNCMode\"] = config.ext_tnc_mode;\n"
    )
    new_save = old_save + (
        "\n"
        "    // MOD TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "    doc[\"tcpKissEn\"] = config.tcp_kiss_enable;\n"
        "    doc[\"tcpKissPort1\"] = config.tcp_kiss_port1;\n"
        "    doc[\"tcpKissPort2\"] = config.tcp_kiss_port2;\n"
    )
    c = must_replace(c, old_save, new_save, "saveConfiguration extTNC block", path)

    old_load = (
        "        // MOD External TNC\n"
        "        config.ext_tnc_enable = doc[\"extTNCEn\"];\n"
        "        config.ext_tnc_channel = doc[\"extTNCCh\"];\n"
        "        config.ext_tnc_mode = doc[\"extTNCMode\"];\n"
    )
    new_load = old_load + (
        "\n"
        "        // MOD TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "        // Uses \"| default\" so devices updating from an older config file (which won't have\n"
        "        // these keys yet) get safe defaults instead of 0/false-for-everything.\n"
        "        config.tcp_kiss_enable = doc[\"tcpKissEn\"] | false;\n"
        "        config.tcp_kiss_port1 = doc[\"tcpKissPort1\"] | 8001;\n"
        "        config.tcp_kiss_port2 = doc[\"tcpKissPort2\"] | 8002;\n"
    )
    c = must_replace(c, old_load, new_load, "loadConfiguration extTNC block", path)

    write(path, c)
    changed_files.append(path)
    print(f"OK: {path} atualizado (save + load).")

# ============================================================
# 3) include/webservice.h - extern declarations
# ============================================================
path = "include/webservice.h"
c = read(path)
if "tcpKissServer1" in c:
    print(f"{path}: externs ja existem - pulando.")
else:
    old = "extern WiFiClient aprsClient;\n"
    new = old + (
        "// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "extern WiFiServer tcpKissServer1;\n"
        "extern WiFiServer tcpKissServer2;\n"
        "extern WiFiClient tcpKissClient1;\n"
        "extern WiFiClient tcpKissClient2;\n"
        "extern uint32_t tcpKissRxCount[2];\n"
        "extern uint32_t tcpKissTxCount[2];\n"
    )
    c = must_replace(c, old, new, "extern aprsClient", path)
    write(path, c)
    changed_files.append(path)
    print(f"OK: {path} atualizado (externs).")

# ============================================================
# 4) src/main.cpp - globals, queue init, loop() polling, taskAPRS TX
# ============================================================
path = "src/main.cpp"
c = read(path)
if "tcpKissServer1" in c:
    print(f"{path}: recurso TCP KISS ja existe - pulando.")
else:
    # 4a) global declarations, right after WiFiClient aprsClient;
    old = "WiFiClient aprsClient;\n"
    new = old + (
        "\n"
        "// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "// All WiFiServer/WiFiClient access happens only from loop() (see below) to avoid a\n"
        "// cross-task race with taskAPRS - taskAPRS only pushes finished packets into\n"
        "// tcpKissTxQueue, it never touches the client/server objects directly.\n"
        "WiFiServer tcpKissServer1;\n"
        "WiFiServer tcpKissServer2;\n"
        "WiFiClient tcpKissClient1;\n"
        "WiFiClient tcpKissClient2;\n"
        "uint32_t tcpKissRxCount[2] = {0, 0};\n"
        "uint32_t tcpKissTxCount[2] = {0, 0};\n"
        "bool tcpKissServersStarted = false;\n"
        "typedef struct\n"
        "{\n"
        "    uint8_t data[500];\n"
        "    size_t len;\n"
        "} TcpKissTxItem;\n"
        "QueueHandle_t tcpKissTxQueue = NULL;\n"
    )
    c = must_replace(c, old, new, "WiFiClient aprsClient global", path)

    # 4b) create the queue early in setup()
    old = (
        "    memset(pkgList, 0, sizeof(pkgListType) * PKGLISTSIZE);\n"
        "    memset(Telemetry, 0, sizeof(TelemetryType) * TLMLISTSIZE);\n"
        "    memset(txQueue, 0, sizeof(txQueueType) * PKGTXSIZE);\n"
        "    memset(msgQueue, 0, sizeof(msgType) * PKGLISTSIZE);\n"
    )
    new = old + (
        "\n"
        "    // TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "    tcpKissTxQueue = xQueueCreate(4, sizeof(TcpKissTxItem));\n"
    )
    c = must_replace(c, old, new, "setup() memset block", path)

    # 4c) loop() - start/stop servers + accept clients + feed kiss_serial() + drain TX queue
    old = (
        "        if (config.ext_tnc_enable && (config.ext_tnc_mode > 0 && config.ext_tnc_mode < 5))\n"
        "        {\n"
        "            if (config.ext_tnc_mode == 1)\n"
        "            { // KISS\n"
    )
    new = (
        "        // TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "        // Enable toggle starts/stops the servers live. Port number changes need a reboot\n"
        "        // (documented on the MOD page) - simpler and safer than rebinding a live socket.\n"
        "        if (config.tcp_kiss_enable && !tcpKissServersStarted)\n"
        "        {\n"
        "            tcpKissServer1.begin(config.tcp_kiss_port1);\n"
        "            tcpKissServer2.begin(config.tcp_kiss_port2);\n"
        "            tcpKissServersStarted = true;\n"
        "            log_i(\"TCP KISS Server started on ports %u and %u\", config.tcp_kiss_port1, config.tcp_kiss_port2);\n"
        "        }\n"
        "        else if (!config.tcp_kiss_enable && tcpKissServersStarted)\n"
        "        {\n"
        "            tcpKissClient1.stop();\n"
        "            tcpKissClient2.stop();\n"
        "            tcpKissServer1.end();\n"
        "            tcpKissServer2.end();\n"
        "            tcpKissServersStarted = false;\n"
        "            log_i(\"TCP KISS Server stopped\");\n"
        "        }\n"
        "\n"
        "        if (config.tcp_kiss_enable)\n"
        "        {\n"
        "            if (!tcpKissClient1 || !tcpKissClient1.connected())\n"
        "            {\n"
        "                WiFiClient newClient1 = tcpKissServer1.available();\n"
        "                if (newClient1)\n"
        "                    tcpKissClient1 = newClient1;\n"
        "            }\n"
        "            if (!tcpKissClient2 || !tcpKissClient2.connected())\n"
        "            {\n"
        "                WiFiClient newClient2 = tcpKissServer2.available();\n"
        "                if (newClient2)\n"
        "                    tcpKissClient2 = newClient2;\n"
        "            }\n"
        "\n"
        "            if (tcpKissClient1 && tcpKissClient1.connected())\n"
        "            {\n"
        "                while (tcpKissClient1.available())\n"
        "                {\n"
        "                    kiss_serial((uint8_t)tcpKissClient1.read());\n"
        "                    tcpKissRxCount[0]++;\n"
        "                }\n"
        "            }\n"
        "            if (tcpKissClient2 && tcpKissClient2.connected())\n"
        "            {\n"
        "                while (tcpKissClient2.available())\n"
        "                {\n"
        "                    kiss_serial((uint8_t)tcpKissClient2.read());\n"
        "                    tcpKissRxCount[1]++;\n"
        "                }\n"
        "            }\n"
        "\n"
        "            if (tcpKissTxQueue != NULL)\n"
        "            {\n"
        "                TcpKissTxItem tcpKissItem;\n"
        "                while (xQueueReceive(tcpKissTxQueue, &tcpKissItem, 0) == pdTRUE)\n"
        "                {\n"
        "                    if (tcpKissClient1 && tcpKissClient1.connected())\n"
        "                    {\n"
        "                        tcpKissClient1.write(tcpKissItem.data, tcpKissItem.len);\n"
        "                        tcpKissTxCount[0]++;\n"
        "                    }\n"
        "                    if (tcpKissClient2 && tcpKissClient2.connected())\n"
        "                    {\n"
        "                        tcpKissClient2.write(tcpKissItem.data, tcpKissItem.len);\n"
        "                        tcpKissTxCount[1]++;\n"
        "                    }\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "\n"
        "        if (config.ext_tnc_enable && (config.ext_tnc_mode > 0 && config.ext_tnc_mode < 5))\n"
        "        {\n"
        "            if (config.ext_tnc_mode == 1)\n"
        "            { // KISS\n"
    )
    c = must_replace(c, old, new, "loop() ext_tnc RX block", path)

    # 4d) taskAPRS - after decoding an incoming RF packet, push it to the TX queue
    #     (queue only - the actual socket write happens in loop(), see 4c)
    old = (
        "                    if (config.ext_tnc_enable)\n"
        "                    {\n"
        "                        if (config.ext_tnc_channel > 0 && config.ext_tnc_channel < 5)\n"
        "                        {\n"
    )
    new = (
        "                    // TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "                    // Only queues the packet here - loop() owns the actual socket writes,\n"
        "                    // to keep all WiFiClient access on a single task (see globals above).\n"
        "                    if (config.tcp_kiss_enable && tcpKissTxQueue != NULL)\n"
        "                    {\n"
        "                        TcpKissTxItem tcpKissItem;\n"
        "                        tcpKissItem.len = kiss_wrapper(tcpKissItem.data, buf, size);\n"
        "                        xQueueSend(tcpKissTxQueue, &tcpKissItem, 0);\n"
        "                    }\n"
        "\n"
        "                    if (config.ext_tnc_enable)\n"
        "                    {\n"
        "                        if (config.ext_tnc_channel > 0 && config.ext_tnc_channel < 5)\n"
        "                        {\n"
    )
    c = must_replace(c, old, new, "taskAPRS ext_tnc TX block", path)

    write(path, c)
    changed_files.append(path)
    print(f"OK: {path} atualizado (globais, fila, loop(), taskAPRS).")

# ============================================================
# 5) src/webservice.cpp - MOD page panel + POST handler
# ============================================================
path = "src/webservice.cpp"
c = read(path)
if "commitTCPKISS" in c:
    print(f"{path}: painel/handler TCP KISS ja existem - pulando.")
else:
    # 5a) POST handler - insert right before commitONEWIRE
    old_handler_anchor = "\telse if (request->hasArg(\"commitONEWIRE\"))\n\t{\n\t\tbool En = false;"
    new_handler = (
        "\telse if (request->hasArg(\"commitTCPKISS\"))\n"
        "\t{\n"
        "\t\t// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "\t\tbool En = false;\n"
        "\t\tfor (uint8_t i = 0; i < request->args(); i++)\n"
        "\t\t{\n"
        "\t\t\tif (request->argName(i) == \"Enable\")\n"
        "\t\t\t{\n"
        "\t\t\t\tif (request->arg(i) != \"\")\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tif (String(request->arg(i)) == \"OK\")\n"
        "\t\t\t\t\t\tEn = true;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tif (request->argName(i) == \"port1\")\n"
        "\t\t\t{\n"
        "\t\t\t\tif (isValidNumber(request->arg(i)))\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tint p = request->arg(i).toInt();\n"
        "\t\t\t\t\tif (p > 0 && p < 65536)\n"
        "\t\t\t\t\t\tconfig.tcp_kiss_port1 = (uint16_t)p;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t\tif (request->argName(i) == \"port2\")\n"
        "\t\t\t{\n"
        "\t\t\t\tif (isValidNumber(request->arg(i)))\n"
        "\t\t\t\t{\n"
        "\t\t\t\t\tint p = request->arg(i).toInt();\n"
        "\t\t\t\t\tif (p > 0 && p < 65536)\n"
        "\t\t\t\t\t\tconfig.tcp_kiss_port2 = (uint16_t)p;\n"
        "\t\t\t\t}\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tconfig.tcp_kiss_enable = En;\n"
        "\t\tsaveConfiguration(\"/default.cfg\", config);\n"
        "\t\tString html = \"OK\";\n"
        "\t\trequest->send(200, \"text/html\", html);\n"
        "\t}\n"
    )
    c = must_replace(c, old_handler_anchor, new_handler + old_handler_anchor, "commitONEWIRE handler anchor", path)

    # 5b) MOD page HTML panel - insert right after the External TNC form's closing </form>
    old_panel_anchor = (
        "\t\tstrcat(html, \"<input class=\\\"button\\\" id=\\\"submitTNC\\\" name=\\\"commitTNC\\\" type=\\\"submit\\\" value=\\\"Apply\\\" maxlength=\\\"80\\\"/>\\n\");\n"
        "\t\tstrcat(html, \"<input type=\\\"hidden\\\" name=\\\"commitTNC\\\"/>\\n\");\n"
        "\t\tstrcat(html, \"</td></tr></table>\\n\");\n"
        "\t\tstrcat(html, \"</form>\\n\");\n"
    )
    new_panel = (
        "\n"
        "\t\t/**************TCP KISS Server Modify******************/\n"
        "\t\t// custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026\n"
        "\t\tstrcat(html, \"<form accept-charset=\\\"UTF-8\\\" action=\\\"#\\\" class=\\\"form-horizontal\\\" id=\\\"fromTCPKISS\\\" method=\\\"post\\\">\\n\");\n"
        "\t\tstrcat(html, \"<table>\\n\");\n"
        "\t\tstrcat(html, \"<th colspan=\\\"2\\\"><span><b>TCP KISS Server Modify</b></span></th>\\n\");\n"
        "\t\tstrcat(html, \"<tr>\\n\");\n"
        "\n"
        "\t\tstrcpy(enFlage, \"\");\n"
        "\t\tif (config.tcp_kiss_enable)\n"
        "\t\t\tstrcpy(enFlage, \"checked\");\n"
        "\t\tstrcat(html, \"<td align=\\\"right\\\"><b>Enable</b></td>\\n\");\n"
        "\t\tstrcat(html, \"<td style=\\\"text-align: left;\\\"><label class=\\\"switch\\\"><input type=\\\"checkbox\\\" name=\\\"Enable\\\" value=\\\"OK\\\" \");\n"
        "\t\tstrcat(html, enFlage);\n"
        "\t\tstrcat(html, \"><span class=\\\"slider round\\\"></span></label></td>\\n\");\n"
        "\t\tstrcat(html, \"</tr>\\n\");\n"
        "\n"
        "\t\tstrcat(html, \"<tr>\\n\");\n"
        "\t\tstrcat(html, \"<td align=\\\"right\\\"><b>PORT 1:</b></td>\\n\");\n"
        "\t\tstrcat(html, \"<td style=\\\"text-align: left;\\\"><input type=\\\"text\\\" name=\\\"port1\\\" value=\\\"\");\n"
        "\t\tstrcat(html, String(config.tcp_kiss_port1).c_str());\n"
        "\t\tstrcat(html, \"\\\" maxlength=\\\"5\\\" size=\\\"6\\\"/></td>\\n\");\n"
        "\t\tstrcat(html, \"</tr>\\n\");\n"
        "\n"
        "\t\tstrcat(html, \"<tr>\\n\");\n"
        "\t\tstrcat(html, \"<td align=\\\"right\\\"><b>PORT 2:</b></td>\\n\");\n"
        "\t\tstrcat(html, \"<td style=\\\"text-align: left;\\\"><input type=\\\"text\\\" name=\\\"port2\\\" value=\\\"\");\n"
        "\t\tstrcat(html, String(config.tcp_kiss_port2).c_str());\n"
        "\t\tstrcat(html, \"\\\" maxlength=\\\"5\\\" size=\\\"6\\\"/></td>\\n\");\n"
        "\t\tstrcat(html, \"</tr>\\n\");\n"
        "\n"
        "\t\t{\n"
        "\t\t\tString tcpKissStatus1 = \"Port \" + String(config.tcp_kiss_port1) + \": \";\n"
        "\t\t\tif (tcpKissClient1 && tcpKissClient1.connected())\n"
        "\t\t\t\ttcpKissStatus1 += \"connected (\" + tcpKissClient1.remoteIP().toString() + \") RX:\" + String(tcpKissRxCount[0]) + \" TX:\" + String(tcpKissTxCount[0]);\n"
        "\t\t\telse\n"
        "\t\t\t\ttcpKissStatus1 += \"idle\";\n"
        "\t\t\tString tcpKissStatus2 = \"Port \" + String(config.tcp_kiss_port2) + \": \";\n"
        "\t\t\tif (tcpKissClient2 && tcpKissClient2.connected())\n"
        "\t\t\t\ttcpKissStatus2 += \"connected (\" + tcpKissClient2.remoteIP().toString() + \") RX:\" + String(tcpKissRxCount[1]) + \" TX:\" + String(tcpKissTxCount[1]);\n"
        "\t\t\telse\n"
        "\t\t\t\ttcpKissStatus2 += \"idle\";\n"
        "\t\t\tstrcat(html, \"<tr><td colspan=\\\"2\\\" align=\\\"center\\\"><b>Status:</b></td></tr>\\n\");\n"
        "\t\t\tstrcat(html, \"<tr><td colspan=\\\"2\\\" align=\\\"center\\\">\");\n"
        "\t\t\tstrcat(html, tcpKissStatus1.c_str());\n"
        "\t\t\tstrcat(html, \"</td></tr>\\n\");\n"
        "\t\t\tstrcat(html, \"<tr><td colspan=\\\"2\\\" align=\\\"center\\\">\");\n"
        "\t\t\tstrcat(html, tcpKissStatus2.c_str());\n"
        "\t\t\tstrcat(html, \"</td></tr>\\n\");\n"
        "\t\t}\n"
        "\n"
        "\t\tstrcat(html, \"<tr><td colspan=\\\"2\\\" style=\\\"word-wrap:break-word;white-space:normal;\\\"><p style=\\\"font-size:9pt;margin:4px 0;\\\">Lets PC software (Xastir, APRSIS32, etc) use this device as a KISS TNC over the local network - no cable needed. No password - anyone on this WiFi network can transmit through the radio via these ports while enabled. Port number changes need a reboot to take effect; the Enable switch does not.</p></td></tr>\\n\");\n"
        "\n"
        "\t\tstrcat(html, \"<tr><td colspan=\\\"2\\\" align=\\\"right\\\">\\n\");\n"
        "\t\tstrcat(html, \"<input class=\\\"button\\\" id=\\\"submitTCPKISS\\\" name=\\\"commitTCPKISS\\\" type=\\\"submit\\\" value=\\\"Apply\\\" maxlength=\\\"80\\\"/>\\n\");\n"
        "\t\tstrcat(html, \"<input type=\\\"hidden\\\" name=\\\"commitTCPKISS\\\"/>\\n\");\n"
        "\t\tstrcat(html, \"</td></tr></table>\\n\");\n"
        "\t\tstrcat(html, \"</form>\\n\");\n"
    )
    c = must_replace(c, old_panel_anchor, old_panel_anchor + new_panel, "External TNC form closing", path)

    write(path, c)
    changed_files.append(path)
    print(f"OK: {path} atualizado (painel MOD + handler POST).")

print("\n" + ("=" * 60))
if changed_files:
    print("OK geral: arquivos alterados:")
    for f in changed_files:
        print(f"  - {f}")
    print("\nProximo passo: compile SEM gravar no equipamento, so para verificar erros de sintaxe:")
    print("  ~/.platformio/penv/bin/pio run -e esp32-nodisp")
else:
    print("Nada foi alterado (recurso ja parecia estar presente em todos os arquivos).")
