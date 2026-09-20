#!/usr/bin/env python3
# Custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
#
# Root cause found via crash backtrace (moving the block to the top of
# loop() as a test made it crash on literally the first iteration, proving
# it): tcpKissServer1.begin()/tcpKissServer2.begin() call into lwIP's
# socket creation before the TCP/IP core task and its internal mutex are
# ready ("assert failed: xQueueSemaphoreTake queue.c:1709" from inside
# NetworkServer::begin() -> lwip_socket -> netconn_new... -> sys_mutex_lock
# -> xQueueSemaphoreTake on a not-yet-created queue). This was happening
# silently as a crash-reboot loop even in the original position deep in
# loop() - we just hadn't caught it on the serial monitor before.
#
# Fix: (1) move the block back to its original position in loop() (undo
# the temporary move-to-top debug test), and (2) add a millis() > 10000
# guard so we don't even attempt tcpKissServer.begin() until at least 10
# seconds after boot, well after WiFi/lwIP is up.
import sys

PATH = "src/main.cpp"

with open(PATH, "r", encoding="utf-8") as f:
    c = f.read()

# --- 1) Undo the move-to-top test: extract the block from the top of loop() ---
moved_start_marker = "        // --- MOVED HERE TEMPORARILY FOR DEBUGGING (was further down in loop()) ---\n        "
moved_end_marker = "\n        // --- END MOVED BLOCK ---\n"

if moved_start_marker in c:
    s = c.find(moved_start_marker)
    e = c.find(moved_end_marker, s)
    if e == -1:
        print("ERRO: encontrei o inicio do bloco movido mas nao o fim. Nada foi alterado.")
        sys.exit(1)
    block = c[s + len(moved_start_marker): e]
    # remove the moved block (including its markers) from the top of loop()
    c = c[:s] + c[e + len(moved_end_marker):]

    # find where to put it back: right before the External TNC handling block
    reinsert_anchor = "\n\n        if (config.ext_tnc_enable && (config.ext_tnc_mode > 0 && config.ext_tnc_mode < 5))"
    if c.count(reinsert_anchor) != 1:
        print(f"ERRO: esperava 1 ocorrencia do ponto de reinsercao, encontrei {c.count(reinsert_anchor)}. Nada foi alterado.")
        sys.exit(1)
    c = c.replace(reinsert_anchor, "\n\n        " + block.strip() + reinsert_anchor, 1)
    print("OK: bloco TCP KISS devolvido para a posicao original em loop().")
else:
    print("Bloco ja estava na posicao original (nao estava no topo) - pulando essa parte.")

# --- 2) Add the millis() > 10000 startup guard ---
old_cond = "if (config.tcp_kiss_enable && !tcpKissServersStarted)"
new_cond = "if (config.tcp_kiss_enable && !tcpKissServersStarted && millis() > 10000)"

if new_cond in c:
    print("Guarda de millis() ja estava aplicada - ok.")
elif c.count(old_cond) == 1:
    c = c.replace(old_cond, new_cond, 1)
    print("OK: adicionada guarda millis() > 10000 antes de iniciar os servidores TCP KISS (espera a rede estar pronta).")
else:
    print(f"ERRO: esperava 1 ocorrencia de '{old_cond}', encontrei {c.count(old_cond)}. Nada foi alterado.")
    sys.exit(1)

with open(PATH, "w", encoding="utf-8") as f:
    f.write(c)

print("OK: arquivo salvo.")
