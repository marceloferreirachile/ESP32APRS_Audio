/*
 Name:		ESP32APRS_Audio
 Created:	13-10-2023 14:27:23
 Author:	HS5TQA/Atten
 Github:	https://github.com/nakhonthai
 Facebook:	https://www.facebook.com/atten
 Support IS: host:aprs.nakhonthai.net port:14580 or aprs.hs5tqa.ampr.org:14580
 Support IS monitor: http://aprs.nakhonthai.net:14501 or http://aprs.hs5tqa.ampr.org:14501
*/
#include <Arduino.h>
#include <memory>
#include "webservice.h"
// LU6JMF - Marcelo Ferreira: PRIME account system, ported from TX-SCHEDULER-TEST.
#include "prime.h"
// LU6JMF - Marcelo Ferreira: motor de templates (fase 6), template embutido no
// firmware - ver include/template_engine.h.
#include "template_engine.h"
#include "base64.hpp"
#include "wireguard_vpn.h"
#include <LibAPRSesp.h>
#include <parse_aprs.h>
#include "jquery_min_js.h"
#include <ESPCPUTemp.h>
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

extern SemaphoreHandle_t psramMutex;
extern bool psramLock(TickType_t timeout = portMAX_DELAY);
extern void psramUnlock();

extern int offset;

// Helper function to allocate memory with PSRAM support
char *allocateStringMemory(size_t size)
{
	char *ptr = NULL;
#ifdef BOARD_HAS_PSRAM
	// Try to allocate in PSRAM first
	size*=2; // overallocation to reduce fragmentation
	ptr = (char *)ps_calloc(size, sizeof(char));
	if (ptr != NULL)
	{
		memset(ptr, 0, size); // Initialize memory to zero
		return ptr;
	}
	// If PSRAM allocation fails, fall back to regular heap
#endif
	// Regular heap allocation
	ptr = (char *)calloc(size, sizeof(char));
	// calloc() already returns zero-initialized memory, and may return NULL
	// under memory pressure -- do NOT memset() an unchecked pointer here,
	// that was the real cause of the StoreProhibited crash (write to 0x0).
	return ptr;
}

// Helper function to format integers to string using allocateStringMemory
char *intToString(int value)
{
	char *str = allocateStringMemory(12); // Enough for a 32-bit integer + null terminator
	if (str != NULL)
	{
		sprintf(str, "%d", value);
	}
	return str;
}

// Helper function to format floats to string using allocateStringMemory
char *floatToString(float value, int decimals)
{
	char *str = allocateStringMemory(20); // Enough for most float values
	if (str != NULL)
	{
		switch (decimals)
		{
		case 0:
			sprintf(str, "%.0f", value);
			break;
		case 1:
			sprintf(str, "%.1f", value);
			break;
		case 2:
			sprintf(str, "%.2f", value);
			break;
		case 3:
			sprintf(str, "%.3f", value);
			break;
		default:
			sprintf(str, "%.2f", value);
			break;
		}
	}
	return str;
}

// Helper function to convert Arduino String to char* using allocateStringMemory
char *StringToCharPtr(const String &str)
{
	size_t len = str.length() + 1; // +1 for null terminator
	char *charPtr = allocateStringMemory(len);
	if (charPtr != NULL)
	{
		strcpy(charPtr, str.c_str());
	}
	return charPtr;
}

#ifdef PPPOS
#include <PPP.h>
#endif

#ifdef SH1106
#include <Adafruit_SH1106.h>
#else
#include "Adafruit_SSD1306.h"
#endif // SH1106

#define SCREEN_ADDRESS 0x3C

AsyncWebServer async_server(80);
AsyncWebServer async_websocket(81);
AsyncWebSocket ws("/ws");
AsyncWebSocket ws_gnss("/ws_gnss");

#ifdef MQTT
#include <PubSubClient.h>
extern PubSubClient clientMQTT;
#endif

#ifdef PPPOS
extern pppType pppStatus;
#endif

// Create an Event Source on /events
AsyncEventSource lastheard_events("/eventHeard");
AsyncEventSource message_events("/eventMsg");

char *webString;

extern unsigned long waitISRetry;
extern volatile int8_t adcEn;
extern volatile int8_t dacEn;
extern unsigned long upTimeStamp;
extern double VBat;
extern bool VBat_Flag;
extern char lastResetReasonStr[24];
extern uint16_t blnSentCount[9];
extern volatile bool blnRestartPending[9];

#ifdef OLED
#ifdef SH1106
extern Adafruit_SH1106 display;
#else
extern Adafruit_SSD1306 display;
#endif
#elif defined(GUI_LCD)
extern Adafruit_SH1106 display;
#endif // OLED

bool defaultSetting = false;

void saveConfig(AsyncWebServerRequest *request)
{
	String html;
	if (saveConfiguration("/default.cfg", config))
	{
		html = "Setup completed successfully";
		request->send(200, "text/html", html); // send to someones browser when asked
	}
	else
	{
		html = "Save config failed.";
		request->send(501, "text/html", html); // Not Implemented
	}
	html.clear();
}

void serviceHandle()
{
	// server.handleClient();
}

// custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
// APRS symbol icons are packed into a single LittleFS file (data/symbols/icons.dat,
// built by pack_icons.py) instead of ~190 separate tiny files: LittleFS allocates a
// full 4KB block per file, and our 128KB partition only has 32 blocks total - not
// enough for 190 files, but plenty for one ~60KB archive (~15 blocks).
#define ICON_PACK_PATH "/symbols/icons.dat"
#define ICON_PACK_MAX_ENTRIES 250
struct IconPackEntry
{
	char name[12];
	uint32_t offset;
	uint32_t length;
};
static IconPackEntry iconPackIndex[ICON_PACK_MAX_ENTRIES];
static int iconPackCount = -1; // -1 = not loaded yet, 0 = loaded but empty/missing
static uint32_t iconPackDataStart = 0;

static void loadIconPackIndex()
{
	iconPackCount = 0;
	if (!LITTLEFS.exists(ICON_PACK_PATH))
		return;
	File f = LITTLEFS.open(ICON_PACK_PATH, "r");
	if (!f)
		return;
	uint8_t hdr[6];
	if (f.read(hdr, 6) != 6 || hdr[0] != 'I' || hdr[1] != 'C' || hdr[2] != 'P' || hdr[3] != 'K')
	{
		f.close();
		return;
	}
	uint16_t count = hdr[4] | (hdr[5] << 8);
	if (count > ICON_PACK_MAX_ENTRIES)
		count = ICON_PACK_MAX_ENTRIES;
	for (uint16_t i = 0; i < count; i++)
	{
		uint8_t rec[20];
		if (f.read(rec, 20) != 20)
			break;
		memcpy(iconPackIndex[i].name, rec, 12);
		iconPackIndex[i].name[11] = '\0';
		iconPackIndex[i].offset = (uint32_t)rec[12] | ((uint32_t)rec[13] << 8) | ((uint32_t)rec[14] << 16) | ((uint32_t)rec[15] << 24);
		iconPackIndex[i].length = (uint32_t)rec[16] | ((uint32_t)rec[17] << 8) | ((uint32_t)rec[18] << 16) | ((uint32_t)rec[19] << 24);
		iconPackCount++;
	}
	iconPackDataStart = 6 + (uint32_t)count * 20;
	f.close();
}

// Returns true if it handled the request (found + served, or found + I/O error already answered).
static bool serveIconFromPack(AsyncWebServerRequest *request, const String &fileName)
{
	if (iconPackCount < 0)
		loadIconPackIndex();
	if (iconPackCount <= 0)
		return false;
	for (int i = 0; i < iconPackCount; i++)
	{
		if (fileName.equals(iconPackIndex[i].name))
		{
			uint32_t entryOffset = iconPackDataStart + iconPackIndex[i].offset;
			uint32_t entryLength = iconPackIndex[i].length;
			AsyncWebServerResponse *response = request->beginResponse(
				"image/png", entryLength,
				[entryOffset, entryLength](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
				{
					if (index >= entryLength)
						return 0;
					size_t remaining = entryLength - index;
					size_t toRead = remaining < maxLen ? remaining : maxLen;
					File pf = LITTLEFS.open(ICON_PACK_PATH, "r");
					if (!pf)
						return 0;
					if (!pf.seek(entryOffset + index))
					{
						pf.close();
						return 0;
					}
					size_t got = pf.read(buffer, toRead);
					pf.close();
					return got;
				});
			response->addHeader("Cache-Control", "public, max-age=86400");
			request->send(response);
			return true;
		}
	}
	return false;
}

void notFound(AsyncWebServerRequest *request)
{
	String url = request->url();
	if (url.startsWith("/symbols/icons/"))
	{
		String fileName = url.substring(strlen("/symbols/icons/"));
		if (serveIconFromPack(request, fileName))
			return;
	}
	request->send(404, "text/plain", "Not found");
}

void handle_logout(AsyncWebServerRequest *request)
{
	char *webString = allocateStringMemory(64); // Small buffer for "Log out"
	if (!webString)
	{
		return; // Memory allocation failed
	}
	strcpy(webString, "Log out");
	request->send(200, "text/html", webString);
	free(webString); // Free the allocated memory
}

// 2.1.5: defined further below (before handle_radio); declared here because earlier handlers use it.
static void sendHtmlChunked(AsyncWebServerRequest *request, String &page, const char *hdrName);

void setMainPage(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	// LU6JMF - Marcelo Ferreira:
	// Fase 6 (tirar HTML do codigo pra templates): a casca do SPA (o que
	// setMainPage imprime abaixo) agora tambem existe como arquivo em
	// LittleFS (data/templates/main_shell.html), com 4 placeholders pros
	// pontos dinamicos ({{TITLE}}, {{H1}}, {{MQTT_TAB_JS}}, {{MQTT_TAB_BTN}}).
	// Um device que so recebeu o .bin por OTA (sem subir esse arquivo pela
	// pagina Storage) ainda nao tem o template no filesystem - nesse caso o
	// bloco abaixo simplesmente nao entra, e o codigo antigo (mantido logo
	// depois, inalterado) gera a mesma pagina exatamente como sempre gerou.
	if (LITTLEFS.exists("/main_shell.html"))
	{
		File tf = LITTLEFS.open("/main_shell.html", "r");
		if (tf)
		{
			String tpl;
			tpl.reserve(tf.size() + 1);
			while (tf.available())
				tpl += (char)tf.read();
			tf.close();

			char temp_buffer[512];
			// LU6JMF - Marcelo Ferreira:
			// AR-APRS visual identity - fixed brand name/version, independent
			// of the configurable network hostname (matches the "black"
			// palette/branding already used in the newer TX-SCHEDULER engine).
			tpl.replace("{{TITLE}}", "<title>AR-APRS - Argentina APRS System</title>\n");

			snprintf(temp_buffer, sizeof(temp_buffer),
					 "<h1 style=\"margin-bottom:2px;\">AR-APRS</h1>\n"
					 "<div style=\"text-align:center;font-size:11px;margin-bottom:2px;\">Argentina APRS System &middot; V%s%s</div>\n",
					 VERSION, VERSION_BUILD);
			tpl.replace("{{H1}}", temp_buffer);

#ifdef MQTT
			tpl.replace("{{MQTT_TAB_JS}}", "} else if (tabName == 'MQTT') {\n$(\"#contentmain\").load(\"/mqtt\");\n");
			tpl.replace("{{MQTT_TAB_BTN}}", "<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MQTT')\">MQTT</button>\n");
#else
			tpl.replace("{{MQTT_TAB_JS}}", "");
			tpl.replace("{{MQTT_TAB_BTN}}", "");
#endif

			AsyncResponseStream *tplStream = request->beginResponseStream("text/html", tpl.length() + 200);
			if (tplStream)
			{
				tplStream->print(tpl);
				tplStream->addHeader("Sensor", "content");
				tplStream->addHeader("Cache-Control", "no-cache");
				request->send(tplStream);
				lastHeardTimeout = 0;
				lastHeard_Flag = true;
				return;
			}
		}
	}

	// ---- Fallback: embedded shell, used when the LittleFS template is absent ----
	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 26000);
	if (!webString)
	{
		return; // Memory allocation failed
	}

	webString->print("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
	webString->print("<meta name=\"robots\" content=\"index\" />\n");
	webString->print("<meta name=\"robots\" content=\"follow\" />\n");
	webString->print("<meta name=\"language\" content=\"English\" />\n");
	webString->print("<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\" />\n");
	webString->print("<meta name=\"GENERATOR\" content=\"configure 20230924\" />\n");
	webString->print("<meta name=\"Author\" content=\"Mr.Somkiat Nakhonthai (HS5TQA)\" />\n");
	webString->print("<meta name=\"Description\" content=\"Web Embedded Configuration\" />\n");
	webString->print("<meta name=\"KeyWords\" content=\"ESP32,ESP32C3,AFSK,APRS\" />\n");
	webString->print("<meta http-equiv=\"Cache-Control\" content=\"no-cache, no-store, must-revalidate\" />\n");
	webString->print("<meta http-equiv=\"pragma\" content=\"no-cache\" />\n");
	webString->print("<link rel=\"shortcut icon\" href=\"http://aprs.nakhonthai.net/favicon.ico\" type=\"image/x-icon\" />\n");
	webString->print("<meta http-equiv=\"Expires\" content=\"0\" />\n");

	char temp_buffer[512];
	// LU6JMF - Marcelo Ferreira:
	// AR-APRS visual identity - fixed brand name/version, independent of the
	// configurable network hostname.
	webString->print("<title>AR-APRS - Argentina APRS System</title>\n");

	webString->print("<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\" />\n");
	webString->print("<script src=\"/jquery-3.7.1.js\"></script>\n");
	webString->print("<script type=\"text/javascript\">\n");
	webString->print("function selectTab(evt, tabName) {\n");
	webString->print("var i, tabcontent, tablinks;\n");
	webString->print("tablinks = document.getElementsByClassName(\"nav-tabs\");\n");
	webString->print("for (i = 0; i < tablinks.length; i++) {\n");
	webString->print("tablinks[i].className = tablinks[i].className.replace(\" active\", \"\");\n");
	webString->print("}\n");
	webString->print("\n");
	webString->print("//document.getElementById(tabName).style.display = \"block\";\n");
	webString->print("if (tabName == 'DashBoard') {\n");
	webString->print("$(\"#contentmain\").load(\"/dashboard\");\n");
	webString->print("} else if (tabName == 'Radio') {\n");
	webString->print("$(\"#contentmain\").load(\"/radio\");\n");
	webString->print("} else if (tabName == 'IGATE') {\n");
	webString->print("$(\"#contentmain\").load(\"/igate\");\n");
	webString->print("} else if (tabName == 'IGTLM') {\n");
	webString->print("$(\"#contentmain\").load(\"/igate2\");\n");
	webString->print("} else if (tabName == 'DIGI') {\n");
	webString->print("$(\"#contentmain\").load(\"/digi\");\n");
	webString->print("} else if (tabName == 'TRACKER') {\n");
	webString->print("$(\"#contentmain\").load(\"/tracker\");\n");
	webString->print("} else if (tabName == 'WX') {\n");
	webString->print("$(\"#contentmain\").load(\"/wx\");\n");
	webString->print("} else if (tabName == 'WXCH') {\n");
	webString->print("$(\"#contentmain\").load(\"/wx2\");\n");
	webString->print("} else if (tabName == 'TLM') {\n");
	webString->print("$(\"#contentmain\").load(\"/tlm\");\n");
	webString->print("} else if (tabName == 'SENSOR') {\n");
	webString->print("$(\"#contentmain\").load(\"/sensor\");\n");
	webString->print("} else if (tabName == 'SCFG') {\n");
	webString->print("$(\"#contentmain\").load(\"/sensor2\");\n");
	webString->print("} else if (tabName == 'VPN') {\n");
	webString->print("$(\"#contentmain\").load(\"/vpn\");\n");
#ifdef MQTT
	webString->print("} else if (tabName == 'MQTT') {\n");
	webString->print("$(\"#contentmain\").load(\"/mqtt\");\n");
#endif
	webString->print("} else if (tabName == 'MSG') {\n");
	webString->print("$(\"#contentmain\").load(\"/msg\");\n");
	webString->print("} else if (tabName == 'OBJ') {\n");
	webString->print("$(\"#contentmain\").load(\"/object\");\n");
	webString->print("} else if (tabName == 'WiFi') {\n");
	webString->print("$(\"#contentmain\").load(\"/wireless\");\n");
	webString->print("} else if (tabName == 'MOD') {\n");
	webString->print("$(\"#contentmain\").load(\"/mod\");\n");
	webString->print("} else if (tabName == 'IO') {\n");
	webString->print("$(\"#contentmain\").load(\"/mod2\");\n");
	webString->print("} else if (tabName == 'System') {\n");
	webString->print("$(\"#contentmain\").load(\"/system\");\n");
	webString->print("} else if (tabName == 'File') {\n");
	webString->print("$(\"#contentmain\").load(\"/storage\");\n");
	webString->print("} else if (tabName == 'About') {\n");
	webString->print("$(\"#contentmain\").load(\"/about\");\n");
	webString->print("} else if (tabName == 'PRIME') {\n");
	webString->print("$(\"#contentmain\").load(\"/prime\");\n");
	webString->print("}\n");
	webString->print("\n");
	webString->print("if (evt != null) evt.currentTarget.className += \" active\";\n");
	webString->print("}\n");
	webString->print("if (!!window.EventSource) {");
	webString->print("var source = new EventSource('/eventHeard');");

	webString->print("source.addEventListener('open', function(e) {");
	webString->print("console.log(\"Events Connected\");");
	webString->print("}, false);");
	webString->print("source.addEventListener('error', function(e) {");
	webString->print("if (e.target.readyState != EventSource.OPEN) {");
	webString->print("console.log(\"Events Disconnected\");");
	webString->print("}\n}, false);");
	webString->print("source.addEventListener('lastHeard', function(e) {");
	// webString->print("console.log(\"lastHeard\", e.data);");
	webString->print("var lh=document.getElementById(\"aprsTable\");");
	webString->print("if(lh != null) {renderTable(e.data);}");
	webString->print("}, false);\n}");
	webString->print("if (!!window.EventSource) {");
	webString->print("var source = new EventSource('/eventMsg');");

	webString->print("source.addEventListener('open', function(e) {");
	webString->print("console.log(\"Events MSG Connected\");");
	webString->print("}, false);");
	webString->print("source.addEventListener('error', function(e) {");
	webString->print("if (e.target.readyState != EventSource.OPEN) {");
	webString->print("console.log(\"Events MSG Disconnected\");");
	webString->print("}\n}, false);");
	webString->print("source.addEventListener('chatMsg', function(e) {");
	// webString->print("console.log(\"lastHeard\", e.data);");
	webString->print("var lh=document.getElementById(\"chatMsg\");");
	webString->print("if(lh != null) {lh.innerHTML = e.data;}");
	webString->print("}, false);\n}\n");
	//webString->print("</script>\n");

	webString->print("let sortDirection = {};\n");
	webString->print("let currentSortKey = \"time\";\n\n");
	webString->print("function renderTable(raw) {\n");
	// webString->print("const tableBody = document.getElementById(\"aprsTableBody\");\n");
	// //webString->print("const tableBody = document.querySelector(\"#aprsTable tbody\");\n");
	// webString->print("if(tableBody == null) {return;}\n");
	webString->print("var data=JSON.parse(raw);\n");	
	webString->print("lastHeardSort(data);\n");
	webString->print("document.querySelectorAll(\"#aprsTable th[data-sort]\")\n");
	webString->print(".forEach(header => {\n\n");
	webString->print("header.addEventListener(\"click\", () => {\n\n");
	webString->print("const key = header.dataset.sort;\n\n");
	webString->print("sortDirection[key] = !sortDirection[key];\n");
	webString->print("currentSortKey = key;\n\n");	
	webString->print("clearArrows();\n\n");
	webString->print("const arrowSpan = header.querySelector(\".arrow\");\n");
	webString->print("arrowSpan.textContent = sortDirection[key] ? \"▲\" : \"▼\";\n\n");
	webString->print("lastHeardSort(data);\n");
	webString->print("printLastHeard(data);\n");
	webString->print("});\n\n");
	webString->print("});\n\n");
	webString->print("printLastHeard(data);\n");
	webString->print("}\n\n");

	webString->print("function iconFallback(imgEl, iconFile, initial) {\n");
webString->print("  imgEl.style.display='none';\n");
webString->print("  var span = imgEl.nextElementSibling;\n");
webString->print("  if (!span) return;\n");
webString->print("  span.style.display='inline-flex';\n");
webString->print("  span.style.alignItems='center';\n");
webString->print("  span.style.justifyContent='center';\n");
webString->print("  var code = parseInt(iconFile.split('-')[0], 10);\n");
webString->print("  var shapes = {\n");
webString->print("    62: \"<svg viewBox='0 0 24 24' width='18' height='18'><rect x='2' y='10' width='20' height='8' rx='2' fill='#1565c0'/><circle cx='7' cy='19' r='2' fill='#333'/><circle cx='17' cy='19' r='2' fill='#333'/></svg>\",\n");
webString->print("    45: \"<svg viewBox='0 0 24 24' width='18' height='18'><polygon points='12,3 22,12 19,12 19,21 5,21 5,12 2,12' fill='#8d6e63'/></svg>\",\n");
webString->print("    35: \"<svg viewBox='0 0 24 24' width='18' height='18'><polygon points='12,2 15,9 22,9 16,14 18,21 12,17 6,21 8,14 2,9 9,9' fill='#fbc02d'/></svg>\",\n");
webString->print("    95: \"<svg viewBox='0 0 24 24' width='18' height='18'><ellipse cx='12' cy='13' rx='9' ry='6' fill='#90a4ae'/></svg>\"\n");
webString->print("  };\n");
webString->print("  if (shapes[code]) {\n");
webString->print("    span.innerHTML = shapes[code];\n");
webString->print("  } else {\n");
webString->print("    span.style.width='18px'; span.style.height='18px'; span.style.borderRadius='50%';\n");
webString->print("    span.style.background='#607d8b'; span.style.color='#fff'; span.style.fontSize='11px'; span.style.fontWeight='bold';\n");
webString->print("    span.style.display='inline-flex'; span.style.alignItems='center'; span.style.justifyContent='center';\n");
webString->print("    span.textContent = (initial || '?').toUpperCase();\n");
webString->print("  }\n");
webString->print("}\n\n");

webString->print("function iconError(imgEl, iconFile, initial) {\n");
webString->print("  if (!imgEl.dataset.stage) {\n");
webString->print("    imgEl.dataset.stage = 'local';\n");
webString->print("    imgEl.src = '/symbols/icons/' + iconFile;\n");
webString->print("  } else {\n");
webString->print("    iconFallback(imgEl, iconFile, initial);\n");
webString->print("  }\n");
webString->print("}\n\n");

webString->print("function printLastHeard(data) {\n");
	webString->print("const tableBody = document.getElementById(\"aprsTableBody\");\n");
	//webString->print("const tableBody = document.querySelector(\"#aprsTable tbody\");\n");
	webString->print("if(tableBody == null) {return;}\n");
	webString->print("tableBody.innerHTML = \"\";\n");
	webString->print("data.forEach(row => {\n");
	webString->print("const tr = document.createElement(\"tr\");\n");
	webString->print("tr.innerHTML = `\n");
	webString->print("<td>${row.time}</td>\n");
	webString->print("<td><img src=\"http://aprs.nakhonthai.net/symbols/icons/${row.icon}\" style=\"width:20px;height:20px;\" onerror=\"iconError(this, '${row.icon}', '${(row.callsign||'?').charAt(0)}');\"><span style=\"display:none;width:20px;height:20px;\"></span></td>\n");
	// Clickable callsign -> aprs.fi - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
	webString->print("<td><a style=\"text-decoration:underline;\" href=\"https://www.qrz.com/db/${encodeURIComponent(row.callsign.split('-')[0])}\" target=\"_blank\" rel=\"noopener\">${row.callsign}</a> <a href=\"https://aprs.fi/#!z=12&call=a%2F${encodeURIComponent(row.callsign)}&timerange=3600&tail=3600\" target=\"_blank\" rel=\"noopener\" title=\"Ver en aprs.fi\">🗺</a></td>\n");
	webString->print("<td align=\"left\">${row.path}</td>\n");
	webString->print("<td>${row.dx !== null ? row.dx : \"-\"}</td>\n");
	webString->print("<td>${row.packet}</td>\n");
	webString->print("<td style=\"color:green;\">${row.audio !== '-' ? row.audio + \"dBV\" : \"-\"}</td>\n");
	webString->print("`;\n");
	webString->print("tableBody.appendChild(tr);\n");
	webString->print("});\n");
	webString->print("}\n\n");

	webString->print("function clearArrows() {\n");
	webString->print("document.querySelectorAll(\".arrow\").forEach(a => a.textContent = \"\");\n");
	webString->print("}\n\n");
	webString->print("function lastHeardSort(data) {\nvar key=currentSortKey;\n");
	webString->print("data.sort((a, b) => {\n\n");
	webString->print("let valA = a[key];\n");
	webString->print("let valB = b[key];\n\n");
	webString->print("if (key === \"time\") {\n");
	//webString->print("// Parse time in dd hh:mm:ss format\n");
	webString->print("const [dayTime, timePart] = valA.split(' ');\n");
	webString->print("const [hours, minutes, seconds] = timePart.split(':');\n");
	webString->print("valA = parseInt(dayTime) * 86400 + parseInt(hours) * 3600 + parseInt(minutes) * 60 + parseInt(seconds);\n");
	//webString->print("                \n");
	webString->print("const [dayTimeB, timePartB] = valB.split(' ');\n");
	webString->print("const [hoursB, minutesB, secondsB] = timePartB.split(':');\n");
	webString->print("valB = parseInt(dayTimeB) * 86400 + parseInt(hoursB) * 3600 + parseInt(minutesB) * 60 + parseInt(secondsB);\n");
	webString->print("}\n\n");
	webString->print("if (valA === null) return 1;\n");
	webString->print("if (valB === null) return -1;\n\n");
	webString->print("if (valA < valB) return sortDirection[key] ? -1 : 1;\n");
	webString->print("if (valA > valB) return sortDirection[key] ? 1 : -1;\n");
	webString->print("return 0;\n");
	webString->print("});\n\n");
	webString->print("}\n\n");
	webString->print("</script>\n");
	webString->print("</head>\n");
	//webString->print("\n");
	webString->print("<body onload=\"selectTab(event, 'DashBoard')\">\n");
	webString->print("\n");
	webString->print("<div class=\"container\">\n");
	webString->print("<div class=\"header\">\n");
	// webString->print("<div style=\"font-size: 8px; text-align: right; padding-right: 8px;\">ESP32IGate Firmware V" + String(VERSION) + "</div>\n");
	// webString->print("<div style=\"font-size: 8px; text-align: right; padding-right: 8px;\"><a href=\"/logout\">[LOG OUT]</a></div>\n");
	// LU6JMF - Marcelo Ferreira: AR-APRS header, version generated from build data.
	webString->print("<h1 style=\"margin-bottom:2px;\">AR-APRS</h1>\n");
	snprintf(temp_buffer, sizeof(temp_buffer),
			 "<div style=\"text-align:center;font-size:11px;margin-bottom:2px;\">Argentina APRS System &middot; V%s%s</div>\n",
			 VERSION, VERSION_BUILD);
	webString->print(temp_buffer);
	webString->print("<div style=\"font-size: 8px; text-align: right; padding-right: 8px;\"><a href=\"/logout\" style=\"color:#d7d7d7;\">[LOG OUT]</a></div>\n");
	webString->print("<div class=\"row\">\n");
	webString->print("<ul class=\"nav nav-tabs\" style=\"margin: 5px;\">\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'DashBoard')\">DashBoard</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'Radio')\" id=\"btnRadio\">Radio</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'IGATE')\">IGATE</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'DIGI')\">DIGI</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'TRACKER')\">TRACKER</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'WX')\">WX</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'TLM')\">TLM</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'SENSOR')\">SENSOR</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MSG')\">MSG</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'OBJ')\">OBJ</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'WiFi')\">WiFi</button>\n");
	webString->print("</ul>\n");
	webString->print("<ul class=\"nav nav-tabs\" style=\"margin: 5px;\">\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'IGTLM')\">IGTLM</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'WXCH')\">WXCH</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'SCFG')\">SCFG</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'VPN')\">VPN</button>\n");
#ifdef MQTT
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MQTT')\">MQTT</button>\n");
#endif
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MOD')\">MOD</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'IO')\">I/O</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'System')\">System</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'File')\">File</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'About')\">About</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'PRIME')\">PRIME</button>\n");
	webString->print("</ul>\n");
	webString->print("</div>\n");
	webString->print("</div>\n");
	webString->print("\n");

	webString->print("<div class=\"contentwide\" id=\"contentmain\"  style=\"font-size: 2pt;\">\n");
	webString->print("\n");
	webString->print("</div>\n");
	webString->print("<br />\n");
	webString->print("<div class=\"footer\">\n");
	// LU6JMF - Marcelo Ferreira: AR-APRS signature.
	webString->print("AR-APRS &middot; Argentina APRS System &middot; 2027<br />\n");
	webString->print("LU6JMF &mdash; Marcelo Ferreira\n");
	webString->print("<br />\n");
	webString->print("</div>\n");
	webString->print("</div>\n");
	webString->print("<!-- <script type=\"text/javascript\" src=\"/nice-select.min.js\"></script> -->\n");
	webString->print("<script type=\"text/javascript\">\n");
	webString->print("var selectize = document.querySelectorAll('select')\n");
	webString->print("var options = { searchable: true };\n");
	webString->print("selectize.forEach(function (select) {\n");
	webString->print("if (select.length > 30 && null === select.onchange && !select.name.includes(\"ExtendedId\")) {\n");
	webString->print("select.classList.add(\"small\", \"selectize\");\n");
	webString->print("tabletd = select.closest('td');\n");
	webString->print("tabletd.style.cssText = 'overflow-x:unset';\n");
	webString->print("NiceSelect.bind(select, options);\n");
	webString->print("}\n");
	webString->print("});\n");
	webString->print("</script>\n");
	webString->print("</body>\n");
	webString->print("</html>");


	webString->addHeader("Sensor", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
	lastHeardTimeout = 0;
	lastHeard_Flag = true;
}

////////////////////////////////////////////////////////////
// handler for web server request: http://IpAddress/      //
////////////////////////////////////////////////////////////

void handle_css(AsyncWebServerRequest *request)
{
	// LU6JMF - Marcelo Ferreira:
	// Fase 6 (tirar HTML/CSS do codigo pra templates): style.css agora vive em
	// LittleFS (data/style.css) e e servido direto dali quando presente. Um
	// device que so recebeu o .bin por OTA (sem subir esse arquivo pela pagina
	// Storage) ainda nao tem esse arquivo no filesystem - nesse caso cai pro
	// texto embutido abaixo, que fica igual ao style.css de hoje. Sem esse
	// fallback a pagina ficaria sem estilo ate alguem subir o arquivo.
	if (LITTLEFS.exists("/style.css"))
	{
		request->send(LITTLEFS, "/style.css", "text/css");
		return;
	}
	const char *css = ".container{width:900px;text-align:left;margin:auto;border-radius:10px 10px 10px 10px;-moz-border-radius:10px 10px 10px 10px;-webkit-border-radius:10px 10px 10px 10px;-khtml-border-radius:10px 10px 10px 10px;-ms-border-radius:10px 10px 10px 10px;box-shadow:3px 3px 3px #707070;background:#fff;border-color: #171717;padding: 0px;border-width: 5px;border-style:solid;}body,font{font:12px verdana,arial,sans-serif;color:#fff}.header{background:#171717;text-decoration:none;color:#fff;font-family:verdana,arial,sans-serif;text-align:left;padding:5px 0;border-radius:10px 10px 0 0;-moz-border-radius:10px 10px 0 0;-webkit-border-radius:10px 10px 0 0;-khtml-border-radius:10px 10px 0 0;-ms-border-radius:10px 10px 0 0}.content{margin:0 0 0 166px;padding:1px 5px 5px;color:#000;background:#fff;text-align:center;font-size: 8pt;}.contentwide{padding:50px 5px 5px;color:#000;background:#fff;text-align:center}.contentwide h2{color:#000;font:1em verdana,arial,sans-serif;text-align:center;font-weight:700;padding:0;margin:0;font-size: 12pt;}.footer{background:#171717;text-decoration:none;color:#fff;font-family:verdana,arial,sans-serif;font-size:9px;text-align:center;padding:10px 0;border-radius:0 0 10px 10px;-moz-border-radius:0 0 10px 10px;-webkit-border-radius:0 0 10px 10px;-khtml-border-radius:0 0 10px 10px;-ms-border-radius:0 0 10px 10px;clear:both}#tail{height:450px;width:805px;overflow-y:scroll;overflow-x:scroll;color:#0f0;background:#000}table{vertical-align:middle;text-align:center;empty-cells:show;padding-left:3;padding-right:3;padding-top:3;padding-bottom:3;border-collapse:collapse;border-color:#263B43;border-style:solid;border-spacing:0px;border-width:3px;text-decoration:none;color:#fff;background:#000;font-family:verdana,arial,sans-serif;font-size : 12px;width:100%;white-space:nowrap}table th{cursor: pointer;user-select: none;font-size: 10pt;font-family:lucidia console,Monaco,monospace;text-shadow:1px 1px #0e038c;text-decoration:none;background:#1F4E5F;border:1px solid silver}table tr:nth-child(even){background:#f7f7f7}table tr:nth-child(odd){background:#eeeeee}table td{color:#000;font-family:lucidia console,Monaco,monospace;text-decoration:none;border:1px solid #263B43}body{background:#edf0f5;color:#000}a{text-decoration:none}a:link,a:visited{text-decoration:none;color:#0000e0;font-weight:400}th:last-child a.tooltip:hover span{left:auto;right:0}ul{padding:5px;margin:10px 0;list-style:none;float:left}ul li{float:left;display:inline;margin:0 10px}ul li a{text-decoration:none;float:left;color:#999;cursor:pointer;font:900 14px/22px arial,Helvetica,sans-serif}ul li a span{margin:0 10px 0 -10px;padding:1px 8px 5px 18px;position:relative;float:left}h1{text-shadow:2px 2px #303030;text-align:center}.toggle{position:absolute;margin-left:-9999px;visibility:hidden}.toggle+label{display:block;position:relative;cursor:pointer;outline:none}input.toggle-round-flat+label{padding:1px;width:33px;height:18px;background-color:#ddd;border-radius:10px;transition:background .4s}input.toggle-round-flat+label:before,input.toggle-round-flat+label:after{display:block;position:absolute;}input.toggle-round-flat+label:before{top:1px;left:1px;bottom:1px;right:1px;background-color:#fff;border-radius:10px;transition:background .4s}input.toggle-round-flat+label:after{top:2px;left:2px;bottom:2px;width:16px;background-color:#ddd;border-radius:12px;transition:margin .4s,background .4s}input.toggle-round-flat:checked+label{background-color:#dd4b39}input.toggle-round-flat:checked+label:after{margin-left:14px;background-color:#dd4b39}@-moz-document url-prefix(){select,input{margin:0;padding:0;border-width:1px;font:12px verdana,arial,sans-serif}input[type=button],button,input[type=submit]{padding:0 3px;border-radius:3px 3px 3px 3px;-moz-border-radius:3px 3px 3px 3px}}.nice-select.small,.nice-select-dropdown li.option{height:24px!important;min-height:24px!important;line-height:24px!important}.nice-select.small ul li:nth-of-type(2){clear:both}.nav{margin-bottom:0;padding-left:10;list-style:none}.nav>li{position:relative;display:block}.nav>li>a{position:relative;display:block;padding:5px 10px}.nav>li>a:hover,.nav>li>a:focus{text-decoration:none;background-color:#eee}.nav>li.disabled>a{color:#999}.nav>li.disabled>a:hover,.nav>li.disabled>a:focus{color:#999;text-decoration:none;background-color:initial;cursor:not-allowed}.nav .open>a,.nav .open>a:hover,.nav .open>a:focus{background-color:#eee;border-color:#428bca}.nav .nav-divider{height:1px;margin:9px 0;overflow:hidden;background-color:#e5e5e5}.nav>li>a>img{max-width:none}.nav-tabs{border-bottom:1px solid #ddd}.nav-tabs>li{float:left;margin-bottom:-1px}.nav-tabs>li>a{margin-right:0;line-height:1.42857143;border:1px solid #ddd;border-radius:10px 10px 0 0}.nav-tabs>li>a:hover{border-color:#eee #eee #ddd}.nav-tabs>button{margin-right:0;line-height:1.42857143;border:2px solid #ddd;border-radius:10px 10px 0 0;padding:1px 4px;font-size:12px;flex:1 1 auto;min-width:0;white-space:nowrap}.nav-tabs>button:hover{background-color:#25bbfc;border-color:#428bca;color:#eaf2f9;border-bottom-color:transparent;}.nav-tabs>button.active,.nav-tabs>button.active:hover,.nav-tabs>button.active:focus{color:#f7fdfd;background-color:#1aae0d;border:1px solid #ddd;border-bottom-color:transparent;cursor:default}.nav-tabs>li.active>a,.nav-tabs>li.active>a:hover,.nav-tabs>li.active>a:focus{color:#428bca;background-color:#e5e5e5;border:1px solid #ddd;border-bottom-color:transparent;cursor:default}.nav-tabs.nav-justified{width:100%;border-bottom:0}.nav-tabs.nav-justified>li{float:none}.nav-tabs.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-tabs.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}.nav-status{float:left;margin:0;padding:3px;width:160px;font-weight:400;min-height:600}#bar,#prgbar {background-color: #f1f1f1;border-radius: 14px}#bar {background-color: #3498db;width: 0%;height: 14px}.switch{position:relative;display:inline-block;width:34px;height:16px}.switch input{opacity:0;width:0;height:0}.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#f55959;-webkit-transition:.4s;transition:.4s}.slider:before{position:absolute;content:\"\";height:12px;width:12px;left:2px;bottom:2px;background-color:#fff;-webkit-transition:.4s;transition:.4s}input:checked+.slider{background-color:#5ca30a}input:focus+.slider{box-shadow:0 0 1px #5ca30a}input:checked+.slider:before{-webkit-transform:translateX(16px);-ms-transform:translateX(16px);transform:translateX(16px)}.slider.round{border-radius:34px}.slider.round:before{border-radius:50%}.button{border:1px solid #06c;background-color:#09c;color:#fff;padding:5px 10px;border-radius: 3px}.button:hover{border:1px solid #09c;background-color:#0ac;color:#fff}.button:disabled,button[disabled]{border:1px solid #999;background-color:#ccc;color:#666}.arrow {margin-left: 5px;font-size: 12px;}\n";
	request->send_P(200, "text/css", css);
}

void handle_jquery(AsyncWebServerRequest *request)
{
#if defined(CONFIG_IDF_TARGET_ESP32)
	adcEn = -1;
	dacEn = -1;
	delay(100);
#endif
	AsyncWebServerResponse *response = request->beginResponse_P(200, "application/javascript", (const uint8_t *)jquery_3_7_1_min_js_gz, jquery_3_7_1_min_js_gz_len);
	response->addHeader("Content-Encoding", "gzip");
	response->addHeader("Cache-Control", "no-cache");
	response->setContentLength(jquery_3_7_1_min_js_gz_len);
	request->send(response);
#if defined(CONFIG_IDF_TARGET_ESP32)
	delay(200);
	adcEn = 1;
	dacEn = 0;
#endif
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
static const char *DASHBOARD_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"function reloadSysInfo() {\n"
	"$(\"#sysInfo\").load(\"/sysinfo\", function () { setTimeout(reloadSysInfo, 60000) });\n"
	"}\n"
	"setTimeout(reloadSysInfo(), 100);\n"
	"function reloadSidebarInfo() {\n"
	"$(\"#sidebarInfo\").load(\"/sidebarInfo\", function () { setTimeout(reloadSidebarInfo, 10000) });\n"
	"}\n"
	"setTimeout(reloadSidebarInfo, 1000);\n"
	"$(window).trigger('resize');\n"
	"</script>\n"
	"<div id=\"sysInfo\">\n</div>\n"
	"<br />\n"
	"<div class=\"nav-status\">\n"
	"<div id=\"sidebarInfo\">\n</div>\n"
	"<br />\n"
	"<table>\n<tr>\n<th colspan=\"2\">Radio Info</th>\n</tr>\n"
	"{{#if rf}}"
	"<tr>\n<td>Freq.TX</td>\n<td style=\"background: #ffffff;\">{{freqTx}} MHz</td>\n</tr>\n"
	"<tr>\n<td>Freq.RX</td>\n<td style=\"background: #ffffff;\">{{freqRx}} MHz</td>\n</tr>\n"
	"<tr>\n<td>TX PWR</td>\n<td>{{txPower}}</td>\n</tr>\n"
	"{{/if}}"
	"<tr>\n<td>MODEM</td>\n<td style=\"background: #ffffff;\">{{modem}}</td>\n</tr>\n"
	"<tr>\n<td>FX.25</td>\n<td style=\"background: #ffffff;\">{{fx25}}</td>\n</tr>\n"
	"</table>\n\n"
	"{{#if igate}}"
	"<br />\n<table>\n<tr>\n<th colspan=\"2\">APRS-IS SERVER</th>\n</tr>\n"
	"<tr>\n<td>HOST</td>\n<td style=\"background: #ffffff;\">{{igate.host}}</td>\n</tr>\n"
	"<tr>\n<td>PORT</td>\n<td style=\"background: #ffffff;\">{{igate.port}}</td>\n</tr>\n"
	"</table>\n"
	"{{/if}}"
	"<br />\n<table>\n<tr>\n<th colspan=\"2\">WiFi</th>\n</tr>\n"
	"<tr>\n<td>MODE</td>\n<td style=\"background: #ffffff;\">{{wifiMode}}</td>\n</tr>\n"
	"<tr>\n<td>SSID</td>\n<td style=\"background: #ffffff;\">{{ssid}}</td>\n</tr>\n"
	"<tr>\n<td>RSSI</td>\n"
	"{{#if wifiConnected}}"
	"<td style=\"background: #ffffff;\">{{rssi}} dBm</td>\n"
	"{{else}}"
	"<td style=\"background:#606060; color:#b0b0b0;\" aria-disabled=\"true\">Disconnect</td>\n"
	"{{/if}}"
	"</tr>\n</table>\n<br />\n"
	"{{#if bluetooth}}"
	"<table>\n<tr>\n<th colspan=\"2\">Bluetooth</th>\n</tr>\n"
	"<td>Master</td>\n"
	"{{#if bluetooth.master}}"
	"<td style=\"background:#0b0; color:#030; width:50%;\">Enabled</td>\n"
	"{{else}}"
	"<td style=\"background:#606060; color:#b0b0b0;\" aria-disabled=\"true\">Disabled</td>\n"
	"{{/if}}"
	"</tr>\n<tr>\n<tr>\n<td>NAME</td>\n<td style=\"background: #ffffff;\">{{bluetooth.name}}</td>\n</tr>\n"
	"<tr>\n<tr>\n<td>MODE</td>\n<td style=\"background: #ffffff;\">{{bluetooth.mode}}</td>\n</tr>\n<tr>\n</table>\n"
	"{{/if}}"
	"</div>\n</div>\n\n"
	"<div class=\"content\">\n<div id=\"lastHeard\">\n"
	"<table id=\"aprsTable\">\n<thread>\n"
	"<th colspan=\"7\" style=\"background-color: #070ac2;\">LAST HEARD <a href=\"/tnc2\" target=\"_tnc2\" style=\"color: yellow;font-size:8pt\">[RAW]</a></th>\n"
	"<tr>\n<th data-sort=\"time\" style=\"min-width:10ch\"><span><b>Time ({{tzStr}})</b></span><span class=\"arrow\"></span></th>\n"
	"<th style=\"min-width:16px\">ICON</th>\n"
	"<th data-sort=\"callsign\" style=\"min-width:10ch\">Callsign<span class=\"arrow\"></span></th>\n"
	"<th>VIA LAST PATH</th>\n"
	"<th data-sort=\"dx\" style=\"min-width:5ch\">DX<span class=\"arrow\"></span></th>\n"
	"<th data-sort=\"packet\" style=\"min-width:5ch\">PACKET<span class=\"arrow\"></span></th>\n"
	"<th data-sort=\"audio\" style=\"min-width:5ch\">AUDIO<span class=\"arrow\"></span></th>\n"
	"</tr></thread>\n<tbody id=\"aprsTableBody\"></tbody>\n"
	"</table>\n</div>\n";

void handle_dashboard(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	char temp_buffer[200];
	// if (!request->authenticate(config.http_username, config.http_password))
	// {
	// 	return request->requestAuthentication();
	// }
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	JsonDocument data;

	data["rf"] = config.rf_en; // truthy flag for {{#if rf}}
	snprintf(temp_buffer, sizeof(temp_buffer), "%.4f", config.freq_tx);
	data["freqTx"] = temp_buffer;
	snprintf(temp_buffer, sizeof(temp_buffer), "%.4f", config.freq_rx);
	data["freqRx"] = temp_buffer;
	data["txPower"] = config.rf_power ? "HIGH" : "LOW";

	data["modem"] = MODEM_TYPE[config.modem_type];
	data["fx25"] = FX25_MODE[config.fx25_mode];

	if (config.igate_en)
	{
		JsonObject igate = data["igate"].to<JsonObject>();
		igate["host"] = config.aprs_host;
		igate["port"] = config.aprs_port;
	}

	const char *strWiFiMode = "OFF";
	if (config.wifi_mode == WIFI_STA_FIX) strWiFiMode = "STA";
	else if (config.wifi_mode == WIFI_AP_FIX) strWiFiMode = "AP";
	else if (config.wifi_mode == WIFI_AP_STA_FIX) strWiFiMode = "AP+STA";
	data["wifiMode"] = strWiFiMode;
	data["ssid"] = WiFi.SSID();
	data["wifiConnected"] = WiFi.isConnected();
	if (WiFi.isConnected())
		data["rssi"] = WiFi.RSSI();

#ifdef BLUETOOTH
	JsonObject bt = data["bluetooth"].to<JsonObject>();
	bt["master"] = config.bt_master;
	bt["name"] = config.bt_name;
	if (config.bt_mode == 1) bt["mode"] = "TNC2";
	else if (config.bt_mode == 2) bt["mode"] = "KISS";
	else bt["mode"] = "NONE";
#endif

	if (config.timeZone >= 0)
	{
		if (config.timeZone == (int)config.timeZone)
			snprintf(temp_buffer, sizeof(temp_buffer), "+%d", (int)config.timeZone);
		else
			snprintf(temp_buffer, sizeof(temp_buffer), "+%.1f", config.timeZone);
	}
	else
	{
		if (config.timeZone == (int)config.timeZone)
			snprintf(temp_buffer, sizeof(temp_buffer), "%d", (int)config.timeZone);
		else
			snprintf(temp_buffer, sizeof(temp_buffer), "%.1f", config.timeZone);
	}
	data["tzStr"] = temp_buffer;

	String html = renderTemplate(DASHBOARD_TEMPLATE, data.as<JsonVariant>());
	sendHtmlChunked(request, html, "dashboard");
	lastHeardTimeout = millis() + 500;
	lastHeard_Flag = true;
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
// Nota: o rewrite corrigiu um bug de sintaxe pre-existente no bloco #ifdef MQTT
// (um "print(...;);" com ponto-e-virgula sobrando dentro dos parenteses -
// nunca compilava porque MQTT nao estava definido em nenhum build atual, por
// isso nunca deu erro visivel) - a versao em template nao carrega esse bug.
static const char *SIDEBAR_TEMPLATE =
	"<table style=\"background:white;border-collapse: unset;\">\n"
	"<tr>\n<th colspan=\"2\">Modes Enabled</th>\n</tr>\n<tr>\n"
	"{{#if igate}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">IGATE</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">IGATE</th>\n{{/if}}"
	"{{#if digi}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">DIGI</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">DIGI</th>\n{{/if}}"
	"</tr>\n<tr>\n"
	"{{#if wx}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">WX</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">WX</th>\n{{/if}}"
	"{{#if tracker}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">TRACKER</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">TRACKER</th>\n{{/if}}"
	"</tr>\n</table>\n<br />\n"
	"<table style=\"background:white;border-collapse: unset;\">\n"
	"<tr>\n<th colspan=\"2\">Network Status</th>\n</tr>\n<tr>\n"
	"{{#if aprsis}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">APRS-IS</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">APRS-IS</th>\n{{/if}}"
	"{{#if vpn}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">VPN</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">VPN</th>\n{{/if}}"
	"</tr>\n<tr>\n"
	"{{#if pppos}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">PPPoS</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">PPPoS</th>\n{{/if}}"
	"{{#if mqttCompiled}}"
	"{{#if mqtt}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">MQTT</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">MQTT</th>\n{{/if}}"
	"{{/if}}"
	"{{#if fx25}}<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">FX.25</th>\n"
	"{{else}}<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">FX.25</th>\n{{/if}}"
	"</tr>\n</table>\n<br />\n"
	"<table>\n<tr>\n<th colspan=\"2\">STATISTICS</th>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">RADIO RX:</td>\n<td style=\"background: #ffffff;\">{{rxCount}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">PACKET RX:</td>\n<td style=\"background: #ffffff;\">{{allCount}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">PACKET TX:</td>\n<td style=\"background: #ffffff;\">{{txCount}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">RF2INET:</td>\n<td style=\"background: #ffffff;\">{{rf2inet}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">INET2RF:</td>\n<td style=\"background: #ffffff;\">{{inet2rf}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">DIGI:</td>\n<td style=\"background: #ffffff;\">{{digiCount}}</td>\n</tr>\n"
	"<tr>\n<td style=\"width: 60px;text-align: right;\">DROP/ERR:</td>\n<td style=\"background: #ffffff;\">{{dropCount}}/{{errorCount}}</td>\n</tr>\n"
	"</table>\n<br />\n"
	"{{#if gnss}}"
	"<table>\n<tr>\n<th colspan=\"2\">GPS Info <a href=\"/gnss\" target=\"_gnss\" style=\"color: yellow;font-size:8pt\">[View]</a></th>\n</tr>\n"
	"<tr>\n<td>LAT:</td>\n<td style=\"background: #ffffff;text-align: left;\">{{gnss.lat}}</td>\n</tr>\n"
	"<tr>\n<td>LON:</td>\n<td style=\"background: #ffffff;text-align: left;\">{{gnss.lon}}</td>\n</tr>\n"
	"<tr>\n<td>ALT:</td>\n<td style=\"background: #ffffff;text-align: left;\">{{gnss.alt}}</td>\n</tr>\n"
	"<tr>\n<td>SAT:</td>\n<td style=\"background: #ffffff;text-align: left;\">{{gnss.sat}}</td>\n</tr>\n"
	"</table>\n"
	"{{/if}}"
	"<script>\n$(window).trigger('resize');\n</script>\n";

void handle_sidebar(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	JsonDocument data;
	data["igate"] = config.igate_en;
	data["digi"] = config.digi_en;
	data["wx"] = config.wx_en;
	data["tracker"] = config.trk_en;
	data["aprsis"] = aprsClient.connected();
	data["vpn"] = wireguard_active();
#ifdef PPPOS
	data["pppos"] = PPP.connected();
#else
	data["pppos"] = false;
#endif
#ifdef MQTT
	data["mqttCompiled"] = true;
	data["mqtt"] = clientMQTT.connected();
#else
	data["mqttCompiled"] = false;
#endif
	data["fx25"] = config.fx25_mode > 0;

	data["rxCount"] = status.rxCount;
	data["allCount"] = status.allCount;
	data["txCount"] = status.txCount;
	data["rf2inet"] = status.rf2inet;
	data["inet2rf"] = status.inet2rf;
	data["digiCount"] = status.digiCount;
	data["dropCount"] = status.dropCount;
	data["errorCount"] = status.errorCount;

	if (config.gnss_enable)
	{
		JsonObject gnss = data["gnss"].to<JsonObject>();
		char buf[32];
		snprintf(buf, sizeof(buf), "%f", gps.location.lat());
		gnss["lat"] = buf;
		snprintf(buf, sizeof(buf), "%f", gps.location.lng());
		gnss["lon"] = buf;
		snprintf(buf, sizeof(buf), "%f", gps.altitude.meters());
		gnss["alt"] = buf;
		gnss["sat"] = gps.satellites.value();
	}

	String html_str = renderTemplate(SIDEBAR_TEMPLATE, data.as<JsonVariant>());
	sendHtmlChunked(request, html_str, "Sidebar");
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
// Loop de linhas/colunas virou {{#each}} aninhado (tables -> rows -> cells);
// a quebra de linha a cada 16 colunas e' feita em C++ ao montar o JSON (o
// motor nao tem aritmetica de modulo na sintaxe do template, entao a divisao
// em linhas continua sendo decidida no C++, so a impressao virou declarativa).
static const char *SYMBOL_TEMPLATE =
	"{{#each tables}}"
	"<table border=\"1\" align=\"center\">\n"
	"<tr><th colspan=\"16\">{{title}}</th></tr>\n"
	"{{#each rows}}"
	"<tr>\n"
	"{{#each this}}<td><img onclick=\"{{onclick}}\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{icon}}\"></td>{{/each}}"
	"</tr>\n"
	"{{/each}}"
	"</table>\n<br />"
	"{{/each}}";

void handle_symbol(AsyncWebServerRequest *request)
{
	int sel = -1;
	for (int i = 0; i < request->args(); i++)
	{
		if (request->argName(i) == "sel")
		{
			if (request->arg(i) != "" && isValidNumber(request->arg(i)))
			{
				sel = request->arg(i).toInt();
			}
		}
	}

	JsonDocument data;
	JsonArray tables = data["tables"].to<JsonArray>();
	char buf[64];
	for (int suffix = 1; suffix <= 2; suffix++)
	{
		JsonObject table = tables.add<JsonObject>();
		table["title"] = (suffix == 1) ? "Table '/'" : "Table '\\'";
		JsonArray rows = table["rows"].to<JsonArray>();
		JsonArray row = rows.add<JsonArray>();
		for (int i = 33; i < 129; i++)
		{
			JsonObject cell = row.add<JsonObject>();
			if (sel == -1)
				snprintf(buf, sizeof(buf), "window.opener.setValue(%d,%d);", i, suffix);
			else
				snprintf(buf, sizeof(buf), "window.opener.setValue(%d,%d,%d);", sel, i, suffix);
			cell["onclick"] = buf;
			snprintf(buf, sizeof(buf), "%d-%d.png", i, suffix);
			cell["icon"] = buf;

			if (((i % 16) == 0) && (i < 126))
				row = rows.add<JsonArray>();
		}
	}

	String html_str = renderTemplate(SYMBOL_TEMPLATE, data.as<JsonVariant>());
	AsyncResponseStream *web = request->beginResponseStream("text/html", html_str.length() + 200);
	if (web)
	{
		web->print(html_str);
		web->addHeader("Symbol", "content");
		web->addHeader("Cache-Control", "no-cache");
		request->send(web);
	}
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
static const char *SYSINFO_TEMPLATE =
	"<table style=\"table-layout: fixed;border-collapse: unset;border-radius: 10px;border-color: #ee800a;border-style: ridge;border-spacing: 1px;border-width: 4px;background: #ee800a;\">\n"
	"<tr>\n"
	"<th><span><b>Up Time</b></span></th>\n"
	"<th><span>RAM(KByte)</span></th>\n"
	"{{#if psram}}<th><span>PSRAM(KByte)</span></th>\n{{/if}}"
	"<th><span>SPIFFS(KByte)</span></th>\n"
	"{{#if vbat}}<th><span>VBat(V)</span></th>\n{{/if}}"
	"<th><span>CPU(Mhz)</span></th>\n"
	"<th><span>CPU.Temp(\xc2\xb0" "C)</span></th>\n"
	"</tr>\n<tr>\n"
	"<td><b>{{upTime}}</b></td>\n"
	"<td><b>{{ram}}</b></td>\n"
	"{{#if psram}}<td><b>{{psram}}</b></td>\n{{/if}}"
	"<td><b>{{spiffs}}</b></td>\n"
	"{{#if vbat}}<td><b>{{vbat}}</b></td>\n{{/if}}"
	"<td><b>{{cpuMhz}}</b></td>\n"
	"<td><b>{{cpuTemp}}</b></td>\n"
	"</tr>\n</table>\n";

void handle_sysinfo(AsyncWebServerRequest *request)
{
	JsonDocument data;
	char temp_buffer[64];

	char strTime[20];
	convertSecondsToDHMS(strTime, (millis() / 1000) - upTimeStamp);
	data["upTime"] = strTime;

	snprintf(temp_buffer, sizeof(temp_buffer), "%.1f/%.1f", (float)ESP.getFreeHeap() / 1000, (float)ESP.getHeapSize() / 1000);
	data["ram"] = temp_buffer;

#ifdef BOARD_HAS_PSRAM
	snprintf(temp_buffer, sizeof(temp_buffer), "%.1f/%.1f", (float)ESP.getFreePsram() / 1000, (float)ESP.getPsramSize() / 1000);
	data["psram"] = temp_buffer;
#endif

	unsigned long cardTotal = LITTLEFS.totalBytes();
	unsigned long cardUsed = LITTLEFS.usedBytes();
	snprintf(temp_buffer, sizeof(temp_buffer), "%.1f/%.1f", (double)cardUsed / 1024, (double)cardTotal / 1024);
	data["spiffs"] = temp_buffer;

	if (VBat_Flag)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.2f", VBat);
		data["vbat"] = temp_buffer;
	}

	data["cpuMhz"] = ESP.getCpuFreqMHz();

	// CPU temp dashboard fix - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
	// The ESPCPUTemp lib refuses classic ESP32 (D0WD/D0WD-V3) outright ("not found" - unsupported
	// by the official IDF driver on this chip), so we read the raw Arduino core sensor directly
	// instead, same as the earlier v1.6H build did. Not officially calibrated on classic ESP32,
	// but gives a usable relative reading instead of a permanent N/A.
	float cpuTempC = temperatureRead();
	if (!isnan(cpuTempC) && cpuTempC > -40.0f && cpuTempC < 125.0f)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.1f", cpuTempC);
		data["cpuTemp"] = temp_buffer;
	}
	else
	{
		data["cpuTemp"] = "N/A";
	}

	String html_str = renderTemplate(SYSINFO_TEMPLATE, data.as<JsonVariant>());
	AsyncWebServerResponse *response = request->beginResponse(200, "text/html", html_str);
	response->addHeader("Sysinfo", "content");
	response->addHeader("Cache-Control", "no-cache");
	request->send(response);
}

// Copy src into dest escaping characters that would break JSON string syntax
// (", \) and stripping raw control bytes, so text taken from a TNC2 packet
// (callsign, path, object/item name) can be embedded safely inside a JSON string.
static void jsonEscapeCopy(char *dest, size_t destSize, const char *src)
{
	size_t j = 0;
	for (size_t i = 0; src[i] != '\0' && j + 1 < destSize; i++)
	{
		unsigned char c = (unsigned char)src[i];
		if (c == '"' || c == '\\')
		{
			if (j + 2 >= destSize)
				break;
			dest[j++] = '\\';
			dest[j++] = (char)c;
		}
		else if (c < 0x20)
		{
			continue; // strip control bytes
		}
		else
		{
			dest[j++] = (char)c;
		}
	}
	dest[j] = '\0';
}

void event_lastHeard(bool gethtml)
{
	// log_d("Event count: %d",lastheard_events.count());
	// if (lastheard_events.count() == 0)
	//	return;

	struct pbuf_t aprs;
	ParseAPRS aprsParse;
	struct tm tmstruct, tmNow;

	// Using dynamic memory allocation instead of String
	// Sized to fit a fully-escaped path/LPath (up to 256 raw chars -> 512 escaped) plus JSON key/quote overhead
	char temp_html[600];
	char *html = allocateStringMemory(16384); // Initial buffer size, adjust as needed
	if (html==nullptr)
	{
		return; // Memory allocation failed
	}
	time_t timeNow = time(NULL);

	// log_d("Create html last heard");
	localtime_r(&timeNow, &tmNow);
//strcat(webString, "  { time: \"21:54:23\", icon: \"91-1.png\", callsign: \"HS5TQA-7\", path: \"RF: WIDE1-1\", dx: 0.0, packet: 2, audio: -19.6 },\n");
	strcpy(html, "[");
	for (int i = 0; i < PKGLISTSIZE; i++)
	{
		if (i >= PKGLISTSIZE)
			break;
		pkgListType pkg = getPkgList(i);
		if (pkg.time > 0)
		{
			// if (pkg.raw == nullptr || pkg.length == 0)
			// 	continue;
			// bool validText = true;
			// for (size_t ci = 0; ci < pkg.length-1; ci++)
			// {
			// 	//if (!isprint((unsigned char)pkg.raw[ci]))
			// 	if((unsigned char)pkg.raw[ci] < 32 || (unsigned char)pkg.raw[ci] > 126)
			// 	{
			// 		validText = false;
			// 		break;
			// 	}
			// }
			// if (!validText)
			// 	continue;

			int packet = pkg.pkg;
			char *pos_gt = strchr(pkg.raw, '>'); // Find first position of '>'
			char *pos_colon = strchr(pkg.raw, ':');
			if(pos_gt == nullptr || pos_colon == nullptr)
				continue;
			if(pos_colon < pos_gt)
				continue;
			int start_val = pos_gt ? (pos_gt - pkg.raw) : -1;
			if (start_val > 3 && start_val < 10)
			{
				// Extract src_call substring
				char src_call[11];
				strncpy(src_call, pkg.raw, start_val);
				src_call[start_val] = '\0';
				memset(&aprs, 0, sizeof(pbuf_t));
				aprs.buf_len = 300;
				aprs.packet_len = pkg.length;
				strncpy((char *)aprs.data, pkg.raw, aprs.packet_len < sizeof(aprs.data) ? aprs.packet_len : sizeof(aprs.data) - 1);

				//char *pos_colon = strchr(pkg.raw, ':');
				char *pos_comma = strchr(pkg.raw, ',');
				char *pos_gt2 = strstr(pkg.raw + 2, ">"); // Find '>' starting from position 2
				char *pos_dash = pos_gt2 ? strchr(pos_gt2, '-') : NULL;

				int start_info = pos_colon ? (pos_colon - pkg.raw) : -1;
				int end_ssid = pos_comma ? (pos_comma - pkg.raw) : -1;
				int start_dst = pos_gt2 ? (pos_gt2 - pkg.raw) : -1;
				int start_dstssid = pos_dash ? (pos_dash - pkg.raw) : -1;

				char path[256] = "";

				if ((end_ssid > start_dst) && (end_ssid < start_info) && (end_ssid < (int)strlen(pkg.raw)))
				{
					int path_len = start_info - end_ssid - 1;
					strncpy(path, pkg.raw + end_ssid + 1, path_len);
					path[path_len] = '\0';
				}
				if (end_ssid < 5)
					end_ssid = start_info;
				if ((start_dstssid > start_dst) && (start_dstssid < start_dst + 10))
				{
					aprs.dstcall_end_or_ssid = &aprs.data[start_dstssid];
				}
				else
				{
					aprs.dstcall_end_or_ssid = &aprs.data[end_ssid];
				}
				aprs.info_start = &aprs.data[start_info + 1];
				aprs.dstname = &aprs.data[start_dst + 1];
				aprs.dstname_len = end_ssid - start_dst;
				aprs.dstcall_end = &aprs.data[end_ssid];
				aprs.srccall_end = &aprs.data[start_dst];

				// Serial.println(aprs.info_start);
				if (aprsParse.parse_aprs(&aprs))
				{
					pkg.calsign[10] = 0;
					// time_t tm = pkg.time;
					localtime_r(&pkg.time, &tmstruct);
					char strTime[20];
					//if (tmNow.tm_mday == tmstruct.tm_mday)
					//	sprintf(strTime, "%02d:%02d:%02d", tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
					//else
						sprintf(strTime, "%02d %02d:%02d:%02d", tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
					// String str = String(tmstruct.tm_hour, DEC) + ":" + String(tmstruct.tm_min, DEC) + ":" + String(tmstruct.tm_sec, DEC);

					// Append to html
					// strcat(html, "  { time: \"21:54:23\", icon: \"91-1.png\", callsign: \"HS5TQA-7\", path: \"RF: WIDE1-1\", dx: 0.0, packet: 2, rssi: -19.6 },\n");
					
					snprintf(temp_html, sizeof(temp_html), "{\"time\":\"%s\",", strTime);
					strcat(html, temp_html);
					char fileImg[64] = "";
					uint8_t sym = (uint8_t)aprs.symbol[1];
					if (sym > 31 && sym < 127)
					{
						if (aprs.symbol[0] > 64 && aprs.symbol[0] < 91) // table A-Z
						{
							snprintf(fileImg, sizeof(fileImg), "%d", sym);
							// if (aprs.symbol[0] == 92)
							// {
							// 	strcat(fileImg, "-2.png");
							// }
							// else if (aprs.symbol[0] == 47)
							// {
								strcat(fileImg, "-1.png");
							//}

							//snprintf(temp_html, sizeof(temp_html), "<td><b>%c</b></td>", aprs.symbol[0]);
							snprintf(temp_html, sizeof(temp_html), "\"icon\":\"%s\",", fileImg);
							strcat(html, temp_html);
						}
						else
						{
							snprintf(fileImg, sizeof(fileImg), "%d", sym);
							if (aprs.symbol[0] == 92)
							{
								strcat(fileImg, "-2.png");
							}
							else if (aprs.symbol[0] == 47)
							{
								strcat(fileImg, "-1.png");
							}
							else
							{
								strcpy(fileImg, "dot.png");
							}
							//snprintf(temp_html, sizeof(temp_html), "<td><img src=\"http://aprs.nakhonthai.net/symbols/icons/%s\"></td>", fileImg);
							snprintf(temp_html, sizeof(temp_html), "\"icon\":\"%s\",", fileImg);
							strcat(html, temp_html);
						}
					}
					else
					{
						//strcat(html, "<td><img src=\"http://aprs.nakhonthai.net/symbols/icons/dot.png\"></td>");
						strcat(html, "\"icon\":\"dot.png\",");
					}
					
					char src_call_esc[23];
					jsonEscapeCopy(src_call_esc, sizeof(src_call_esc), src_call);
					if (aprs.srcname_len > 0 && aprs.srcname_len < 10) // Get Item/Object
					{
						char itemname[10];
						memset(&itemname, 0, 10);
						memcpy(&itemname, aprs.srcname, aprs.srcname_len);
						char itemname_esc[21];
						jsonEscapeCopy(itemname_esc, sizeof(itemname_esc), itemname);
						snprintf(temp_html, sizeof(temp_html), "\"callsign\":\"%s(%s)\",", itemname_esc, src_call_esc);
						strcat(html, temp_html);
					}else{
						snprintf(temp_html, sizeof(temp_html), "\"callsign\":\"%s\",", src_call_esc);
						strcat(html, temp_html);
					}
					//strcat(html, "</td>");
					if (strlen(path) == 0)
					{
						strcat(html, "\"path\":\"RF: DIRECT\",");
					}
					else
					{
						// Find last occurrence of ','
						char *last_comma = strrchr(path, ',');
						char LPath[256] = "";
						if (last_comma != NULL)
						{
							strncpy(LPath, last_comma + 1, sizeof(LPath) - 1);
							LPath[sizeof(LPath) - 1] = '\0';
						}
						else
						{
							strncpy(LPath, path, sizeof(LPath) - 1);
							LPath[sizeof(LPath) - 1] = '\0';
						}
						char path_esc[513];
						// if(path.indexOf("qAR")>=0 || path.indexOf("qAS")>=0 || path.indexOf("qAC")>=0){ //Via from Internet Server
						if (strstr(path, "qA") != NULL || strstr(path, "TCPIP") != NULL)
						{
							jsonEscapeCopy(path_esc, sizeof(path_esc), LPath);
							snprintf(temp_html, sizeof(temp_html), "\"path\":\"INET:%s\",", path_esc);
							strcat(html, temp_html);
						}
						else
						{
							jsonEscapeCopy(path_esc, sizeof(path_esc), path);
							if (strchr(path, '*') != NULL)
							{
								snprintf(temp_html, sizeof(temp_html), "\"path\":\"DIGI: %s\",", path_esc);
								strcat(html, temp_html);
							}
							else
							{
								snprintf(temp_html, sizeof(temp_html), "\"path\":\"RF: %s\",", path_esc);
								strcat(html, temp_html);
							}
						}
					}
					// html += "<td>" + path + "</td>";
					if (aprs.flags & F_HASPOS)
					{
						double lat, lon;
						if (gps.location.isValid())
						{
							lat = gps.location.lat();
							lon = gps.location.lng();
						}
						else
						{
							lat = config.igate_lat;
							lon = config.igate_lon;
						}
						double dtmp = aprsParse.direction(lon, lat, aprs.lng, aprs.lat);
						double dist = aprsParse.distance(lon, lat, aprs.lng, aprs.lat);
						snprintf(temp_html, sizeof(temp_html), "\"dx\":\"%.1fkm/%.0f°\",", dist, dtmp);
						strcat(html, temp_html);
					}
					else
					{
						strcat(html, "\"dx\":\"-\",");
					}
					snprintf(temp_html, sizeof(temp_html), "\"packet\":\"%d\",", packet);
					strcat(html, temp_html);
					if (pkg.audio_level == 0)
					{
						strcat(html, "\"audio\":\"-\"},");
					}
					else
					{
						double Vrms = (double)pkg.audio_level / 1000;
						double audBV = 20.0F * log10(Vrms);
						// if (audBV < -20.0F)
						// {
						// 	strcat(html, " audio:\"");
						// }
						// else if (audBV > -5.0F)
						// {
						// 	strcat(html, "<td style=\"color: #f00000;\">");
						// }
						// else
						// {
						// 	strcat(html, "<td style=\"color: #008000;\">");
						// }
						snprintf(temp_html, sizeof(temp_html), "\"audio\":\"%.1f\"},", audBV);
						strcat(html, temp_html);
						//strcat(html, "dBV</td></tr>\n");
					}
				}
			}
		}
	}
	html[strlen(html) - 1] = '\0'; // Remove the last comma
	if (html[0] == '[')
	strcat(html, "]");

	size_t len = strlen(html);
	// char *info = (char *)calloc(len + 1, sizeof(char));
	// if (info)
	// {
	// 	strcpy(info, html);
	if (len > 10)
		lastheard_events.send(html, "lastHeard", millis() / 1000, 1000);
	// 	free(info);
	// }

	free(html);
}

String event_chatMessage(bool gethtml)
{
	// log_d("Event count: %d",lastheard_events.count());
	// if (message_events.count() == 0)
	//	return "NO";

	struct tm tmstruct, tmNow;

	// LU6JMF fix (Set/2026): the old fixed 4096-byte char* buffer + strcat()
	// loop had no bounds checking. With PKGLISTSIZE=20 and long messages,
	// the accumulated HTML could exceed 4096 bytes and overflow the heap
	// buffer, corrupting adjacent memory (same class of bug as the earlier
	// IGATE block_trim_free heap-corruption incident). Switched to a String
	// that grows as needed - no fixed cap, no overflow.
	String html;
	html.reserve(2048);

	time_t timen = time(NULL);
	localtime_r(&timen, &tmNow);

	html += "<tr>\n";
	html += "<th style=\"width:60pt\"><span><b>Time (";
	if (config.timeZone >= 0)
		html += "+";

	// Convert timezone to string
	char temp_buffer[64];
	if (config.timeZone == (int)config.timeZone)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%d", (int)config.timeZone);
		html += temp_buffer;
		html += ")</b></span></th>\n";
	}
	else
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.1f", config.timeZone);
		html += temp_buffer;
		html += ")</b></span></th>\n";
	}

	html += "<th style=\"width:70pt\">Callsign</th>\n";
	html += "<th>Message</th>\n";
	html += "<th style=\"width:10pt\">ACK</th>\n";
	html += "<th style=\"width:20pt\">msgID</th>\n";
	html += "</tr>\n";

	pkgMsgSort(msgQueue);
	for (int i = 0; i < PKGLISTSIZE; i++)
	{
		if (i >= PKGLISTSIZE)
			break;
		msgType pkg = getMsgList(i);
		if (pkg.time > 0)
		{
			pkg.callsign[10] = 0;
			localtime_r(&pkg.time, &tmstruct);
			char strTime[10];
			if (tmNow.tm_mday == tmstruct.tm_mday)
				sprintf(strTime, "%02d:%02d:%02d", tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
			else
				sprintf(strTime, "%dd %02d:%02d", tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min);

			if (pkg.ack > 0)
			{
				html += "<tr style=\"background-color: #f1697dff;\">\n";
			}
			else if (pkg.ack == -1)
			{
				html += "<tr style=\"background-color: #7ff1c5ff;\">\n";
			}
			else if (pkg.ack == -2)
			{
				html += "<tr style=\"background-color: #73caf0ff;\">\n";
			}
			else
			{
				html += "<tr style=\"background-color: #f55353ff;\">\n";
			}

			html += "<td>";
			html += strTime;
			html += "</td>";

			html += "<td>";
			html += pkg.callsign;
			html += "</td>";

			html += "<td style=\"text-align: left;\">";
			html += pkg.text;
			html += "</td>";

			if (pkg.ack > 0)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<td>%d/%d</td>", pkg.ack, config.msg_retry);
				html += temp_buffer;
			}
			else if (pkg.ack == -1)
			{
				html += "<td>RX</td>";
			}
			else if (pkg.ack == -2)
			{
				html += "<td>TX</td>";
			}
			else
			{
				html += "<td>TF</td>";
			}

			snprintf(temp_buffer, sizeof(temp_buffer), "<td>%d</td></tr>\n", pkg.msgID);
			html += temp_buffer;
		}
	}

	log_d("HTML Length=%d Byte gethtml:%d event_cnt:%d", html.length(), gethtml, message_events.count());

	if (gethtml)
	{
		return html;
	}

	if (message_events.count() > 0)
	{
		message_events.send(html.c_str(), "chatMsg", time(NULL), 5000);
	}

	return String("");
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
static const char *STORAGE_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"function sub(obj){"
	"var fileName = obj.value.split('\\\\');"
	"document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
	"}\n"
	"$('form').submit(function(e){"
	"e.preventDefault();"
	"if(e.currentTarget.id === 'formDownload'){"
	"var fd = new FormData(e.currentTarget);"
	"window.open('/download?FILE=' + encodeURIComponent(fd.get('FILE')), '_blank');"
	"return;"
	"}"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id === 'upload_form'){ document.getElementById('upload_sumbit').disabled = true;"
	"var formUp = $('#upload_form')[0];"
	"var dataUp = new FormData(formUp);"
	"$.ajax({"
	"url: '/upload',"
	"type: 'POST',"
	"data: dataUp,"
	"contentType: false,"
	"processData:false,"
	"xhr: function() {"
	"var xhr = new window.XMLHttpRequest();"
	"xhr.upload.addEventListener('progress', function(evt) {"
	"if (evt.lengthComputable) {"
	"var per = evt.loaded / evt.total;"
	"$('#prg').html(Math.round(per*100) + '%');"
	"$('#bar').css('width',Math.round(per*100) + '%');"
	"}"
	"}, false);"
	"return xhr;"
	"},"
	"success:function(d, s) {"
	"alert('Upload Success');"
	"$(\"#contentmain\").load(\"/storage\");\n"
	"},"
	"error: function (a, b, c) {"
	"}"
	"});"
	"}else{"
	"$.ajax({"
	"url: '/storage',"
	"type: 'POST',"
	"data: data,"
	"contentType: false,"
	"processData:false,"
	"success:function(d, s) {"
	"if(e.currentTarget.id===\"formDelete\") $(\"#contentmain\").load(\"/storage\");\n"
	"},"
	"error: function (a, b, c) {"
	"}"
	"});"
	"}"
	"});"
	"</script>"
	"<div style=\"font-size: 8pt;text-align:left;\">"
	"<b>Total space: </b>{{totalStr}} ,"
	"<b>Used space: </b>{{usedStr}}"
	"</br>Listing directory: </b>{{dirname}}</div>\n"
	"{{#if openFailed}}Failed to open directory\n{{/if}}"
	"{{#if notDir}}Not a directory{{/if}}"
	"<table border=\"1\"><tr align=\"center\" bgcolor=\"#03DDFC\"><th width=\"100\"><b>DIRECTORY</b></th><th><b>FILE NAME</b></th><th width=\"100\"><b>SIZE(Byte)</b></th><th width=\"170\"><b>DATE TIME</b></th><th width=\"50\"><b>DELETE</b></th><th width=\"100\"><b>DOWNLOAD</b></th></tr>"
	"{{#each entries}}"
	"{{#if isDir}}"
	"<tr><td>{{name}}</td><td></td><td></td><td align=\"right\">{{dateStr}}</td><td></td></tr>\n"
	"{{#each subfiles}}"
	"<tr><td>{{dirName}}</td><td align=\"left\">{{name}}</td><td align=\"right\">{{size}}</td><td align=\"center\">{{dateStr}}</td><td></td><td></td></tr>\n"
	"{{/each}}"
	"{{else}}"
	"<tr><td>/</td><td align=\"left\"><a href=\"/download?FILE={{name}}\" target=\"_blank\">{{name}}</a></td><td align=\"right\">{{size}}</td><td align=\"center\">{{dateStr}}</td>"
	"{{#if deletable}}"
	"<td align=\"center\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formDelete\" method=\"post\"><input name=\"delete\" type=\"hidden\" /><input name=\"FILE\" type=\"hidden\" value=\"{{name}}\" /><button name=\"commit\" id=\"btnDelete\" type=\"submit\" style=\"background-color:red;color:white\">X</button></form></td>\n"
	"{{else}}<td></td>{{/if}}"
	"<td align=\"center\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formDownload\" method=\"post\"><input name=\"download\" type=\"hidden\" /><input name=\"FILE\" type=\"hidden\" value=\"{{name}}\" /><button name=\"commit\" id=\"btnDownload\" type=\"submit\" style=\"background-color:green;color:white\">DOWNLOAD</button></form></td></tr>\n"
	"{{/if}}"
	"{{/each}}"
	"</table>\n"
	"<br><form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"upload_form\" method=\"post\" enctype=\"multipart/form-data\">\n"
	"<table border=\"1\" style=\"margin-top:10px;width:100%;\">\n"
	"<tr align=\"center\" bgcolor=\"#03DDFC\"><th colspan=\"3\"><b>UPLOAD FILE</b></th></tr>\n"
	"<tr align=\"center\"><td width=\"60\" align=\"right\"><b>File:</b></td><td align=\"left\"><input type=\"file\" name=\"data\" required></td><td width=\"120\"><button class=\"button\" type='submit' id='upload_sumbit'>UPLOAD</button></td></tr>\n"
	"</table>\n</form><br/>\n"
	"</body>\n</html>\n";

void handle_storage(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	adcEn = -1;
	dacEn = -1;
	delay(100);	

	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	const char *dirname = "/";
	char strTime[100];

	unsigned long cardTotal = LITTLEFS.totalBytes();
	unsigned long cardUsed = LITTLEFS.usedBytes();

	if (request->hasArg("delete"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "FILE")
			{
				String path = request->arg(i);
#ifdef DEBUG
				Serial.println("Deleting file: " + path);
#endif
				if (LITTLEFS.remove("/" + path))
				{
					//html = "File deleted";
#ifdef DEBUG
					Serial.println("File deleted");
#endif
				}
				else
				{
					//html = "Delete failed";
#ifdef DEBUG
					Serial.println("Delete failed");
#endif
				}
				break;
			}
		}
	}else if (request->hasArg("download"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "FILE")
			{
				String path = request->arg(i);
				String dataType = "";
				if (path.endsWith(".src"))
					path = path.substring(0, path.lastIndexOf("."));
				else if (path.endsWith(".htm"))
					dataType = "text/html";
				else if (path.endsWith(".csv"))
					dataType = "text/csv";
				else if (path.endsWith(".css"))
					dataType = "text/css";
				else if (path.endsWith(".xml"))
					dataType = "text/xml";
				else if (path.endsWith(".png"))
					dataType = "image/png";
				else if (path.endsWith(".gif"))
					dataType = "image/gif";
				else if (path.endsWith(".jpg"))
					dataType = "image/jpeg";
				else if (path.endsWith(".ico"))
					dataType = "image/x-icon";
				else if (path.endsWith(".svg"))
					dataType = "image/svg+xml";
				else if (path.endsWith(".ico"))
					dataType = "image/x-icon";
				else if (path.endsWith(".js"))
					dataType = "application/javascript";
				else if (path.endsWith(".pdf"))
					dataType = "application/pdf";
				else if (path.endsWith(".zip"))
					dataType = "application/zip";
				else if (path.endsWith(".cfg"))
					dataType = "plain/text";
				else if (path.endsWith(".json"))
					dataType = "application/json";
				else if (path.endsWith(".gz"))
				{
					if (path.startsWith("/gz/htm"))
						dataType = "text/html";
					else if (path.startsWith("/gz/css"))
						dataType = "text/css";
					else if (path.startsWith("/gz/csv"))
						dataType = "text/csv";
					else if (path.startsWith("/gz/xml"))
						dataType = "text/xml";
					else if (path.startsWith("/gz/js"))
						dataType = "application/javascript";
					else if (path.startsWith("/gz/svg"))
						dataType = "image/svg+xml";
					else
						dataType = "application/x-gzip";
				}else{
					dataType = "application/octet-stream";
					path = path.substring(0, path.lastIndexOf("."));
					//html = "File type not support";
					//request->send_P(404, PSTR("text/plain"), PSTR("File type not support"));
					//break;
				}

				if (path != "" && dataType != "")
				{
					String file = "/" + path;
					//request->send(LITTLEFS, file, dataType, true);
					AsyncWebServerResponse *response = request->beginResponse(LITTLEFS, file, dataType, true);
					//response->addHeader("Content-Disposition","attachment");
					request->send(response);
				}
				else
				{
					if (dataType != "")
						request->send_P(404, PSTR("text/plain"), PSTR("ContentType Not Support"));
					else
						request->send_P(404, PSTR("text/plain"), PSTR("File Not found"));
				}
				return;
			}
		}
	}

	JsonDocument data;

	if (cardTotal > 1000000)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f MByte", (double)cardTotal / 1048576);
		data["totalStr"] = buf;
	}
	else
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f KByte", (double)cardTotal / 1024);
		data["totalStr"] = buf;
	}
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f KByte", (double)cardUsed / 1024);
		data["usedStr"] = buf;
	}
	data["dirname"] = dirname;

	JsonArray entries = data["entries"].to<JsonArray>();

	File root = LITTLEFS.open(dirname);
	bool openFailed = !root;
	bool notDir = root && !root.isDirectory();

	if (root && root.isDirectory())
	{
		File file = root.openNextFile();
		while (file)
		{
			JsonObject entry = entries.add<JsonObject>();
			char buf[64];
			if (file.isDirectory())
			{
				entry["isDir"] = true;
				entry["name"] = file.name();
				time_t t = file.getLastWrite();
				struct tm *tmstruct = localtime(&t);
				snprintf(buf, sizeof(buf), "%d-%02d-%02d %02d:%02d:%02d", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
				entry["dateStr"] = buf;

				// LU6JMF - Marcelo Ferreira:
				// This listing only ever showed root-level files, so a file saved
				// under a subfolder (like /templates/main_shell.html) was invisible
				// here even though it was really on the filesystem - made it look
				// like uploads into subfolders silently failed when they hadn't.
				// List one level into the subfolder too (read-only, no
				// delete/download - just enough to see what's really there).
				JsonArray subfiles = entry["subfiles"].to<JsonArray>();
				String subDirName = String(file.name());
				File sub = LITTLEFS.open(subDirName);
				if (sub && sub.isDirectory())
				{
					File subFile = sub.openNextFile();
					while (subFile)
					{
						if (!subFile.isDirectory())
						{
							JsonObject se = subfiles.add<JsonObject>();
							se["dirName"] = subDirName;
							se["name"] = subFile.name();
							se["size"] = subFile.size();
							time_t st = subFile.getLastWrite();
							struct tm *stmstruct = localtime(&st);
							snprintf(buf, sizeof(buf), "%d-%02d-%02d %02d:%02d:%02d", (stmstruct->tm_year) + 1900, (stmstruct->tm_mon) + 1, stmstruct->tm_mday, stmstruct->tm_hour, stmstruct->tm_min, stmstruct->tm_sec);
							se["dateStr"] = buf;
						}
						subFile = sub.openNextFile();
					}
				}
			}
			else
			{
				entry["isDir"] = false;
				const char *fName = file.name();
				entry["name"] = fName;
				entry["size"] = file.size();
				time_t t = file.getLastWrite();
				struct tm *tmstruct = localtime(&t);
				snprintf(buf, sizeof(buf), "%d-%02d-%02d %02d:%02d:%02d", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
				entry["dateStr"] = buf;
				entry["deletable"] = (strcmp(fName, "fsversion.txt") != 0);
			}
			file = root.openNextFile();
		}
	}
	data["openFailed"] = openFailed;
	data["notDir"] = notDir;

	String html_str = renderTemplate(STORAGE_TEMPLATE, data.as<JsonVariant>());
	sendHtmlChunked(request, html_str, "Sensor");
	adcEn = 1;
	dacEn = 0;
}

void handle_download(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	String dataType = "";
	String path = "";

	if (request->args() > 0)
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "FILE")
			{
				path = request->arg(i);
				break;
			}
		}
	}

	if (path.endsWith(".src"))
		path = path.substring(0, path.lastIndexOf("."));
	else if (path.endsWith(".htm"))
		dataType = "text/html";
	else if (path.endsWith(".csv"))
		dataType = "text/csv";
	else if (path.endsWith(".css"))
		dataType = "text/css";
	else if (path.endsWith(".xml"))
		dataType = "text/xml";
	else if (path.endsWith(".png"))
		dataType = "image/png";
	else if (path.endsWith(".gif"))
		dataType = "image/gif";
	else if (path.endsWith(".jpg"))
		dataType = "image/jpeg";
	else if (path.endsWith(".ico"))
		dataType = "image/x-icon";
	else if (path.endsWith(".svg"))
		dataType = "image/svg+xml";
	else if (path.endsWith(".ico"))
		dataType = "image/x-icon";
	else if (path.endsWith(".js"))
		dataType = "application/javascript";
	else if (path.endsWith(".pdf"))
		dataType = "application/pdf";
	else if (path.endsWith(".zip"))
		dataType = "application/zip";
	else if (path.endsWith(".txt"))
		dataType = "text/plain";
	else if (path.endsWith(".cfg"))
		dataType = "text/html";
	else if (path.endsWith(".json"))
		dataType = "application/json";
	else if (path.endsWith(".gz"))
	{
		if (path.startsWith("/gz/htm"))
			dataType = "text/html";
		else if (path.startsWith("/gz/css"))
			dataType = "text/css";
		else if (path.startsWith("/gz/csv"))
			dataType = "text/csv";
		else if (path.startsWith("/gz/xml"))
			dataType = "text/xml";
		else if (path.startsWith("/gz/js"))
			dataType = "application/javascript";
		else if (path.startsWith("/gz/svg"))
			dataType = "image/svg+xml";
		else
			dataType = "application/x-gzip";
	}

	if (path != "" && dataType != "")
	{
		String file = "/" + path;
		request->send(LITTLEFS, file, dataType, true);
		// AsyncWebServerResponse *response = request->beginResponse(LITTLEFS, file, dataType, true);
		// response->addHeader("Content-Disposition","attachment");
		// request->send(response);
	}
	else
	{
		if (dataType != "")
			request->send_P(404, PSTR("text/plain"), PSTR("ContentType Not Support"));
		else
			request->send_P(404, PSTR("text/plain"), PSTR("File Not found"));
	}
}

void handle_delete(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	String html = "FAIL";
	String dataType = "text/plain";
	String path;
	if (request->args() > 0)
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "FILE")
			{
				path = request->arg(i);
#ifdef DEBUG
				Serial.println("Deleting file: " + path);
#endif
				if (LITTLEFS.remove("/" + path))
				{
					html = "File deleted";
#ifdef DEBUG
					Serial.println("File deleted");
#endif
				}
				else
				{
					html = "Delete failed";
#ifdef DEBUG
					Serial.println("Delete failed");
#endif
				}
				break;
			}
		}
	}
	request->send(200, "text/html", html); // send to someones browser when asked
}

void handle_format(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	String html = "FAIL";
	if (request->args() > 0)
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "commit")
			{
				if (request->arg(i) == "FORMAT")
				{
					LITTLEFS.format();
					html = "OK";
					break;
				}
			}
		}
	}

	request->send(200, "text/html", html); // send to someones browser when asked
}

static const char *RADIO_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"var sliderVol = document.getElementById(\"sliderVolume\");\n"
	"var outputVol = document.getElementById(\"volShow\");\n"
	"var sliderSql = document.getElementById(\"sliderSql\");\n"
	"var outputSql = document.getElementById(\"sqlShow\");\n"
	"var sliderTxGain = document.getElementById(\"sliderTxGain\");\n"
	"var outputTxGain = document.getElementById(\"txGainShow\");\n"
	"var sliderTxTilt = document.getElementById(\"sliderTxTilt\");\n"
	"var outputTxTilt = document.getElementById(\"txTiltShow\");\n"
	"outputVol.innerHTML = sliderVol.value;\n"
	"outputSql.innerHTML = sliderSql.value;\n"
	"outputTxGain.innerHTML = sliderTxGain.value;\n"
	"outputTxTilt.innerHTML = sliderTxTilt.value;\n"
	"\n"
	"sliderVol.oninput = function () {\n"
	"outputVol.innerHTML = this.value;\n"
	"}\n"
	"sliderSql.oninput = function () {\n"
	"outputSql.innerHTML = this.value;\n"
	"}\n"
	"sliderTxGain.oninput = function () {\n"
	"outputTxGain.innerHTML = this.value;\n"
	"}\n"
	"sliderTxTilt.oninput = function () {\n"
	"outputTxTilt.innerHTML = this.value;\n"
	"}\n"
	"\n"
	"function calibrateTone(type){\n"
	"document.getElementById('calBtnLow').disabled=true;\n"
	"document.getElementById('calBtnHigh').disabled=true;\n"
	"document.getElementById('calBtnAlt').disabled=true;\n"
	"document.getElementById('calStatus').innerHTML='Sending...';\n"
	"fetch('/calibrate_tone?type='+type).then(function(r){return r.text();}).then(function(t){\n"
	"document.getElementById('calStatus').innerHTML=t;\n"
	"}).catch(function(e){\n"
	"document.getElementById('calStatus').innerHTML='Error calling /calibrate_tone';\n"
	"}).finally(function(){\n"
	"setTimeout(function(){\n"
	"document.getElementById('calBtnLow').disabled=false;\n"
	"document.getElementById('calBtnHigh').disabled=false;\n"
	"document.getElementById('calBtnAlt').disabled=false;\n"
	"},11000);\n"
	"});\n"
	"}\n"
	"\n"
	"function calibrateRestart(){\n"
	"if(!confirm('Restart the device now? Any in-progress tone test will stop and the web panel will briefly disconnect.')) return;\n"
	"document.getElementById('calBtnRestart').disabled=true;\n"
	"document.getElementById('calStatus').innerHTML='Restarting - the device will be back online in a few seconds...';\n"
	"var data = new FormData();\n"
	"data.append('REBOOT','1');\n"
	"fetch('/system',{method:'POST',body:data});\n"
	"}\n"
	"\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formRadio\") document.getElementById(\"submitRadio\").disabled=true;\n"
	"if(e.currentTarget.id===\"formTNC\") document.getElementById(\"submitTNC\").disabled=true;\n"
	"var formIdForReenable = e.currentTarget.id;\n"
	"$.ajax({\n"
	"url: '/radio',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"},\n"
	// v2.1.3.1-audiotest: the submit button was disabled above to prevent a double-click, but was
	// never re-enabled after the request finished (success or error) - so a second Apply Change on
	// the same page load (e.g. while iterating on TX Audio Calibration) silently did nothing. Always
	// re-enable it once the request settles.
	"complete: function (data) {\n"
	"if(formIdForReenable===\"formRadio\") document.getElementById(\"submitRadio\").disabled=false;\n"
	"if(formIdForReenable===\"formTNC\") document.getElementById(\"submitTNC\").disabled=false;\n"
	"}\n"
	"});\n"
	"});\n"
	"function rfType(){\n"
	"var type = document.getElementById(\"rf_type\").value;\n"
	"if(type==1||type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"max\",174);document.getElementById(\"rx_freq\").setAttribute(\"max\",174);};\n"
	"if(type==1){document.getElementById(\"tx_freq\").setAttribute(\"min\",134);document.getElementById(\"rx_freq\").setAttribute(\"min\",134);};\n"
	"if(type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"min\",136);document.getElementById(\"rx_freq\").setAttribute(\"min\",136);};\n"
	"if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"max\",470);document.getElementById(\"rx_freq\").setAttribute(\"max\",470);};\n"
	"if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"min\",400);document.getElementById(\"rx_freq\").setAttribute(\"min\",400);};\n"
	"if(type==3){document.getElementById(\"tx_freq\").setAttribute(\"min\",320);document.getElementById(\"rx_freq\").setAttribute(\"min\",320);};\n"
	"if(type==3){document.getElementById(\"tx_freq\").setAttribute(\"max\",400);document.getElementById(\"rx_freq\").setAttribute(\"max\",400);};\n"
	"if(type==6){document.getElementById(\"tx_freq\").setAttribute(\"min\",350);document.getElementById(\"rx_freq\").setAttribute(\"min\",350);};\n"
	"if(type==6){document.getElementById(\"tx_freq\").setAttribute(\"max\",390);document.getElementById(\"rx_freq\").setAttribute(\"max\",390);};\n"
	"if(type==1||type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"value\",144.390);document.getElementById(\"rx_freq\").setAttribute(\"value\",144.390);};\n"
	"if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"value\",432.5);document.getElementById(\"rx_freq\").setAttribute(\"value\",432.5);};\n"
	"\n"
	"}\n"
	"</script>\n"
	"<form id='formRadio' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>RF Analog Module</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"radioEnable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Module Type:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"rf_type\" id=\"rf_type\" onchange=\"rfType()\">\n"
	"{{#each rfTypeOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select>\n"
	"</td>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX Frequency:</b></td>\n"
	"<td style=\"text-align: left;\"><input type=\"number\" id=\"tx_freq\" name=\"tx_freq\" min=\"{{freqMin}}\" max=\"{{freqMax}}\"\n"
	"step=\"0.0001\" value=\"{{txFreq}}\" /> MHz</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>RX Frequency:</b></td>\n"
	"<td style=\"text-align: left;\"><input type=\"number\" id=\"rx_freq\" name=\"rx_freq\" min=\"{{freqMin}}\" max=\"{{freqMax}}\"\n"
	"step=\"0.0001\" value=\"{{rxFreq}}\" /> Mhz</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX CTCSS:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"tx_ctcss\" id=\"tx_ctcss\">\n"
	"{{#each txCtcssOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select> Hz\n"
	"</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>RX CTCSS:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"rx_ctcss\" id=\"rx_ctcss\">\n"
	"<option value=\"0\" selected>0.0</option>\n"
	"{{#each rxCtcssOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select> Hz\n"
	"</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Narrow/Wide:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"nw_band\" id=\"nw_band\">\n"
	"<option value=\"0\" {{#if band}}{{else}}selected{{/if}}>12.5KHz</option>\n"
	"<option value=\"1\" {{#if band}}selected{{/if}}>25.0KHz</option>\n"
	"</select>\n"
	"</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX Power:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"rf_power\" id=\"rf_power\">\n"
	"<option value=\"1\" {{#if rfPower}}selected{{/if}}>HIGH</option>\n"
	"<option value=\"0\" {{#if rfPower}}{{else}}selected{{/if}}>LOW</option>\n"
	"</select>\n"
	"</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>VOLUME:</b></td>\n"
	"<td style=\"text-align: left;\"><input id=\"sliderVolume\" name=\"volume\" type=\"range\"\nmin=\"1\" max=\"8\" value=\"{{volume}}\" /><b><span style=\"font-size: 14pt;\" id=\"volShow\">{{volume}}</span></b></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>SQL Level:</b></td>\n"
	"<td style=\"text-align: left;\"><input id=\"sliderSql\" name=\"sql_level\" type=\"range\"\nmin=\"0\" max=\"8\" value=\"{{sqlLevel}}\" /><b><span style=\"font-size: 14pt;\" id=\"sqlShow\">{{sqlLevel}}</span></b></td>\n"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitRadio'  name=\"commitRadio\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitRadio\"/>\n"
	"</td></tr></table><br />\n"
	"</form>"
	"<form id='formTNC' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table style=\"table-layout:fixed;width:100%;\">\n"
	"<th colspan=\"2\"><span><b>AFSK/TNC Configuration</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Modem Type:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"modem_type\" id=\"modem_type\" \">\n"
	"{{#each modemOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select>\n"
	"</td>\n"
	"<tr>\n"
	"<td align=\"right\"><b>FX.25 Mode:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"fx25_mode\" id=\"fx25_mode\" \">\n"
	"{{#each fx25Options}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select>  (FX.25 = AX.25 + FEC)\n"
	"</td>\n"
	"<tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Deemphasis Audio:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"LPF\" value=\"OK\" {{#if lpfEnabled}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Audio low pass filter 1hz-2.5KHz</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX Audio Level:</b></td>\n"
	"<td style=\"text-align: left;\"><input id=\"sliderTxGain\" name=\"txAudioGain\" type=\"range\"\nmin=\"0\" max=\"100\" value=\"{{txAudioGain}}\" /><b><span style=\"font-size: 14pt;\" id=\"txGainShow\">{{txAudioGain}}</span></b> %</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td></td>\n"
	"<td style=\"text-align: left;word-wrap:break-word;white-space:normal;\"><label style=\"font-size: 8pt;\"><i>TX audio level (0-100%). Adjust while listening on another radio - 100% is factory default (no change if never touched).</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX Pre-emphasis:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"txPreemphasis\" value=\"OK\" {{#if txPreemphasisEnabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td></td>\n"
	"<td style=\"text-align: left;word-wrap:break-word;white-space:normal;\"><label style=\"font-size: 8pt;\"><i>Enable only if the radio input is microphone (not 'flat'/discriminator). Attenuates the 1200Hz tone to compensate for the radio's own pre-emphasis.</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Pre-emphasis Tilt:</b></td>\n"
	"<td style=\"text-align: left;\"><input id=\"sliderTxTilt\" name=\"txPreemphasisTilt\" type=\"range\"\nmin=\"0\" max=\"100\" value=\"{{txPreemphasisTilt}}\" /><b><span style=\"font-size: 14pt;\" id=\"txTiltShow\">{{txPreemphasisTilt}}</span></b> %</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>TX Time Slot:</b></td>\n"
	"<td style=\"text-align: left;\"><input type=\"number\" name=\"timeSlot\" min=\"0\" max=\"99999\"\nstep=\"100\" value=\"{{timeSlot}}\" /> mSec.</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Preamble:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"preamble\">\n"
	"{{#each preambleOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select> mSec.\n"
	"</td>\n"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitTNC'  name=\"commitTNC\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitTNC\"/>\n"
	"</td></tr></table><br />\n"
	"</form>"
	"<table style=\"table-layout:fixed;width:100%;\">\n"
	"<th colspan=\"2\"><span><b>TX Audio Calibration</b></span></th>\n"
	"<tr>\n"
	"<td colspan=\"2\" style=\"text-align:left;font-size:8pt;word-wrap:break-word;white-space:normal;\"><i>Transmits a continuous tone for 10 seconds (turns off by itself) to calibrate TX audio while listening on another radio. This keys the PTT and transmits real RF.</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td colspan=\"2\" style=\"text-align:left;\">\n"
	"<button class=\"button\" type='button' id='calBtnLow' onclick=\"calibrateTone('low')\"> Low Tone (1200Hz) </button>\n"
	"<button class=\"button\" type='button' id='calBtnHigh' onclick=\"calibrateTone('high')\"> High Tone (2200Hz) </button>\n"
	"<button class=\"button\" type='button' id='calBtnAlt' onclick=\"calibrateTone('alt')\"> Alternating </button>\n"
	"</td>\n"
	"</tr>\n"
	"<tr><td colspan=\"2\" id=\"calStatus\" style=\"text-align:left;word-wrap:break-word;white-space:normal;\"></td></tr>\n"
	// v2.1.3.1-audiotest: the calibration tone bypasses the normal AX.25 TX scheduler (it drives
	// hw_afsk_dac_isr/the DAC timer directly instead of going through a scheduled TX job), so the
	// scheduler can be left thinking a slot is busy after a calibration run. Warn and offer a direct
	// restart so real beacons/objects resume reliably without the operator having to go find the
	// System tab.
	"<tr><td colspan=\"2\" style=\"text-align:left;font-size:8pt;color:#b35900;word-wrap:break-word;white-space:normal;\"><i>While a tone plays (10 s) the TX scheduler waits; queued packets go out right after it ends. No restart needed. If the digipeater is transmitting when you press a tone button, wait a few seconds and press it again.</i></td></tr>\n"
	"<tr><td colspan=\"2\" style=\"text-align:left;\">\n"
	"<button class=\"button\" type='button' id='calBtnRestart' style=\"background-color:#b35900;\" onclick=\"calibrateRestart()\"> Restart Device Now </button>\n"
	"</td></tr>\n"
	"</table><br />";

// 2.1.5: send an already-rendered HTML page as a CHUNKED response.
// The old pattern (AsyncResponseStream + print(html_str)) kept TWO full copies of the page in
// heap at once (the String plus the stream buffer, a big contiguous block): on the largest
// pages (MOD, SYSTEM, IGATE...) that is what failed under heap fragmentation ("IGATE page
// stopped opening"). Here the String is MOVED (no copy) into a shared_ptr and handed to the
// client in small slices as the TCP window allows; the memory is released automatically when
// the response ends or the client disconnects.
static void sendHtmlChunked(AsyncWebServerRequest *request, String &page, const char *hdrName)
{
	std::shared_ptr<String> body(new (std::nothrow) String());
	if (!body)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	*body = std::move(page);
	AsyncWebServerResponse *resp = request->beginChunkedResponse("text/html",
		[body](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
		{
			size_t total = body->length();
			if (index >= total)
				return 0;
			size_t n = total - index;
			if (n > maxLen)
				n = maxLen;
			memcpy(buffer, body->c_str() + index, n);
			return n;
		});
	if (!resp)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	if (hdrName)
		resp->addHeader(hdrName, "content");
	resp->addHeader("Cache-Control", "no-cache");
	request->send(resp);
}

void handle_radio(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	// bool noiseEn=false;
	bool radioEnable = false;
	if (request->hasArg("commitRadio"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));
			if (request->argName(i) == "radioEnable")
			{
				if (request->arg(i) != "")
				{
					// Compare the argument directly without converting to String
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						radioEnable = true;
					}
				}
			}

			if (request->argName(i) == "nw_band")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.band = request->arg(i).toInt();
						// if (request->arg(i).toInt())
						// 	config.band = 1;
						// else
						// 	config.band = 0;
					}
				}
			}

			if (request->argName(i) == "volume")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.volume = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rf_power")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						if (request->arg(i).toInt())
							config.rf_power = true;
						else
							config.rf_power = false;
					}
				}
			}

			if (request->argName(i) == "sql_level")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.sql_level = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "tx_freq")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.freq_tx = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "rx_freq")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.freq_rx = request->arg(i).toFloat();
				}
			}

			if (request->argName(i) == "tx_offset")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.offset_tx = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "rx_offset")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.offset_rx = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "tx_ctcss")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tone_tx = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "rx_ctcss")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tone_rx = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "rf_type")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.rf_type = request->arg(i).toInt();
				}
			}
		}
		// config.noise=noiseEn;
		// config.agc=agcEn;
		config.rf_en = radioEnable;
		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(64); // Small buffer for "OK"
		if (html)
		{
			strcpy(html, "OK");
			request->send(200, "text/html", html); // send to someones browser when asked
			free(html);							   // Free the allocated memory
		}
		saveConfiguration("/default.cfg", config);
		delay(500);
		RF_MODULE(false);
	}
	else if (request->hasArg("commitTNC"))
	{
		bool hpf = 0;
		bool lpf = 0;
		bool txPreemph = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "HPF")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						hpf = true;
					}
				}
			}
			if (request->argName(i) == "LPF")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						lpf = true;
					}
				}
			}
			if (request->argName(i) == "txPreemphasis")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						txPreemph = true;
					}
				}
			}
			if (request->argName(i) == "txAudioGain")
			{
				if (request->arg(i) != "" && isValidNumber(request->arg(i)))
				{
					long v = request->arg(i).toInt();
					if (v < 0)
						v = 0;
					if (v > 100)
						v = 100;
					config.tx_audio_gain = (uint8_t)v;
				}
			}
			if (request->argName(i) == "txPreemphasisTilt")
			{
				if (request->arg(i) != "" && isValidNumber(request->arg(i)))
				{
					long v = request->arg(i).toInt();
					if (v < 0)
						v = 0;
					if (v > 100)
						v = 100;
					config.tx_preemphasis_tilt = (uint8_t)v;
				}
			}
			if (request->argName(i) == "timeSlot")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.tx_timeslot = request->arg(i).toInt();
					}
				}
			}
			if (request->argName(i) == "preamble")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.preamble = request->arg(i).toInt();
					}
				}
			}
			if (request->argName(i) == "modem_type")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.modem_type = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "fx25_mode")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.fx25_mode = request->arg(i).toInt();
				}
			}
		}
		config.audio_hpf = hpf;
		config.audio_lpf = lpf;
		config.tx_preemphasis = txPreemph;
		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(64); // Small buffer for "OK"
		if (html)
		{
			strcpy(html, "OK");
			request->send(200, "text/html", html); // send to someones browser when asked
			free(html);							   // Free the allocated memory
		}
		saveConfiguration("/default.cfg", config);
		afskSetModem(config.modem_type, config.audio_lpf, config.tx_timeslot, config.preamble * 100, config.fx25_mode);
		afskSetTxGain(config.tx_audio_gain);
		afskSetTxPreemphasis(config.tx_preemphasis, config.tx_preemphasis_tilt);
	}
	else
	{
		JsonDocument data;
		data["enabled"] = config.rf_en;

		JsonArray rfTypeOptions = data["rfTypeOptions"].to<JsonArray>();
		for (int i = 0; i < 9; i++)
		{
			JsonObject o = rfTypeOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = RF_TYPE[i];
			o["selected"] = (config.rf_type == i);
		}

		float freqMin = 0;
		float freqMax = 0;
		switch (config.rf_type)
		{
		case RF_SA868_VHF:
			freqMin = 134.0F;
			freqMax = 174.0F;
			break;
		case RF_SR_1WV:
		case RF_SR_2WVS:
			freqMin = 136.0F;
			freqMax = 174.0F;
			break;
		case RF_SA868_350:
			freqMin = 320.0F;
			freqMax = 400.0F;
			break;
		case RF_SR_1W350:
			freqMin = 350.0F;
			freqMax = 390.0F;
			break;
		case RF_SA868_UHF:
		case RF_SR_1WU:
		case RF_SR_2WUS:
			freqMin = 400.0F;
			freqMax = 470.0F;
			break;
		default:
			freqMin = 134.0F;
			freqMax = 500.0F;
			break;
		}
		char buf[24];
		snprintf(buf, sizeof(buf), "%.4f", freqMin);
		data["freqMin"] = buf;
		snprintf(buf, sizeof(buf), "%.4f", freqMax);
		data["freqMax"] = buf;
		snprintf(buf, sizeof(buf), "%.4f", config.freq_tx);
		data["txFreq"] = buf;
		snprintf(buf, sizeof(buf), "%.4f", config.freq_rx);
		data["rxFreq"] = buf;

		JsonArray txCtcssOptions = data["txCtcssOptions"].to<JsonArray>();
		JsonArray rxCtcssOptions = data["rxCtcssOptions"].to<JsonArray>();
		for (int i = 0; i < 39; i++)
		{
			char label[8];
			snprintf(label, sizeof(label), "%.1f", ctcss[i]);

			JsonObject ot = txCtcssOptions.add<JsonObject>();
			ot["value"] = i;
			ot["label"] = label;
			ot["selected"] = (config.tone_tx == i);

			JsonObject orr = rxCtcssOptions.add<JsonObject>();
			orr["value"] = i;
			orr["label"] = label;
			orr["selected"] = (config.tone_rx == i);
		}

		data["band"] = (bool)config.band;
		data["rfPower"] = (bool)config.rf_power;
		data["volume"] = config.volume;
		data["sqlLevel"] = config.sql_level;

		JsonArray modemOptions = data["modemOptions"].to<JsonArray>();
#ifdef CONFIG_IDF_TARGET_ESP32S3
		int modemCount = 4;
#else
		int modemCount = 3;
#endif
		for (int i = 0; i < modemCount; i++)
		{
			JsonObject o = modemOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = MODEM_TYPE[i];
			o["selected"] = (config.modem_type == i);
		}

		JsonArray fx25Options = data["fx25Options"].to<JsonArray>();
		for (int i = 0; i < 3; i++)
		{
			JsonObject o = fx25Options.add<JsonObject>();
			o["value"] = i;
			o["label"] = FX25_MODE[i];
			o["selected"] = (config.fx25_mode == i);
		}

		data["lpfEnabled"] = config.audio_lpf;
		data["txAudioGain"] = config.tx_audio_gain;
		data["txPreemphasisEnabled"] = config.tx_preemphasis;
		data["txPreemphasisTilt"] = config.tx_preemphasis_tilt;
		data["timeSlot"] = config.tx_timeslot;

		JsonArray preambleOptions = data["preambleOptions"].to<JsonArray>();
		for (int i = 1; i < 11; i++)
		{
			JsonObject o = preambleOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = i * 100;
			o["selected"] = (config.preamble == i);
		}

		esp_task_wdt_reset();
		String html_str = renderTemplate(RADIO_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "Sysinfo");
	}
}

// v2.1.3.1-audiotest: TX audio calibration tone (see "TX Audio Calibration" section of the
// RADIO page). Real RF/PTT - AFSK_StartTxCalibration() enforces its own hard 10s safety timeout
// independently of this HTTP request, so this handler just needs to kick it off and reply; it
// does not need to (and must not) block for the duration of the tone.
void handle_calibrate_tone(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	String type = "low";
	if (request->hasArg("type"))
		type = request->arg("type");

	uint8_t toneType = 0; // default: low/mark tone
	const char *toneName = "1200Hz (low tone)";
	if (type == "high")
	{
		toneType = 1;
		toneName = "2200Hz (high tone)";
	}
	else if (type == "alt")
	{
		toneType = 2;
		toneName = "alternating 1200/2200Hz";
	}

	if (!config.rf_en)
	{
		request->send(409, "text/plain", "RF disabled (RADIO tab > Enable) - enable the RF module before calibrating.");
		return;
	}

	// 2.1.5: never start the tone while the RF scheduler is transmitting (or a PTT OFF
	// is pending): it would collide with the packet. The user just retries in a moment.
	{
		bool schedBusy = false, txNow = false, pttOffPend = false;
		lu6jmfTxDebugSnapshot(&schedBusy, NULL, &txNow, &pttOffPend,
							  NULL, NULL, NULL, NULL, NULL, NULL, NULL);
		if (schedBusy || txNow || pttOffPend)
		{
			request->send(409, "text/plain", "The digipeater is transmitting right now - wait a few seconds and press the tone button again.");
			return;
		}
	}

	char msg[220];
	if (AFSK_StartTxCalibration(toneType))
	{
		// v2.1.3.1-audiotest (debug): echo back the gain/preemphasis values actually in effect for
		// this tone, so we can confirm from the browser whether Apply Change really propagated them,
		// without needing a serial console.
		snprintf(msg, sizeof(msg),
				 "Tone %s started - transmitting for 10s, PTT turns off automatically. "
				 "[debug: gain=%u%% preemph=%s tilt=%u%%]",
				 toneName, config.tx_audio_gain, config.tx_preemphasis ? "ON" : "OFF", config.tx_preemphasis_tilt);
		request->send(200, "text/plain", msg);
	}
	else
	{
		request->send(409, "text/plain", "A tone calibration is already in progress - wait for it to finish.");
	}
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
static const char *VPN_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formVPN\") document.getElementById(\"submitVPN\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/vpn',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"function vpnMsg(t) { var e = document.getElementById('vpnreg'); if (e) e.textContent = t; }\n"
	"function loadVPNConfig() {\n"
	"const url = \"http://hs1.hs5tqa.ampr.org:81/wg/create\";\n"
	"const espID = {'name': '{{espId}}'};\n"
	"vpnMsg('Registering... the server can take up to a minute, please wait.');\n"
	"const ctl = new AbortController();\n"
	"const tm = setTimeout(() => ctl.abort(), 90000);\n"
	"fetch(url,{\n"
	"method: 'POST',\n"
	"body: JSON.stringify(espID),\n"
	"signal: ctl.signal,\n"
	"headers: { 'Content-Type': 'application/json' }\n"
	"})\n"
	".then(response => { if (!response.ok) throw new Error('server answered ' + response.status); return response.json(); })\n"
	".then(data => {\n"
	"clearTimeout(tm);\n"
	"vpnMsg('Done. Check the fields below, then click Apply Change and reboot the device.');\n"
	"console.log(\"VPN Data:\", data);\n"
	"document.getElementById(\"wg_enable\").checked = true;\n"
	"document.getElementById(\"wg_peer_address\").value = data.Enpoint.split(\":\")[0];\n"
	"document.getElementById(\"wg_port\").value = data.Enpoint.split(\":\")[1];\n"
	"document.getElementById(\"wg_local_address\").value = data.Address;\n"
	"document.getElementById(\"wg_netmask_address\").value = \"255.255.255.0\";\n"
	"document.getElementById(\"wg_gw_address\").value = data.Gateway;\n"
	"document.getElementById(\"wg_public_key\").value = data.PublicKey;\n"
	"document.getElementById(\"wg_private_key\").value = data.PrivateKey;\n"
	"})\n"
	".catch(err => { clearTimeout(tm); vpnMsg('Could not register: ' + err + '. Wait a minute and try again.'); console.error(\"VPN API Error:\", err); });\n}\n"
	"</script>\n"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromVPN\" method=\"post\">\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>Wireguard Configuration</b></span></th>\n"
	"<tr><td align=\"right\"><b>Status</b></td><td style=\"text-align: left;\">{{vpnStatus}}</td></tr>\n"
	"<tr>"
	"<td align=\"right\"><b>Enable</b></td>\n"
	"{{#if vpnEnabled}}"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"wg_enable\" name=\"vpnEnable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n"
	"{{else}}"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"wg_enable\" name=\"vpnEnable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n"
	"{{/if}}"
	"</tr>\n"
	"<tr>\n<td align=\"right\"><b>Server Address</b></td>\n<td style=\"text-align: left;\"><input  size=\"20\" maxlength=\"32\" id=\"wg_peer_address\" name=\"wg_peer_address\" type=\"text\" value=\"{{peerAddress}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Server Port</b></td>\n<td style=\"text-align: left;\"><input id=\"wg_port\" size=\"5\" name=\"wg_port\" type=\"number\" value=\"{{port}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Local Address</b></td>\n<td style=\"text-align: left;\"><input id=\"wg_local_address\" name=\"wg_local_address\" type=\"text\" value=\"{{localAddress}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Netmask</b></td>\n<td style=\"text-align: left;\"><input id=\"wg_netmask_address\" name=\"wg_netmask_address\" type=\"text\" value=\"{{netmask}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Gateway</b></td>\n<td style=\"text-align: left;\"><input id=\"wg_gw_address\" name=\"wg_gw_address\" type=\"text\" value=\"{{gateway}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Public Server Key</b></td>\n<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"44\" id=\"wg_public_key\" name=\"wg_public_key\" type=\"text\" value=\"{{publicKey}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Private Client Key</b></td>\n<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"44\" id=\"wg_private_key\" name=\"wg_private_key\" type=\"text\" value=\"{{privateKey}}\" /></td>\n</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitVPN'  name=\"commitVPN\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitVPN\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br /><br />"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromGetVPN\" method=\"post\">\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>Helper: Free VPN Wireguard for Web Service</b></span></th>\n"
	"<tr><td align=\"left\">1. Click New Register button to get VPN config from web service.</td></tr>\n"
	"<tr><td align=\"left\">2. The VPN config will fill in the form automatically.</td></tr>\n"
	"<tr><td align=\"left\">3. Click Apply Change and reboot again.</td></tr>\n"
	"<tr><td align=\"left\">4. Enjoy your free VPN service!</td></tr>\n"
	"{{#if amprUrl}}"
	"<tr><td>Your External by AMPR URL: <a href=\"{{amprUrl}}\" target=\"_blank\">{{amprUrl}}</a></td></tr>\n"
	"<tr><td>Fast Direct URL: <a href=\"{{directUrl}}\" target=\"_blank\">{{directUrl}}</a></td></tr>\n"
	"{{/if}}"
	"<tr><td><button type=\"button\" onclick=\"loadVPNConfig()\">New Register</button></td></tr>\n"
	"<tr><td id=\"vpnreg\" align=\"left\"></td></tr>\n"
	"</table><br />\n"
	"</form>";

extern "C" const char *wireguard_last_error; // wireguard_vpn.cpp - why the tunnel was not started

void handle_vpn(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitVPN"))
	{
		bool vpnEn = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "vpnEnable")
			{
				if (request->arg(i) != "")
				{
					// if (isValidNumber(request->arg(i)))
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						vpnEn = true;
				}
			}

			// if (request->argName(i) == "taretime") {
			//	if (request->arg(i) != "")
			//	{
			//		//if (isValidNumber(request->arg(i)))
			//		if (strcmp(request->arg(i).c_str(), "OK") == 0)
			//			taretime = true;
			//	}
			// }
			if (request->argName(i) == "wg_port")
			{
				if (request->arg(i) != "")
				{
					config.wg_port = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "wg_public_key")
			{
				if (request->arg(i) != "")
				{
					// LU6JMF: trim pasted whitespace/newline (it shifts the key and breaks it) + bounded copy
					String k = request->arg(i);
					k.trim();
					strlcpy(config.wg_public_key, k.c_str(), sizeof(config.wg_public_key));
				}
			}

			if (request->argName(i) == "wg_private_key")
			{
				if (request->arg(i) != "")
				{
					String k = request->arg(i);
					k.trim();
					strlcpy(config.wg_private_key, k.c_str(), sizeof(config.wg_private_key));
				}
			}

			if (request->argName(i) == "wg_peer_address")
			{
				if (request->arg(i) != "")
				{
					String pa = request->arg(i);
					pa.trim();
					strlcpy(config.wg_peer_address, pa.c_str(), sizeof(config.wg_peer_address));
				}
			}

			if (request->argName(i) == "wg_local_address")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wg_local_address, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "wg_netmask_address")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wg_netmask_address, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "wg_gw_address")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wg_gw_address, request->arg(i).c_str());
				}
			}
		}

		// LU6JMF: never leave VPN enabled with unusable keys (a WireGuard key is exactly
		// 44 base64 chars ending in '='); it used to crash the unit at every boot.
		if (vpnEn && !(strlen(config.wg_private_key) == 44 && config.wg_private_key[43] == '=' &&
					   strlen(config.wg_public_key) == 44 && config.wg_public_key[43] == '='))
		{
			vpnEn = false;
		}
		config.vpn = vpnEn;
		saveConfig(request);
	}
	else
	{
		JsonDocument data;
		String ESP32_ID = WiFi.macAddress();
		ESP32_ID.replace(":", "");
		data["espId"] = ESP32_ID;
		data["vpnEnabled"] = config.vpn;
		data["peerAddress"] = config.wg_peer_address;
		data["port"] = config.wg_port;
		data["localAddress"] = config.wg_local_address;
		data["netmask"] = config.wg_netmask_address;
		data["gateway"] = config.wg_gw_address;
		data["publicKey"] = config.wg_public_key;
		data["privateKey"] = config.wg_private_key;

		// LU6JMF: tell the user WHY the tunnel is not up (it used to fail silently / crash).
		{
			String st;
			if (!config.vpn)
				st = "Disabled";
			else if (wireguard_last_error != NULL && wireguard_last_error[0] != '\0')
				st = String("NOT RUNNING - ") + wireguard_last_error + ". Fix it, Apply Change and reboot.";
			else if (wireguard_active())
				st = "Started (waiting for the server handshake, or connected)";
			else
				st = "Enabled, waiting for WiFi and clock sync";
			data["vpnStatus"] = st;
		}

		// Check if local address starts with "10.44."
		String wg_local_addr = String(config.wg_local_address);
		if (wg_local_addr.startsWith("10.44."))
		{
			int lastoct = wg_local_addr.substring(wg_local_addr.lastIndexOf('.') + 1).toInt();
			int thirdoct = wg_local_addr.substring(wg_local_addr.indexOf('.', wg_local_addr.indexOf('.') + 1) + 1, wg_local_addr.lastIndexOf('.')).toInt();
			data["amprUrl"] = "http://hs" + String(thirdoct) + ".hs5tqa.ampr.org:" + String((thirdoct * 10000) + 8000 + lastoct);
			data["directUrl"] = "http://vpn.nakhonthai.net:" + String((thirdoct * 10000) + 8000 + lastoct);
		}

		String html_str = renderTemplate(VPN_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "VPN");
	}
}

#ifdef MQTT
// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
static const char *MQTT_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formVPN\") document.getElementById(\"submitMQTT\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/mqtt',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromMQTT\" method=\"post\">\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>MQTT Configuration</b></span></th>\n"
	"<tr>"
	"<td align=\"right\"><b>Enable</b></td>\n"
	"{{#if enabled}}"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n"
	"{{else}}"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n"
	"{{/if}}"
	"</tr>\n"
	"<tr>\n<td align=\"right\"><b>Server Address:</b></td>\n<td style=\"text-align: left;\"><input  size=\"30\" maxlength=\"32\" name=\"host\" type=\"text\" value=\"{{host}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Server Port:</b></td>\n<td style=\"text-align: left;\"><input size=\"5\"  maxlength=\"5\"  name=\"port\" type=\"number\" value=\"{{port}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>User:</b></td>\n<td style=\"text-align: left;\"><input maxlength=\"32\" name=\"user\" type=\"text\" value=\"{{user}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Password:</b></td>\n<td style=\"text-align: left;\"><input size=\"40\" maxlength=\"63\" name=\"pass\" type=\"password\" value=\"{{pass}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Topic:</b></td>\n<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"32\" id=\"topic\" name=\"topic\" type=\"text\" value=\"{{topic}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Topic Flag:</b></td>\n"
	"<td align=\"center\">\n<fieldset id=\"TopicGrp\">\n"
	"<legend>Topic Flags Send out MQTT</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTNC\" type=\"checkbox\" value=\"OK\" {{#if topicTNC}}checked{{/if}}/>TNC</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSts\" type=\"checkbox\" value=\"OK\" {{#if topicSts}}checked{{/if}}/>Status</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTlm\" type=\"checkbox\" value=\"OK\" {{#if topicTlm}}checked{{/if}}/>Telemetry</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicWX\" type=\"checkbox\" value=\"OK\" {{#if topicWX}}checked{{/if}}/>Weather</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSensor\" type=\"checkbox\" value=\"OK\" {{#if topicSensor}}checked{{/if}}/>Sensor</td>\n"
	"<td style=\"border:unset;\"></td>"
	"</tr></table></fieldset>\n</td></tr>\n"
	"<tr>\n<td align=\"right\"><b>Subscription:</b></td>\n<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"32\" name=\"subscribe\" type=\"text\" value=\"{{subscribe}}\" /></td>\n</tr>\n"
	"<tr>\n<td align=\"right\"><b>Subscription Flag:</b></td>\n"
	"<td align=\"center\">\n<fieldset id=\"SubGrp\">\n"
	"<legend>Subscription Flags Receive</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subCMD\" type=\"checkbox\" value=\"OK\" {{#if subCMD}}checked{{/if}}/>AT-Command</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subTNC\" type=\"checkbox\" value=\"OK\" {{#if subTNC}}checked{{/if}}/>TNC</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subMsg\" type=\"checkbox\" value=\"OK\" {{#if subMsg}}checked{{/if}}/>Message</td>\n"
	"<td style=\"border:unset;\"></td>"
	"</tr></table></fieldset>\n</td></tr>\n"
	"</table><br />\n"
	"<td><input class=\"button\" id=\"submitMQTT\" name=\"commitMQTT\" type=\"submit\" value=\"Save Config\" maxlength=\"80\"/></td>\n"
	"<input type=\"hidden\" name=\"commitMQTT\"/>\n"
	"</form>\n";

void handle_mqtt(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitMQTT"))
	{
		bool mqttEn = false;
		config.mqtt_topic_flag = 0;
		config.mqtt_subscribe_flag = 0;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "enable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						mqttEn = true;
				}
			}

			if (request->argName(i) == "host")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.mqtt_host, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "port")
			{
				if (request->arg(i) != "")
				{
					config.mqtt_port = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "user")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.mqtt_user, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "pass")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.mqtt_pass, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "topic")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.mqtt_topic, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "subscribe")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.mqtt_subscribe, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "TopicTNC")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_TOPIC_TNC;
				}
			}
			if (request->argName(i) == "TopicSts")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_TOPIC_STATUS;
				}
			}
			if (request->argName(i) == "TopicTlm")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_TOPIC_TELEMETRY;
				}
			}
			if (request->argName(i) == "TopicWX")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_TOPIC_WX;
				}
			}
			if (request->argName(i) == "TopicSensor")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_TOPIC_SENSOR;
				}
			}

			if (request->argName(i) == "subCMD")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_SUBSCRIBE_CMD;
				}
			}
			if (request->argName(i) == "subTNC")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_SUBSCRIBE_TNC;
				}
			}
			if (request->argName(i) == "subMsg")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.mqtt_topic_flag |= MQTT_SUBSCRIBE_MESSAGE;
				}
			}
		}

		config.en_mqtt = mqttEn;
		clientMQTT.disconnect();
		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(256); // Buffer for response message
		if (html)
		{
			if (saveConfiguration("/default.cfg", config))
			{
				strcpy(html, "Setup completed successfully");
				request->send(200, "text/html", html); // send to someones browser when asked
			}
			else
			{
				strcpy(html, "Save config failed.");
				request->send(501, "text/html", html); // Not Implemented
			}
			free(html); // Free the allocated memory
		}
	}
	else
	{
		JsonDocument data;
		data["enabled"] = config.en_mqtt;
		data["host"] = config.mqtt_host;
		data["port"] = config.mqtt_port;
		data["user"] = config.mqtt_user;
		data["pass"] = config.mqtt_pass;
		data["topic"] = config.mqtt_topic;
		data["subscribe"] = config.mqtt_subscribe;
		data["topicTNC"] = (bool)(config.mqtt_topic_flag & MQTT_TOPIC_TNC);
		data["topicSts"] = (bool)(config.mqtt_topic_flag & MQTT_TOPIC_STATUS);
		data["topicTlm"] = (bool)(config.mqtt_topic_flag & MQTT_TOPIC_TELEMETRY);
		data["topicWX"] = (bool)(config.mqtt_topic_flag & MQTT_TOPIC_WX);
		data["topicSensor"] = (bool)(config.mqtt_topic_flag & MQTT_TOPIC_SENSOR);
		data["subCMD"] = (bool)(config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_CMD);
		data["subTNC"] = (bool)(config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_TNC);
		data["subMsg"] = (bool)(config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_MESSAGE);

		String html_str = renderTemplate(MQTT_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "MQTT");
	}
}
#endif

// v2.1.2-lu6jmf: format a minute count as a short human string (min / h / days)
static void formatMinutesShort(char *buf, size_t bufSize, unsigned long totalMinutes)
{
	if (totalMinutes < 60)
	{
		snprintf(buf, bufSize, "%lumin", totalMinutes);
	}
	else if (totalMinutes < 1440)
	{
		unsigned long h = totalMinutes / 60;
		unsigned long m = totalMinutes % 60;
		if (m == 0)
			snprintf(buf, bufSize, "%luh", h);
		else
			snprintf(buf, bufSize, "%luh%02lumin", h, m);
	}
	else
	{
		unsigned long d = totalMinutes / 1440;
		unsigned long h = (totalMinutes % 1440) / 60;
		if (h == 0)
			snprintf(buf, bufSize, "%lud", d);
		else
			snprintf(buf, bufSize, "%lud%luh", d, h);
	}
}

// v2.1.2-lu6jmf: preview of the resulting NEWS send schedule (growing-gap: msg1 immediate,
// then gaps 2T,3T,4T,5T,... -> cumulative offsets 0, 2T, 5T, 9T, 14T, 20T, ...)
static void buildNewsSchedulePreview(char *buf, size_t bufSize, uint16_t baseTMinutes)
{
	if (baseTMinutes < 5)
		baseTMinutes = 5;
	buf[0] = 0;
	char part[16];
	for (uint8_t k = 0; k <= 5; k++)
	{
		unsigned long offsetMin = (k == 0) ? 0UL : (unsigned long)baseTMinutes * k * (k + 3) / 2UL;
		if (k == 0)
			strlcpy(part, "0", sizeof(part));
		else
			formatMinutesShort(part, sizeof(part), offsetMin);
		if (k > 0)
			strlcat(buf, " -> ", bufSize);
		strlcat(buf, part, bufSize);
	}
	strlcat(buf, " -> ...", bufSize);
}

static const char *MSG_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formMSG\") document.getElementById(\"submitMSG\").disabled=true;\n"
	"if(e.currentTarget.id===\"formBLN\") document.getElementById(\"submitBLN\").disabled=true;\n"
	"if(e.currentTarget.id===\"formChat\") document.getElementById(\"submitChat\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/msg',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"if(e.currentTarget.id===\"formMSG\") alert(\"Submited Successfully\");\n"
	"if(e.currentTarget.id===\"formBLN\") alert(\"Submited Successfully\");\n"
	"if(e.currentTarget.id===\"formChat\") { document.getElementById(\"submitChat\").disabled=false; document.getElementById(\"msg\").value=\"\"; }\n"
	"},\n"
	"error: function (data) {\n"
	"if(e.currentTarget.id===\"formMSG\") alert(\"An error occurred.\");\n"
	"if(e.currentTarget.id===\"formBLN\") alert(\"An error occurred.\");\n"
	"if(e.currentTarget.id===\"formChat\") { alert(\"An error occurred.\"); document.getElementById(\"submitChat\").disabled=false; }\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<div style=\"text-align:right;color:#999;font-size:7pt;padding:2px 4px;\">{{buildInfo}}</div>\n"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formMSG\" method=\"post\">\n"
	"<table width=\"90%\">\n"
	"<th colspan=\"2\"><span><b>Message Configuration</b></span></th>\n"
	"<tr>"
	"<td align=\"right\"><b>Enable</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" {{#if msgEnabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>My Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input  size=\"20\" maxlength=\"9\" name=\"mycall\" type=\"text\" value=\"{{myCall}}\" /> *<i>Callsign with SSID (Ex. HS5TQA-12)</i></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"msgRf\" value=\"OK\" {{#if msgRf}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"msgInet\" value=\"OK\" {{#if msgInet}}checked{{/if}}/>Internet </td></tr>\n"
	"<tr>"
	"<td align=\"right\"><b>Encryption</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"encrypt\" value=\"OK\" {{#if msgEncrypt}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>AES Key:</b></td>\n"
	"<td style=\"text-align: left;\"><input  size=\"40\" maxlength=\"33\" name=\"key\" type=\"text\" value=\"{{key}}\" /> *<i>ASCII HEX 16Byte</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Send Retry:</b></td>\n"
	"<td style=\"text-align: left;\"><input  min=\"0\" max=\"99\"   name=\"retry\" type=\"number\" value=\"{{retry}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Send Timeout:</b></td>\n"
	"<td style=\"text-align: left;\"><input  min=\"1\" max=\"9999\"   name=\"timeout\" type=\"number\" value=\"{{timeout}}\" /> Sec.</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"path\" id=\"path\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitMSG'  name=\"commitMSG\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitMSG\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br /><br />"
	"<table width=\"90%\">\n"
	"<th style=\"background-color: #070ac2;\">CHAT MESSAGE</th>\n"
	"<tr><td>\n"
	"<table id=\"chatMsg\">\n"
	"{{chatMessageHtml}}"
	"</table>\n"
	"</td></tr><tr><td colspan=\"5\">"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formChat\" method=\"post\">\n"
	"<table>\n"
	"<tr>\n"
	"<td align=\"left\"><b>TO:</b><input size=\"10\" name=\"toCall\" id=\"toCall\" type=\"text\" value=\"\" oninput=\"this.value=this.value.toUpperCase();\" /> <b>MSG:</b><input size=\"80\" name=\"msg\" id=\"msg\" type=\"text\" value=\"\" /></td>\n"
	"<td align=\"right\">\n"
	"<input class=\"button\" id=\"submitChat\" name=\"commitChat\" type=\"submit\" value=\"Send\"/>\n"
	"<input type=\"hidden\" name=\"commitChat\"/>\n"
	"</td></tr></table>\n"
	"</form><br />\n"
	"</td></tr></table><br />\n"
	"<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formBLN\" method=\"post\">\n"
	"<table width=\"90%\" style=\"table-layout:fixed;border-collapse:collapse;\">\n"
	"<th colspan=\"7\" style=\"background-color: #070ac2;\"><span><b>Bulletins: BLN1-BLN4 Alerts, NEWS5-NEWS9</b></span></th>\n"
	"<tr><td colspan=\"7\"><i>Uses the same TX Channel/PATH as Message Configuration above.</i></td></tr>\n"
	"<tr>"
	"<td align=\"center\" style=\"width:6%;\"><b>#</b></td>"
	"<td align=\"center\" style=\"width:8%;\"><b>Enable</b></td>"
	"<td align=\"center\" style=\"width:38%;\"><b>Text</b></td>"
	"<td align=\"center\" style=\"width:14%;\"><b>Interval</b></td>"
	"<td align=\"center\" style=\"width:12%;\"><b>Limit</b><br /><i style=\"font-weight:normal;font-size:8pt;\">0 = unlimited</i></td>"
	"<td align=\"center\" style=\"width:14%;\"><b>Active for</b></td>"
	"<td align=\"center\" style=\"width:8%;\"><b>Sent</b></td>"
	"</tr>\n"
	"{{#each blnRows}}<tr>\n"
	"{{#if isNews}}<td align=\"center\"><b>NEWS{{num}}</b></td>\n"
	"{{else}}<td align=\"center\"><b>BLN{{num}}</b></td>\n"
	"{{/if}}"
	"<td align=\"center\"><label class=\"switch\"><input type=\"checkbox\" name=\"blnEn{{num}}\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"<td style=\"text-align: left;\"><input id=\"blnTxt{{num}}\" style=\"width:96%;box-sizing:border-box;\" maxlength=\"{{maxLen}}\" name=\"blnText{{num}}\" type=\"text\" value=\"{{text}}\" oninput=\"var n=this.maxLength-this.value.length;var c=document.getElementById('blnCnt{{num}}');c.textContent=n+' left';c.style.color=(n<0)?'red':'#2e7d32';\" /><br />"
	"<span id=\"blnCnt{{num}}\" style=\"font-size:8pt;color:{{charsLeftColor}};\">{{charsLeft}} left</span></td>\n"
	"<td style=\"text-align: left;\"><select style=\"width:96%;box-sizing:border-box;\" name=\"blnInv{{num}}\">{{#each intervalOptions}}<option value=\"{{value}}\"{{#if selected}} selected{{/if}}>{{label}}</option>{{/each}}</select></td>\n"
	"<td style=\"text-align: left;\"><input style=\"width:90%;box-sizing:border-box;\" min=\"0\" max=\"65535\" name=\"blnLim{{num}}\" type=\"number\" value=\"{{limit}}\" /></td>\n"
	"<td style=\"text-align: left;\"><select style=\"width:96%;box-sizing:border-box;\" name=\"blnDur{{num}}\">{{#each durationOptions}}<option value=\"{{value}}\"{{#if selected}} selected{{/if}}>{{label}}</option>{{/each}}</select></td>\n"
	"<td align=\"center\">{{sentCount}}</td>\n"
	"</tr>\n"
	"{{#if isNews}}<tr><td colspan=\"2\"></td><td colspan=\"5\" style=\"text-align:left;font-size:7pt;color:#555;padding-top:0;padding-bottom:4px;\">sends: {{schedPreview}}</td></tr>\n"
	"{{/if}}"
	"{{/each}}"
	"<tr><td colspan=\"7\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitBLN' name=\"commitBLN\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitBLN\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />\n"
	"<div style=\"margin:8px 0;padding:6px;background:#fff3cd;border:1px solid #ffe69c;font-size:9pt;\">Objects (Object1-Object4) moved to their own page - see the <b>OBJ</b> tab. They use the same TX Channel/PATH configured above in Message Configuration.</div>\n";

// LU6JMF - Marcelo Ferreira (Set/2026): Objects split out of the MSG page into
// its own page/tab. Same fields, same commitObj POST contract, just no longer
// competing with BLN/News/Chat for space on one already-huge page.
static const char *OBJECT_TEMPLATE = R"OBJHTML(<script type="text/javascript">
$('form').submit(function (e) {
e.preventDefault();
var data = new FormData(e.currentTarget);
document.getElementById("submitObj").disabled=true;
$.ajax({
url: '/object',
type: 'POST',
data: data,
contentType: false,
processData: false,
success: function (data) {
alert("Submited Successfully");
document.getElementById("submitObj").disabled=false;
},
error: function (data) {
alert("An error occurred.");
document.getElementById("submitObj").disabled=false;
}
});
});
</script>
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="formObj" method="post">
<table width="90%" style="border-collapse:collapse;">
<th colspan="2" style="background-color: #070ac2;"><span><b>Objects: Object1-Object4</b></span></th>
<tr><td colspan="2"><i>Uses the same TX Channel/PATH as Message Configuration (MSG tab).</i></td></tr>
{{#each objRows}}<tr><td colspan="2" style="background-color:#eef0f4;"><b>Object{{num}}</b></td></tr>
<tr><td align="right"><b>Enable:</b></td><td align="left"><label class="switch"><input type="checkbox" name="objEn{{num}}" value="OK"{{#if enabled}} checked{{/if}}><span class="slider round"></span></label></td></tr>
<tr><td align="right"><b>Item/Obj Name:</b></td><td align="left"><input maxlength="9" name="objName{{num}}" type="text" value="{{name}}" />{{#if firstRow}} <i>3-9 character</i>{{/if}}</td></tr>
<tr><td align="right"><b>Latitude / Longitude:</b></td><td align="left"><input style="width:120px;" step="any" name="objLat{{num}}" type="number" value="{{lat}}" /> <input style="width:120px;" step="any" name="objLon{{num}}" type="number" value="{{lon}}" />{{#if firstRow}} <i>independent of the digi's own position</i>{{/if}}</td></tr>
<tr><td align="right"><b>Symbol:</b></td><td align="left">Table:<input maxlength="1" size="1" id="obj{{num}}Table" name="obj{{num}}Table" type="text" value="{{symTable}}" style="background-color: rgb(97, 239, 170);" oninput="objSymRefresh({{idx}});" /> Symbol:<input maxlength="1" size="1" id="obj{{num}}Symbol" name="obj{{num}}Symbol" type="text" value="{{symSymbol}}" style="background-color: rgb(97, 239, 170);" oninput="objSymRefresh({{idx}});" /> <img border="1" style="vertical-align: middle;" id="obj{{num}}ImgSymbol" onclick="openWindowSymbolObj({{idx}});" src="http://aprs.nakhonthai.net/symbols/icons/{{symImgNum}}-{{symImgTable}}.png">{{#if firstRow}} <i>*Click icon or type Table/Symbol directly</i>{{/if}}</td></tr>
<tr><td align="right"><b>Text/Comment:</b></td><td align="left"><input style="width:96%;box-sizing:border-box;" maxlength="{{maxLen}}" name="objText{{num}}" type="text" value="{{text}}" oninput="var n=this.maxLength-this.value.length;var c=document.getElementById('objCnt{{num}}');c.textContent=n+' left';c.style.color=(n<0)?'red':'#2e7d32';" /><br /><span id="objCnt{{num}}" style="font-size:8pt;color:{{charsLeftColor}};">{{charsLeft}} left</span>{{#if firstRow}} <i style="font-size:8pt;">(own buffer, not shared)</i>{{/if}}</td></tr>
<tr><td align="right"><b>Interval:</b></td><td align="left"><input type="radio" name="objMode{{num}}" value="0"{{#if fixedMode}} checked{{/if}}> Fixed: <select name="objInv{{num}}">{{#each intervalOptions}}<option value="{{value}}"{{#if selected}} selected{{/if}}>{{label}}</option>{{/each}}</select> + max sends <input style="width:60px;" min="0" max="65535" name="objLim{{num}}" type="number" value="{{limit}}" /><br /><input type="radio" name="objMode{{num}}" value="1"{{#if fixedMode}}{{else}} checked{{/if}}> Active for: <select name="objActFor{{num}}">{{#each afOptions}}<option value="{{value}}"{{#if selected}} selected{{/if}}>{{label}}</option>{{/each}}{{#if firstRow}}</select><br /><i>either mode (Permanent off): never exceeds 72h total send time</i></td></tr>
{{else}}</select></td></tr>
{{/if}}
<tr><td align="right"><b>Permanent / Never disable:</b></td><td align="left"><label class="switch"><input type="checkbox" name="objPerm{{num}}" value="OK"{{#if permanent}} checked{{/if}}><span class="slider round"></span></label>{{#if firstRow}} <i>if checked, ignores Interval and the 72h cap entirely</i>{{/if}}</td></tr>
{{#if firstRow}}<tr><td align="right"><b>Timestamp:</b></td><td align="left"><i>always UTC (zulu), automatic - not user configurable</i></td></tr>
{{else}}<tr><td align="right"><b>Timestamp:</b></td><td align="left"><i>always UTC, automatic</i></td></tr>
{{/if}}
{{/each}}
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitObj' name="commitObj"> Apply Change </button></div>
<input type="hidden" name="commitObj"/>
</td></tr></table><br />
</form><br />
<script type="text/javascript">
function openWindowSymbolObj(sel) {
window.open("/symbol?sel="+sel.toString(), null, "height=400,width=400,status=no,toolbar=no,menubar=no,location=no");
}
function setValue(sel,symbol,table) {
var txtsymbol=document.getElementById('obj'+(sel+1)+'Symbol');
var txttable=document.getElementById('obj'+(sel+1)+'Table');
var imgicon=document.getElementById('obj'+(sel+1)+'ImgSymbol');
txtsymbol.value = String.fromCharCode(symbol);
if(table==1){
 txttable.value='/';
}else if(table==2){
 txttable.value='\\';
}
imgicon.src = "http://aprs.nakhonthai.net/symbols/icons/"+symbol.toString()+'-'+table.toString()+'.png';
}
function objSymRefresh(sel) {
var txtsymbol=document.getElementById('obj'+(sel+1)+'Symbol');
var txttable=document.getElementById('obj'+(sel+1)+'Table');
var imgicon=document.getElementById('obj'+(sel+1)+'ImgSymbol');
var sym=txtsymbol.value.charCodeAt(0);
var tbl=(txttable.value=='\\')?2:1;
if(!isNaN(sym)&&sym>0){
 imgicon.src = "http://aprs.nakhonthai.net/symbols/icons/"+sym.toString()+'-'+tbl.toString()+'.png';
}
}
</script>
)OBJHTML";

void handle_object(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitObj"))
	{
		// v2.1-lu6jmf: Object1-Object4
		bool objEn[4] = {false};
		char objName[4][10];
		double objLat[4] = {0}, objLon[4] = {0};
		char objSymbol[4][3];
		char objText[4][STATUS_SIZE];
		uint8_t objMode[4] = {0};
		uint16_t objInterval[4], objLimit[4] = {0};
		uint8_t objActiveFor[4] = {0};
		bool objPermanent[4] = {false};
		for (uint8_t oi = 0; oi < 4; oi++)
		{
			// LU6JMF fix (Set/2026): pre-seed every field from the current config,
			// not just symbol/lat/lon/interval. Without this, any commitObj request
			// that doesn't carry literally every field for every slot (e.g. another
			// device/tab submitting the form) silently wipes name/text/limit/
			// active_for back to defaults for the slots it didn't include.
			// NOTE: objEn/objPermanent stay default-false (declared above) on purpose -
			// they're checkboxes, and an unchecked checkbox is simply absent from the
			// POST body (standard HTML behavior). Pre-seeding those two from config
			// would make it impossible to ever uncheck them via a normal full submit.
			strlcpy(objName[oi], config.obj_name[oi], sizeof(objName[oi]));
			strlcpy(objText[oi], config.obj_text[oi], sizeof(objText[oi]));
			objSymbol[oi][0] = config.obj_symbol[oi][0] ? config.obj_symbol[oi][0] : '/';
			objSymbol[oi][1] = config.obj_symbol[oi][1] ? config.obj_symbol[oi][1] : 'r';
			objSymbol[oi][2] = 0;
			objLat[oi] = config.obj_lat[oi];
			objLon[oi] = config.obj_lon[oi];
			objMode[oi] = config.obj_mode[oi];
			objInterval[oi] = config.obj_interval[oi] ? config.obj_interval[oi] : 900;
			objLimit[oi] = config.obj_limit[oi];
			objActiveFor[oi] = config.obj_activefor[oi];
		}
		for (uint8_t i = 0; i < request->args(); i++)
		{
			String argName = request->argName(i);
			for (uint8_t oi = 0; oi < 4; oi++)
			{
				char f[16];
				snprintf(f, sizeof(f), "objEn%d", oi + 1);
				if (argName == f && request->arg(i) == "OK")
					objEn[oi] = true;
				snprintf(f, sizeof(f), "objName%d", oi + 1);
				if (argName == f)
					strlcpy(objName[oi], request->arg(i).c_str(), sizeof(objName[oi]));
				snprintf(f, sizeof(f), "objLat%d", oi + 1);
				if (argName == f)
					objLat[oi] = request->arg(i).toFloat(); // Arduino String has no toDouble()
				snprintf(f, sizeof(f), "objLon%d", oi + 1);
				if (argName == f)
					objLon[oi] = request->arg(i).toFloat();
				snprintf(f, sizeof(f), "obj%dTable", oi + 1);
				if (argName == f && request->arg(i).length() > 0)
					objSymbol[oi][0] = request->arg(i)[0];
				snprintf(f, sizeof(f), "obj%dSymbol", oi + 1);
				if (argName == f && request->arg(i).length() > 0)
					objSymbol[oi][1] = request->arg(i)[0];
				snprintf(f, sizeof(f), "objText%d", oi + 1);
				if (argName == f)
					strlcpy(objText[oi], request->arg(i).c_str(), sizeof(objText[oi]));
				snprintf(f, sizeof(f), "objMode%d", oi + 1);
				if (argName == f)
					objMode[oi] = request->arg(i).toInt();
				snprintf(f, sizeof(f), "objInv%d", oi + 1);
				if (argName == f && isValidNumber(request->arg(i)))
					objInterval[oi] = request->arg(i).toInt();
				snprintf(f, sizeof(f), "objLim%d", oi + 1);
				if (argName == f && isValidNumber(request->arg(i)))
					objLimit[oi] = request->arg(i).toInt();
				snprintf(f, sizeof(f), "objActFor%d", oi + 1);
				if (argName == f && isValidNumber(request->arg(i)))
					objActiveFor[oi] = request->arg(i).toInt();
				snprintf(f, sizeof(f), "objPerm%d", oi + 1);
				if (argName == f && request->arg(i) == "OK")
					objPermanent[oi] = true;
			}
		}
		for (uint8_t oi = 0; oi < 4; oi++)
		{
			config.obj_en[oi] = objEn[oi];
			strlcpy(config.obj_name[oi], objName[oi], sizeof(config.obj_name[oi]));
			config.obj_lat[oi] = objLat[oi];
			config.obj_lon[oi] = objLon[oi];
			config.obj_symbol[oi][0] = objSymbol[oi][0];
			config.obj_symbol[oi][1] = objSymbol[oi][1];
			config.obj_symbol[oi][2] = 0;
			strlcpy(config.obj_text[oi], objText[oi], sizeof(config.obj_text[oi]));
			config.obj_mode[oi] = objMode[oi];
			config.obj_interval[oi] = objInterval[oi];
			config.obj_limit[oi] = objLimit[oi];
			config.obj_activefor[oi] = objActiveFor[oi];
			config.obj_permanent[oi] = objPermanent[oi];
		}
		char *html = allocateStringMemory(256);
		if (html)
		{
			if (saveConfiguration("/default.cfg", config))
			{
				strcpy(html, "Objects updated");
				request->send(200, "text/html", html);
			}
			else
			{
				strcpy(html, "Save config failed.");
				request->send(501, "text/html", html);
			}
			free(html);
		}
	}
	else
	{
		esp_task_wdt_reset();
		JsonDocument data;

		static const uint16_t objIvlOpts[3] = {900, 1800, 3600};
		static const char *objIvlLabels[3] = {"15 min", "30 min", "60 min"};
		static const uint8_t objAfOpts[4] = {24, 36, 48, 72};

		JsonArray objRows = data["objRows"].to<JsonArray>();
		for (uint8_t oi = 0; oi < 4; oi++)
		{
			JsonObject o = objRows.add<JsonObject>();
			o["num"] = oi + 1;
			o["idx"] = oi;
			o["firstRow"] = (oi == 0);
			o["enabled"] = config.obj_en[oi];
			o["name"] = config.obj_name[oi];

			char buf[32];
			snprintf(buf, sizeof(buf), "%.6f", config.obj_lat[oi]);
			o["lat"] = buf;
			snprintf(buf, sizeof(buf), "%.6f", config.obj_lon[oi]);
			o["lon"] = buf;

			char objTableCh = config.obj_symbol[oi][0] ? config.obj_symbol[oi][0] : '/';
			char objSymCh = config.obj_symbol[oi][1] ? config.obj_symbol[oi][1] : 'r';
			int objTableNum = (objTableCh == '\\') ? 2 : 1;
			char cbuf[2];
			cbuf[0] = objTableCh;
			cbuf[1] = 0;
			o["symTable"] = cbuf;
			cbuf[0] = objSymCh;
			cbuf[1] = 0;
			o["symSymbol"] = cbuf;
			o["symImgNum"] = (int)objSymCh;
			o["symImgTable"] = objTableNum;

			int objCharsLeft = (int)(STATUS_SIZE - 1) - (int)strlen(config.obj_text[oi]);
			o["text"] = config.obj_text[oi];
			o["maxLen"] = STATUS_SIZE - 1;
			o["charsLeftColor"] = (objCharsLeft < 0) ? "red" : "#2e7d32";
			o["charsLeft"] = objCharsLeft;

			bool fixedMode = (config.obj_mode[oi] == 0);
			o["fixedMode"] = fixedMode;

			JsonArray intervalOptions = o["intervalOptions"].to<JsonArray>();
			for (uint8_t k = 0; k < 3; k++)
			{
				JsonObject io = intervalOptions.add<JsonObject>();
				io["value"] = objIvlOpts[k];
				io["label"] = objIvlLabels[k];
				io["selected"] = (config.obj_interval[oi] == objIvlOpts[k]);
			}

			o["limit"] = config.obj_limit[oi];

			JsonArray afOptions = o["afOptions"].to<JsonArray>();
			for (uint8_t k = 0; k < 4; k++)
			{
				JsonObject ao = afOptions.add<JsonObject>();
				ao["value"] = objAfOpts[k];
				ao["label"] = String(objAfOpts[k]) + "h";
				ao["selected"] = (config.obj_activefor[oi] == objAfOpts[k]);
			}

			o["permanent"] = config.obj_permanent[oi];
		}

		String html_str = renderTemplate(OBJECT_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "Object");
	}
}

void handle_msg(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitChat"))
	{
		// Using char arrays instead of String
		char toCall[10];
		char msg[256];
		memset(toCall, 0, sizeof(toCall));
		memset(msg, 0, sizeof(msg));

		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "toCall")
			{
				if (request->arg(i) != "")
				{
					strncpy(toCall, request->arg(i).c_str(), sizeof(toCall) - 1);
				}
			}
			if (request->argName(i) == "msg")
			{
				if (request->arg(i) != "")
				{
					strncpy(msg, request->arg(i).c_str(), sizeof(msg) - 1);
				}
			}
		}
		log_d("Chat to %s | msg %s", toCall, msg);

		// LU6JMF - Marcelo Ferreira:
		// Server-side duplicate guard. The client-side disabled-button fix only
		// covers one click's own AJAX round-trip; it does not catch the browser
		// firing two POSTs for what the user experienced as a single click/enter
		// (seen live: two independent sendAPRSMessage() calls, same toCall+text,
		// about a second apart). Ignore an identical toCall+msg repeated within
		// a few seconds instead of sending it again.
		static char lastChatTo[10] = {0};
		static char lastChatMsg[256] = {0};
		static unsigned long lastChatMillis = 0;
		bool isDuplicate = (millis() - lastChatMillis < 5000) &&
							(strcmp(toCall, lastChatTo) == 0) &&
							(strcmp(msg, lastChatMsg) == 0) &&
							(lastChatMillis != 0);

		if (!isDuplicate)
		{
			sendAPRSMessage(String(toCall), String(msg), config.msg_encrypt);
			strncpy(lastChatTo, toCall, sizeof(lastChatTo) - 1);
			strncpy(lastChatMsg, msg, sizeof(lastChatMsg) - 1);
			lastChatMillis = millis();
		}
		else
		{
			log_d("Chat duplicate ignored (same toCall+msg within 5s)");
		}

		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(64); // Small buffer for "Send completed"
		if (html)
		{
			strcpy(html, isDuplicate ? "Duplicate ignored" : "Send completed");
			request->send(200, "text/html", html); // send to someones browser when asked
			free(html);							   // Free the allocated memory
		}
	}
	else if (request->hasArg("commitBLN"))
	{
		bool blnEn[9] = {false};
		char blnText[9][STATUS_SIZE];
		uint16_t blnInterval[9];
		uint16_t blnLimit[9];
		uint8_t blnDuration[9]; // LU6JMF - Marcelo Ferreira: 24/36/48/72h campaign lifetime (replaces bln_activefor)
		for (uint8_t bi = 0; bi < 9; bi++)
		{
			// LU6JMF fix (Set/2026): pre-seed text from the current config, same as
			// commitObj. Without this, a commitBLN request that doesn't carry every
			// field for every slot (e.g. another device/tab submitting the form)
			// silently wipes the bulletin/news text back to empty for the slots it
			// didn't include.
			// NOTE: blnEn stays default-false (declared above) on purpose - it's a
			// checkbox, and an unchecked checkbox is simply absent from the POST
			// body (standard HTML behavior). Pre-seeding it from config would make
			// it impossible to ever uncheck it via a normal full submit.
			strlcpy(blnText[bi], config.bln_text[bi], sizeof(blnText[bi]));
			blnInterval[bi] = config.bln_interval[bi];
			blnLimit[bi] = config.bln_limit[bi];
			blnDuration[bi] = config.bln_duration[bi];
		}

		for (uint8_t i = 0; i < request->args(); i++)
		{
			String argName = request->argName(i);
			for (uint8_t bi = 0; bi < 9; bi++)
			{
				char fieldEn[10], fieldText[10], fieldInt[12], fieldLimit[12], fieldDuration[12];
				snprintf(fieldEn, sizeof(fieldEn), "blnEn%d", bi + 1);
				snprintf(fieldText, sizeof(fieldText), "blnText%d", bi + 1);
				snprintf(fieldInt, sizeof(fieldInt), "blnInv%d", bi + 1);
				snprintf(fieldLimit, sizeof(fieldLimit), "blnLim%d", bi + 1);
				snprintf(fieldDuration, sizeof(fieldDuration), "blnDur%d", bi + 1);

				if (argName == fieldEn)
				{
					if (request->arg(i) == "OK")
						blnEn[bi] = true;
				}
				if (argName == fieldText)
				{
					strlcpy(blnText[bi], request->arg(i).c_str(), sizeof(blnText[bi]));
				}
				if (argName == fieldInt)
				{
					// LU6JMF - Marcelo Ferreira: interval now stored in minutes.
					if (isValidNumber(request->arg(i)))
						blnInterval[bi] = request->arg(i).toInt();
				}
				if (argName == fieldLimit)
				{
					// Left blank by the user = unlimited (0); a number = hard stop after that many sends
					if (isValidNumber(request->arg(i)))
						blnLimit[bi] = request->arg(i).toInt();
					else
						blnLimit[bi] = 0;
				}
				if (argName == fieldDuration)
				{
					if (isValidNumber(request->arg(i)))
						blnDuration[bi] = request->arg(i).toInt();
				}
			}
		}

		for (uint8_t bi = 0; bi < 9; bi++)
		{
			const bool wasEnabled = config.bln_en[bi];
			const bool turningOn = (!wasEnabled && blnEn[bi]);

			// LU6JMF - Marcelo Ferreira:
			// Normalize user input before comparing/persisting campaign settings.
			uint16_t normalizedInterval = blnInterval[bi];
			if (normalizedInterval == 0)
				normalizedInterval = 30;
			if (normalizedInterval > 1440)
				normalizedInterval = 1440;

			uint8_t normalizedDuration = blnDuration[bi];
			if (normalizedDuration != 24 &&
			    normalizedDuration != 36 &&
			    normalizedDuration != 48 &&
			    normalizedDuration != 72)
			{
				normalizedDuration = 72;
			}

			const bool textChanged =
				(strncmp(config.bln_text[bi], blnText[bi], sizeof(config.bln_text[bi])) != 0);
			const bool intervalChanged = (config.bln_interval[bi] != normalizedInterval);
			const bool limitChanged = (config.bln_limit[bi] != blnLimit[bi]);
			const bool durationChanged = (config.bln_duration[bi] != normalizedDuration);

			const bool restartCampaign =
				blnEn[bi] &&
				(turningOn || textChanged || intervalChanged || limitChanged || durationChanged);

			config.bln_en[bi] = blnEn[bi];
			strlcpy(config.bln_text[bi], blnText[bi], sizeof(config.bln_text[bi]));
			config.bln_interval[bi] = normalizedInterval;
			config.bln_limit[bi] = blnLimit[bi];
			config.bln_duration[bi] = normalizedDuration;

			if (!blnEn[bi])
			{
				// Disabled means there is no active campaign.
				config.bln_started_at[bi] = 0;
				config.bln_sent_total[bi] = 0;
				blnSentCount[bi] = 0;
				blnRestartPending[bi] = false;
			}
			else if (restartCampaign)
			{
				// taskAPRS owns the exact calendar start and immediate TX1.
				config.bln_started_at[bi] = 0;
				config.bln_sent_total[bi] = 0;
				blnSentCount[bi] = 0;
				blnRestartPending[bi] = true;
			}
			else
			{
				// Keep web display synchronized with persistent campaign state.
				blnSentCount[bi] = config.bln_sent_total[bi];
			}
		}

		char *html = allocateStringMemory(256);
		if (html)
		{
			if (saveConfiguration("/default.cfg", config))
			{
				strcpy(html, "Setup completed successfully");
				request->send(200, "text/html", html);
			}
			else
			{
				strcpy(html, "Save config failed.");
				request->send(501, "text/html", html);
			}
			free(html);
		}
	}
	else if (request->hasArg("commitMSG"))
	{
		bool msgEn = false;
		bool msgRf = false;
		bool msgInet = false;
		bool msgEncrypt = false;

		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "enable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						msgEn = true;
				}
			}
			if (request->argName(i) == "msgRf")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						msgRf = true;
				}
			}
			if (request->argName(i) == "msgInet")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						msgInet = true;
				}
			}
			if (request->argName(i) == "encrypt")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						msgEncrypt = true;
				}
			}

			if (request->argName(i) == "mycall")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.msg_mycall, request->arg(i).c_str());
					config.msg_mycall[9] = 0;
				}
			}

			if (request->argName(i) == "key")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.msg_key, request->arg(i).c_str());
					config.msg_key[32] = 0;
				}
			}

			if (request->argName(i) == "retry")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.msg_retry = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "path")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.msg_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "timeout")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.msg_interval = request->arg(i).toInt();
				}
			}
		}

		config.msg_enable = msgEn;
		config.msg_rf = msgRf;
		config.msg_inet = msgInet;
		config.msg_encrypt = msgEncrypt;

		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(256); // Buffer for response message
		if (html)
		{
			if (saveConfiguration("/default.cfg", config))
			{
				strcpy(html, "Setup completed successfully");
				request->send(200, "text/html", html); // send to someones browser when asked
			}
			else
			{
				strcpy(html, "Save config failed.");
				request->send(501, "text/html", html); // Not Implemented
			}
			free(html); // Free the allocated memory
		}
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_msg runs on the async_tcp task itself,
		// and this is the largest page in the firmware (BLN/News x9, Objects x4, chat table) -
		// can take long enough under heap pressure to trip the TWDT if never fed.
		JsonDocument data;

		// v2.1.2-lu6jmf: discreet build marker so it is unmistakable in the browser whether a
		// given reflash actually took. Also carries the last reset reason here (not on the
		// main DashBoard) so it doesn't alarm non-technical users with terms like PANIC/WDT/
		// BROWNOUT - stays available for troubleshooting without being front-and-center.
		char buildInfo[160];
		snprintf(buildInfo, sizeof(buildInfo), "build " __DATE__ " " __TIME__ " | reset: %s", lastResetReasonStr);
		data["buildInfo"] = buildInfo;

		data["msgEnabled"] = config.msg_enable;
		data["myCall"] = config.msg_mycall;
		data["msgRf"] = config.msg_rf;
		data["msgInet"] = config.msg_inet;
		data["msgEncrypt"] = config.msg_encrypt;
		data["key"] = config.msg_key;
		data["retry"] = config.msg_retry;
		data["timeout"] = config.msg_interval;

		JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			JsonObject o = pathOptions.add<JsonObject>();
			o["value"] = pthIdx;
			o["label"] = PATH_NAME[pthIdx];
			o["selected"] = (config.msg_path == pthIdx);
		}

		data["chatMessageHtml"] = event_chatMessage(true);
		esp_task_wdt_reset();

		static const uint16_t blnIntervalOpts[5] = {5, 10, 15, 30, 60};
		static const char *blnIntervalLabels[5] = {"5 min", "10 min", "15 min", "30 min", "60 min"};
		static const uint8_t blnDurOpts[4] = {24, 36, 48, 72};

		JsonArray blnRows = data["blnRows"].to<JsonArray>();
		for (uint8_t bi = 0; bi < 9; bi++)
		{
			JsonObject r = blnRows.add<JsonObject>();
			r["num"] = bi + 1;
			r["isNews"] = (bi >= 4);
			r["enabled"] = config.bln_en[bi];
			r["text"] = config.bln_text[bi];
			r["maxLen"] = STATUS_SIZE - 1;

			int blnCharsLeft = (int)(STATUS_SIZE - 1) - (int)strlen(config.bln_text[bi]);
			r["charsLeftColor"] = (blnCharsLeft < 0) ? "red" : "#2e7d32";
			r["charsLeft"] = blnCharsLeft;

			JsonArray intervalOptions = r["intervalOptions"].to<JsonArray>();
			for (uint8_t k = 0; k < 5; k++)
			{
				JsonObject o = intervalOptions.add<JsonObject>();
				o["value"] = blnIntervalOpts[k];
				o["label"] = blnIntervalLabels[k];
				o["selected"] = (config.bln_interval[bi] == blnIntervalOpts[k]);
			}

			r["limit"] = config.bln_limit[bi];

			JsonArray durationOptions = r["durationOptions"].to<JsonArray>();
			for (uint8_t k = 0; k < 4; k++)
			{
				JsonObject o = durationOptions.add<JsonObject>();
				o["value"] = blnDurOpts[k];
				o["label"] = String(blnDurOpts[k]) + "h";
				o["selected"] = (config.bln_duration[bi] == blnDurOpts[k]);
			}

			r["sentCount"] = blnSentCount[bi];

			if (bi >= 4)
			{
				uint16_t baseTMin = config.bln_interval[bi];
				if (baseTMin < 5)
					baseTMin = 5;
				char schedPreview[220];
				buildNewsSchedulePreview(schedPreview, sizeof(schedPreview), baseTMin);
				r["schedPreview"] = schedPreview;
			}
		}

		esp_task_wdt_reset();
		String html_str = renderTemplate(MSG_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks).
		// v2.1.3-lu6jmf: bumped 28000->36000 - BLN Active-for/schedule-preview rows and the
		// Objects section pushed real content past 28000, forcing a mid-stream realloc that
		// could fail under heap fragmentation and truncate the page (seen live: cut off
		// partway through Object4, Apply button + <script> with setValue() never sent).
		sendHtmlChunked(request, html_str, "MSG");
	}
}

static const char *MOD_TEMPLATE = R"MODHTML(<script type="text/javascript">
$('form').submit(function (e) {
e.preventDefault();
var data = new FormData(e.currentTarget);
if(e.currentTarget.id==="formUART0") document.getElementById("submitURAT0").disabled=true;
if(e.currentTarget.id==="formUART1") document.getElementById("submitURAT1").disabled=true;
if(e.currentTarget.id==="formUART1") document.getElementById("submitURAT1").disabled=true;
if(e.currentTarget.id==="formGNSS") document.getElementById("submitGNSS").disabled=true;
if(e.currentTarget.id==="formMODBUS") document.getElementById("submitMODBUS").disabled=true;
if(e.currentTarget.id==="formTNC") document.getElementById("submitTNC").disabled=true;
if(e.currentTarget.id==="formRF") document.getElementById("submitRF").disabled=true;
if(e.currentTarget.id==="formI2C0") document.getElementById("submitI2C0").disabled=true;
if(e.currentTarget.id==="formI2C1") document.getElementById("submitI2C1").disabled=true;
if(e.currentTarget.id==="formCOUNT0") document.getElementById("submitCOUNT0").disabled=true;
if(e.currentTarget.id==="formCOUNT1") document.getElementById("submitCOUNT1").disabled=true;
{{#if pppos}}if(e.currentTarget.id==="formPPPoS") document.getElementById("submitPPPoS").disabled=true;
{{/if}}$.ajax({
url: '/mod',
type: 'POST',
data: data,
contentType: false,
processData: false,
success: function (data) {
alert("Submited Successfully\nRequire hardware REBOOT!");
},
error: function (data) {
alert("An error occurred.");
}
});
});
</script>
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="32%" style="border:unset;">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromUART0" method="post">
<table>
<th colspan="2"><span><b>UART0 Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if uart0En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>RX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="rx" type="number" value="{{uart0Rx}}" /></td>
</tr>
<tr>
<td align="right"><b>TX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="tx" type="number" value="{{uart0Tx}}" /></td>
</tr>
<tr>
<td align="right"><b>RTS/DE GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="rts" type="number" value="{{uart0Rts}}" /></td>
</tr>
<tr>
<td align="right"><b>Baudrate:</b></td>
<td style="text-align: left;">
<select name="baudrate" id="baudrate">
{{#each uart0Baud}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} </option>
{{/each}}</select> bps
</td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitUART0" name="commitUART0" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitUART0"/>
</td></tr></table>
</form><br />
</td><td width="32%" style="border:unset;">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromUART1" method="post">
<table>
<th colspan="2"><span><b>UART1 Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if uart1En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>RX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="rx" type="number" value="{{uart1Rx}}" /></td>
</tr>
<tr>
<td align="right"><b>TX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="tx" type="number" value="{{uart1Tx}}" /></td>
</tr>
<tr>
<td align="right"><b>RTS/DE GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="rts" type="number" value="{{uart1Rts}}" /></td>
</tr>
<tr>
<td align="right"><b>Baudrate:</b></td>
<td style="text-align: left;">
<select name="baudrate" id="baudrate">
{{#each uart1Baud}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} </option>
{{/each}}</select> bps
</td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitUART1" name="commitUART1" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitUART1"/>
</td></tr></table>
</form><br />
</td></tr></table>
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromRF" method="post">
<table>
<th colspan="2"><span><b>RF GPIO Modify</b></span></th>
<tr>
<tr>
<td align="right"><b>ADC Attenuation:</b></td>
<td style="text-align: left;">
<select name="atten" id="atten">
{{#each attenOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select> DC-Offset: {{adcOffset}} mV
</td>
</tr>
<tr>
<td align="right"><b>UART Baudrate:</b></td>
<td style="text-align: left;">
<select name="baudrate" id="baudrate">
{{#each rfBaud}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} </option>
{{/each}}</select> bps
</td>
</tr>
<tr>
<td align="right"><b>UART RX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="rx" type="number" value="{{rfRx}}" /></td>
</tr>
<tr>
<td align="right"><b>UART TX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="tx" type="number" value="{{rfTx}}" /></td>
</tr>
<tr>
<td align="right"><b>PD GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="pd" type="number" value="{{rfPdGpio}}" /> Active:<input type="radio" name="pd_active" value="0" {{#if rfPdHigh}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="pd_active" value="1" {{#if rfPdHigh}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr>
<td align="right"><b>H/L GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="pwr" type="number" value="{{rfPwrGpio}}" /> Active:<input type="radio" name="pwr_active" value="0" {{#if rfPwrHigh}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="pwr_active" value="1" {{#if rfPwrHigh}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr>
<td align="right"><b>SQL GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="sql" type="number" value="{{rfSqlGpio}}" /> Active:<input type="radio" name="sql_active" value="0" {{#if rfSqlHigh}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="sql_active" value="1" {{#if rfSqlHigh}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr>
<td align="right"><b>PTT GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}"  name="ptt" type="number" value="{{rfPttGpio}}" /> Active:<input type="radio" name="ptt_active" value="0" {{#if rfPttHigh}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="ptt_active" value="1" {{#if rfPttHigh}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitRF" name="commitRF" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitRF"/>
</td></tr></table>
</form>
</td></tr></table>
<br />
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromTNC" method="post">
<table>
<th colspan="2"><span><b>External TNC Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if tncEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>PORT:</b></td>
<td style="text-align: left;">
<select name="channel" id="channel">
{{#each tncPortOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<tr>
<td align="right"><b>MODE:</b></td>
<td style="text-align: left;">
<select name="mode" id="mode">
{{#each tncModeOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitTNC" name="commitTNC" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitTNC"/>
</td></tr></table>
</form>
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromTCPKISS" method="post">
<table>
<th colspan="2"><span><b>TCP KISS Server Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if tcpKissEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>PORT 1:</b></td>
<td style="text-align: left;"><input type="text" name="port1" value="{{tcpKissPort1}}" maxlength="5" size="6"/></td>
</tr>
<tr>
<td align="right"><b>PORT 2:</b></td>
<td style="text-align: left;"><input type="text" name="port2" value="{{tcpKissPort2}}" maxlength="5" size="6"/></td>
</tr>
<tr><td colspan="2" align="center"><b>Status:</b></td></tr>
<tr><td colspan="2" align="center">{{tcpKissStatus1}}</td></tr>
<tr><td colspan="2" align="center">{{tcpKissStatus2}}</td></tr>
<tr><td colspan="2" style="word-wrap:break-word;white-space:normal;"><p style="font-size:9pt;margin:4px 0;">Lets PC software (Xastir, APRSIS32, etc) use this device as a KISS TNC over the local network - no cable needed. No password - anyone on this WiFi network can transmit through the radio via these ports while enabled. Port number changes need a reboot to take effect; the Enable switch does not.</p></td></tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitTCPKISS" name="commitTCPKISS" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitTCPKISS"/>
</td></tr></table>
</form>
</td></tr></table>
<br />
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form id='formATCommand' method="POST" action='#' enctype='multipart/form-data'>
<table>
<th colspan="2"><span><b>AT-COMMAND CHANNEL</b></span></th>
<tr>
<td width="150" align="right"><b>MQTT:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="mqtt" value="OK" {{#if cmdMqtt}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>MESSAGE:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="msg" value="OK" {{#if cmdMsg}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>BLUETOOTH:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="bluetooth" value="OK" {{#if cmdBluetooth}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>UART PORT:</b></td>
<td style="text-align: left;">
<select name="uart" id="cmdUart">
{{#each cmdUartOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitCMD'  name="commit"> Apply Change </button></div>
<input type="hidden" name="commitCMD"/>
</td></tr></table><br />
</form><br />
{{#if pppos}}<br />
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form id='formPPPoS' method="POST" action='#' enctype='multipart/form-data'>
<table>
<th colspan="2"><span><b>PPP Over Serial (GSM/4G-LTE)</b></span></th>
<tr>
<td align="right"><b>Enable:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="pppEn" value="OK" {{#if pppEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<td align="right"><b>GNSS:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="pppGnss" value="OK" {{#if pppGnss}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>NAPT:</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="pppNapt" value="OK" {{#if pppNapt}}checked{{/if}}><span class="slider round"></span></label> *WiFi NAT</td>
</tr>
<tr>
<td align="right"><b>APN:</b></td>
<td style="text-align: left;"><input maxlength="20" name="pppAPN" type="text" value="{{pppApn}}" /></td>
</tr>
<tr>
<td align="right"><b>PIN:</b></td>
<td style="text-align: left;"><input min="0" max="999999" name="pppPin" type="number" value="{{pppPin}}" /> <i>*PIN of SIM</i></td>
</tr>
<tr>
<td align="right"><b>RX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="50" name="rx" type="number" value="{{pppRx}}" /></td>
</tr>
<tr>
<td align="right"><b>TX GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="50" name="tx" type="number" value="{{pppTx}}" /></td>
</tr>
<tr>
<td align="right"><b>RESET GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="50"  name="rst" type="number" value="{{pppRstGpio}}" /> Active:<input type="radio" name="rst_active" value="0" {{#if pppRstHigh}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="rst_active" value="1" {{#if pppRstHigh}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<td align="right"><b>RESET DELAY:</b></td>
<td style="text-align: left;"><input min="0" max="999999" name="rstDly" type="number" value="{{pppRstDelay}}" /> mSec.</td>
</tr>
<tr>
<tr>
<td align="right"><b>PORT:</b></td>
<td style="text-align: left;">
<select name="port" id="port">
{{#each pppPortOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<tr>
<td align="right"><b>Baudrate:</b></td>
<td style="text-align: left;">
<select name="baudrate" id="baudrate">
{{#each pppBaud}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} </option>
{{/each}}</select> bps
</td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitPPPoS'  name="commitPPPoS"> Apply Change </button></div>
<input type="hidden" name="commitPPPoS"/>
</td></tr></table><br />
</form>
</td></tr></table>
{{/if}})MODHTML";

void handle_mod(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitUART0"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "baudrate")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart0_baudrate = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rx")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart0_rx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "tx")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart0_tx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rts")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart0_rts_gpio = request->arg(i).toInt();
				}
			}
		}

		config.uart0_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitUART1"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "baudrate")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart1_baudrate = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rx")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart1_rx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "tx")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart1_tx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rts")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.uart1_rts_gpio = request->arg(i).toInt();
				}
			}
		}

		config.uart1_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	// else if (request->hasArg("commitUART2"))
	// {
	// 	bool En = false;
	// 	for (uint8_t i = 0; i < request->args(); i++)
	// 	{
	// 		// Serial.print("SERVER ARGS ");
	// 		// Serial.print(request->argName(i));
	// 		// Serial.print("=");
	// 		// Serial.println(request->arg(i));

	// 		if (request->argName(i) == "Enable")
	// 		{
	// 			if (request->arg(i) != "")
	// 			{
	// 				if (String(request->arg(i)) == "OK")
	// 					En = true;
	// 			}
	// 		}

	// 	// 	if (request->argName(i) == "baudrate")
	// 	// 	{
	// 	// 		if (isValidNumber(request->arg(i)))
	// 	// 		{
	// 	// 			config.uart2_baudrate = request->arg(i).toInt();
	// 	// 		}
	// 	// 	}

	// 	// 	if (request->argName(i) == "rx")
	// 	// 	{
	// 	// 		if (isValidNumber(request->arg(i)))
	// 	// 		{
	// 	// 			config.uart2_rx_gpio = request->arg(i).toInt();
	// 	// 		}
	// 	// 	}

	// 	// 	if (request->argName(i) == "tx")
	// 	// 	{
	// 	// 		if (isValidNumber(request->arg(i)))
	// 	// 		{
	// 	// 			config.uart2_tx_gpio = request->arg(i).toInt();
	// 	// 		}
	// 	// 	}

	// 	// 	if (request->argName(i) == "rts")
	// 	// 	{
	// 	// 		if (isValidNumber(request->arg(i)))
	// 	// 		{
	// 	// 			config.uart2_rts_gpio = request->arg(i).toInt();
	// 	// 		}
	// 	// 	}
	// 	// }

	// 	// config.uart2_enable = En;
	// 	saveConfiguration("/default.cfg", config);
	// 	String html = "OK";
	// 	request->send(200, "text/html", html);
	// }
	else if (request->hasArg("commitTNC"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					// if (isValidNumber(request->arg(i)))
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "channel")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.ext_tnc_channel = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "mode")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.ext_tnc_mode = request->arg(i).toInt();
				}
			}
		}

		config.ext_tnc_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitTCPKISS"))
	{
		// TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}
			if (request->argName(i) == "port1")
			{
				if (isValidNumber(request->arg(i)))
				{
					int p = request->arg(i).toInt();
					if (p > 0 && p < 65536)
						config.tcp_kiss_port1 = (uint16_t)p;
				}
			}
			if (request->argName(i) == "port2")
			{
				if (isValidNumber(request->arg(i)))
				{
					int p = request->arg(i).toInt();
					if (p > 0 && p < 65536)
						config.tcp_kiss_port2 = (uint16_t)p;
				}
			}
		}
		config.tcp_kiss_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitRF"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "sql_active")
			{
				if (request->arg(i) != "")
				{
					config.rf_sql_active = (bool)request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "pd_active")
			{
				if (request->arg(i) != "")
				{
					config.rf_pd_active = (bool)request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "pwr_active")
			{
				if (request->arg(i) != "")
				{
					config.rf_pwr_active = (bool)request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "ptt_active")
			{
				if (request->arg(i) != "")
				{
					config.rf_ptt_active = (bool)request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "baudrate")
			{
				if (request->arg(i) != "")
				{
					config.rf_baudrate = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rx")
			{
				if (request->arg(i) != "")
				{
					config.rf_rx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "tx")
			{
				if (request->arg(i) != "")
				{
					config.rf_tx_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "pd")
			{
				if (request->arg(i) != "")
				{
					config.rf_pd_gpio = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "pwr")
			{
				if (request->arg(i) != "")
				{
					config.rf_pwr_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "ptt")
			{
				if (request->arg(i) != "")
				{
					config.rf_ptt_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "sql")
			{
				if (request->arg(i) != "")
				{
					config.rf_sql_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "atten")
			{
				if (request->arg(i) != "")
				{
					config.adc_atten = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "offset")
			{
				if (request->arg(i) != "")
				{
					config.adc_dc_offset = request->arg(i).toInt();
				}
			}
		}
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitCMD"))
	{
		bool mqtt = false;
		bool msg = false;
		bool bluetooth = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "mqtt")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						mqtt = true;
					}
				}
			}

			if (request->argName(i) == "msg")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						msg = true;
					}
				}
			}

			if (request->argName(i) == "bluetooth")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						bluetooth = true;
					}
				}
			}

			if (request->argName(i) == "uart")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.at_cmd_uart = request->arg(i).toInt();
				}
			}
		}
		config.at_cmd_mqtt = mqtt;
		config.at_cmd_msg = msg;
		config.at_cmd_bluetooth = bluetooth;
		String html;
		if (saveConfiguration("/default.cfg", config))
		{
			html = "Setup completed successfully";
			request->send(200, "text/html", html); // send to someones browser when asked
		}
		else
		{
			html = "Save config failed.";
			request->send(501, "text/html", html); // Not Implemented
		}
	}
#ifdef PPPOS
	else if (request->hasArg("commitPPPoS"))
	{
		bool pppEn = false;
		bool pppGnss = false;
		bool pppNapt = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "pppEn")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						pppEn = true;
					}
				}
			}

			if (request->argName(i) == "pppGnss")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						pppGnss = true;
					}
				}
			}

			if (request->argName(i) == "pppNapt")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						pppNapt = true;
					}
				}
			}

			if (request->argName(i) == "pppAPN")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.ppp_apn, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "pppPin")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.ppp_pin, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "rstDly")
			{
				if (request->arg(i) != "")
				{
					config.ppp_rst_delay = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "baudrate")
			{
				if (request->arg(i) != "")
				{
					config.ppp_serial_baudrate = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "port")
			{
				if (request->arg(i) != "")
				{
					config.ppp_serial = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "rx")
			{
				if (request->arg(i) != "")
				{
					config.ppp_rx_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "tx")
			{
				if (request->arg(i) != "")
				{
					config.ppp_tx_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "rst")
			{
				if (request->arg(i) != "")
				{
					config.ppp_rst_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "rst_active")
			{
				if (request->arg(i) != "")
				{
					config.ppp_rst_active = (bool)request->arg(i).toInt();
				}
			}

			// if (request->argName(i) == "pppSerial")
			// {
			// 	if (request->arg(i) != "")
			// 	{
			// 		if (isValidNumber(request->arg(i)))
			// 			config.ppp_serial = request->arg(i).toInt();
			// 	}
			// }
		}
		config.ppp_enable = pppEn;
		config.ppp_gnss = pppGnss;
		config.ppp_napt = pppNapt;
		if (config.ppp_enable)
		{
			if (config.ppp_serial == 0)
			{
				config.uart0_enable = false;
			}
			else if (config.ppp_serial == 1)
			{
				config.uart1_enable = false;
			}
			// else if (config.ppp_serial == 2)
			// {
			// 	config.uart2_enable = false;
			// }
		}
		char *html = allocateStringMemory(256); // Small buffer for success/failure message
		if (html != NULL)
		{
			if (saveConfiguration("/default.cfg", config))
			{
				strcpy(html, "Setup completed successfully");
				request->send(200, "text/html", html); // send to someones browser when asked
			}
			else
			{
				strcpy(html, "Save config failed.");
				request->send(501, "text/html", html); // Not Implemented
			}
			free(html);
		}
	}
#endif
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		JsonDocument data;
		int gpioMax = GPIO_NUM_MAX;
		data["gpioMax"] = gpioMax;

		data["uart0En"] = config.uart0_enable;
		data["uart0Rx"] = config.uart0_rx_gpio;
		data["uart0Tx"] = config.uart0_tx_gpio;
		data["uart0Rts"] = config.uart0_rts_gpio;
		data["uart1En"] = config.uart1_enable;
		data["uart1Rx"] = config.uart1_rx_gpio;
		data["uart1Tx"] = config.uart1_tx_gpio;
		data["uart1Rts"] = config.uart1_rts_gpio;
		data["rfBaudSel"] = config.rf_baudrate;

		JsonArray uart0Baud = data["uart0Baud"].to<JsonArray>();
		JsonArray uart1Baud = data["uart1Baud"].to<JsonArray>();
		JsonArray rfBaud = data["rfBaud"].to<JsonArray>();
		for (int i = 0; i < 13; i++)
		{
			JsonObject o0 = uart0Baud.add<JsonObject>();
			o0["value"] = baudrate[i];
			o0["selected"] = (config.uart0_baudrate == baudrate[i]);
			JsonObject o1 = uart1Baud.add<JsonObject>();
			o1["value"] = baudrate[i];
			o1["selected"] = (config.uart1_baudrate == baudrate[i]);
			JsonObject o2 = rfBaud.add<JsonObject>();
			o2["value"] = baudrate[i];
			o2["selected"] = (config.rf_baudrate == baudrate[i]);
		}

		JsonArray attenOptions = data["attenOptions"].to<JsonArray>();
		for (int i = 0; i < 5; i++)
		{
			JsonObject o = attenOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = ADC_ATTEN[i];
			o["selected"] = (config.adc_atten == i);
		}
		data["adcOffset"] = offset;

		data["rfRx"] = config.rf_rx_gpio;
		data["rfTx"] = config.rf_tx_gpio;
		data["rfPdGpio"] = config.rf_pd_gpio;
		data["rfPdHigh"] = config.rf_pd_active;
		data["rfPwrGpio"] = config.rf_pwr_gpio;
		data["rfPwrHigh"] = config.rf_pwr_active;
		data["rfSqlGpio"] = config.rf_sql_gpio;
		data["rfSqlHigh"] = config.rf_sql_active;
		data["rfPttGpio"] = config.rf_ptt_gpio;
		data["rfPttHigh"] = config.rf_ptt_active;

		data["tncEn"] = config.ext_tnc_enable;
		JsonArray tncPortOptions = data["tncPortOptions"].to<JsonArray>();
		JsonArray cmdUartOptions = data["cmdUartOptions"].to<JsonArray>();
		for (int i = 0; i < 5; i++)
		{
			JsonObject o = tncPortOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = TNC_PORT[i];
			o["selected"] = (config.ext_tnc_channel == i);
			JsonObject oc = cmdUartOptions.add<JsonObject>();
			oc["value"] = i;
			oc["label"] = TNC_PORT[i];
			oc["selected"] = (config.at_cmd_uart == i);
		}
		JsonArray tncModeOptions = data["tncModeOptions"].to<JsonArray>();
		for (int i = 0; i < 4; i++)
		{
			JsonObject o = tncModeOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = TNC_MODE[i];
			o["selected"] = (config.ext_tnc_mode == i);
		}

		data["tcpKissEn"] = config.tcp_kiss_enable;
		data["tcpKissPort1"] = config.tcp_kiss_port1;
		data["tcpKissPort2"] = config.tcp_kiss_port2;
		{
			String tcpKissStatus1 = "Port " + String(config.tcp_kiss_port1) + ": ";
			if (tcpKissClient1 && tcpKissClient1.connected())
				tcpKissStatus1 += "connected (" + tcpKissClient1.remoteIP().toString() + ") RX:" + String(tcpKissRxCount[0]) + " TX:" + String(tcpKissTxCount[0]);
			else
				tcpKissStatus1 += "idle";
			String tcpKissStatus2 = "Port " + String(config.tcp_kiss_port2) + ": ";
			if (tcpKissClient2 && tcpKissClient2.connected())
				tcpKissStatus2 += "connected (" + tcpKissClient2.remoteIP().toString() + ") RX:" + String(tcpKissRxCount[1]) + " TX:" + String(tcpKissTxCount[1]);
			else
				tcpKissStatus2 += "idle";
			data["tcpKissStatus1"] = tcpKissStatus1;
			data["tcpKissStatus2"] = tcpKissStatus2;
		}

		data["cmdMqtt"] = config.at_cmd_mqtt;
		data["cmdMsg"] = config.at_cmd_msg;
		data["cmdBluetooth"] = config.at_cmd_bluetooth;

#ifdef PPPOS
		data["pppos"] = true;
		data["pppEn"] = config.ppp_enable;
		data["pppGnss"] = config.ppp_gnss;
		data["pppNapt"] = config.ppp_napt;
		data["pppApn"] = config.ppp_apn;
		data["pppPin"] = atoi(config.ppp_pin);
		data["pppRx"] = config.ppp_rx_gpio;
		data["pppTx"] = config.ppp_tx_gpio;
		data["pppRstGpio"] = config.ppp_rst_gpio;
		data["pppRstHigh"] = config.ppp_rst_active;
		data["pppRstDelay"] = config.ppp_rst_delay;

		JsonArray pppPortOptions = data["pppPortOptions"].to<JsonArray>();
		for (int i = 0; i < 2; i++)
		{
			JsonObject o = pppPortOptions.add<JsonObject>();
			o["value"] = i;
			o["label"] = GNSS_PORT[i + 1];
			o["selected"] = (config.ppp_serial == i);
		}
		JsonArray pppBaud = data["pppBaud"].to<JsonArray>();
		for (int i = 0; i < 13; i++)
		{
			JsonObject o = pppBaud.add<JsonObject>();
			o["value"] = baudrate[i];
			o["selected"] = (config.ppp_serial_baudrate == baudrate[i]);
		}
#else
		data["pppos"] = false;
#endif

		String html_str = renderTemplate(MOD_TEMPLATE, data.as<JsonVariant>());
		// Streamed response: avoids a single large calloc() for the whole page
		// (same fix already applied to handle_igate/wx/sensor/etc after we
		// confirmed via live heap-integrity diagnostics that the single big
		// contiguous allocation was the reproducible trigger for a heap
		// corruption assert under real traffic).
		sendHtmlChunked(request, html_str, "MOD");
	}
}

static const char *MOD2_TEMPLATE = R"MOD2HTML(<script type="text/javascript">
$('form').submit(function (e) {
e.preventDefault();
var data = new FormData(e.currentTarget);
if(e.currentTarget.id==="formGNSS") document.getElementById("submitGNSS").disabled=true;
if(e.currentTarget.id==="formMODBUS") document.getElementById("submitMODBUS").disabled=true;
if(e.currentTarget.id==="formI2C0") document.getElementById("submitI2C0").disabled=true;
if(e.currentTarget.id==="formI2C1") document.getElementById("submitI2C1").disabled=true;
if(e.currentTarget.id==="formCOUNT0") document.getElementById("submitCOUNT0").disabled=true;
if(e.currentTarget.id==="formCOUNT1") document.getElementById("submitCOUNT1").disabled=true;
$.ajax({
url: '/mod2',
type: 'POST',
data: data,
contentType: false,
processData: false,
success: function (data) {
alert("Submited Successfully\nRequire hardware REBOOT!");
},
error: function (data) {
alert("An error occurred.");
}
});
});
</script>
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromONEWIRE" method="post">
<table>
<th colspan="2"><span><b>1-Wire Bus Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if oneWireEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="data" type="number" value="{{oneWireGpio}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitONEWIRE" name="commitONEWIRE" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitONEWIRE"/>
</td></tr></table>
</form><br />
</td></tr></table>
<br />
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromI2C0" method="post">
<table>
<th colspan="2"><span><b>I2C_0(OLED) Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if i2c0En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>SDA GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="sda" type="number" value="{{i2c0Sda}}" /></td>
</tr>
<tr>
<td align="right"><b>SCK GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="sck" type="number" value="{{i2c0Sck}}" /></td>
</tr>
<tr>
<td align="right"><b>Frequency:</b></td>
<td style="text-align: left;"><input min="1000" max="800000" name="freq" type="number" value="{{i2c0Freq}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitI2C0" name="commitI2C0" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitI2C0"/>
</td></tr></table>
</form>
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromCOUNTER0" method="post">
<table>
<th colspan="2"><span><b>Counter_0 Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if counter0En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>INPUT GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="gpio" type="number" value="{{counter0Gpio}}" /></td>
</tr>
<tr>
<td align="right">Active</td>
<td style="text-align: left;"><input type="radio" name="active" value="0" {{#if counter0High}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="active" value="1" {{#if counter0High}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitCOUNTER0" name="commitCOUNTER0" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitCOUNTER0"/>
</td></tr></table>
</form>
</td><td width="50%" style="border:unset;">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromI2C1" method="post">
<table>
<th colspan="2"><span><b>I2C_1 Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if i2c1En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>SDA GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="sda" type="number" value="{{i2c1Sda}}" /></td>
</tr>
<tr>
<td align="right"><b>SCK GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="sck" type="number" value="{{i2c1Sck}}" /></td>
</tr>
<tr>
<td align="right"><b>Frequency:</b></td>
<td style="text-align: left;"><input min="1000" max="800000" name="freq" type="number" value="{{i2c1Freq}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitI2C1" name="commitI2C1" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitI2C1"/>
</td></tr></table>
</form>
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromCOUNTER1" method="post">
<table>
<th colspan="2"><span><b>Counter_1 Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if counter1En}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>INPUT GPIO:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="gpio" type="number" value="{{counter1Gpio}}" /></td>
</tr>
<tr>
<td align="right">Active</td>
<td style="text-align: left;"><input type="radio" name="active" value="0" {{#if counter1High}}{{else}}checked="checked"{{/if}}/>LOW <input type="radio" name="active" value="1" {{#if counter1High}}checked="checked"{{/if}}/>HIGH </td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitCOUNTER1" name="commitCOUNTER1" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitCOUNTER1"/>
</td></tr></table>
</form>
</td></tr></table>
<br />
<table style="text-align:unset;border-width:0px;background:unset"><tr style="background:unset;vertical-align:top"><td width="50%" style="border:unset;vertical-align:top">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromGNSS" method="post">
<table>
<th colspan="2"><span><b>GNSS Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if gnssEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>PORT:</b></td>
<td style="text-align: left;">
<select name="channel" id="channel">
{{#each gnssPortOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<td align="right"><b>AT Command:</b></td>
<td style="text-align: left;"><input maxlength="30" size="20" id="atc" name="atc" type="text" value="{{gnssAtCommand}}" /></td>
</tr>
<td align="right"><b>TCP Host:</b></td>
<td style="text-align: left;"><input maxlength="20" size="15" id="Host" name="Host" type="text" value="{{gnssTcpHost}}" /></td>
</tr>
<tr>
<td align="right"><b>TCP Port:</b></td>
<td style="text-align: left;"><input min="1024" max="65535"  id="Port" name="Port" type="number" value="{{gnssTcpPort}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitGNSS" name="commitGNSS" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitGNSS"/>
</td></tr></table>
</form><br />
</td><td width="50%" style="border:unset;">
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="fromMODBUS" method="post">
<table>
<th colspan="2"><span><b>MODBUS Modify</b></span></th>
<tr>
<td align="right"><b>Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="Enable" value="OK" {{#if modbusEn}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td align="right"><b>PORT:</b></td>
<td style="text-align: left;">
<select name="channel" id="channel">
{{#each modbusPortOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} </option>
{{/each}}</select>
</td>
</tr>
<tr>
<td align="right"><b>Address:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="address" type="number" value="{{modbusAddress}}" /></td>
</tr>
<tr>
<td align="right"><b>DE:</b></td>
<td style="text-align: left;"><input min="-1" max="{{gpioMax}}" name="de" type="number" value="{{modbusDeGpio}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<input class="button" id="submitMODBUS" name="commitMODBUS" type="submit" value="Apply" maxlength="80"/>
<input type="hidden" name="commitMODBUS"/>
</td></tr></table>
</form>
</td></tr></table>
)MOD2HTML";

void handle_mod2(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitGNSS"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					// if (isValidNumber(request->arg(i)))
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "atc")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.gnss_at_command, request->arg(i).c_str());
				}
				else
				{
					memset(config.gnss_at_command, 0, sizeof(config.gnss_at_command));
				}
			}

			if (request->argName(i) == "Host")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.gnss_tcp_host, request->arg(i).c_str());
				}
			}

			if (request->argName(i) == "Port")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.gnss_tcp_port = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "channel")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.gnss_channel = request->arg(i).toInt();
				}
			}
		}

		config.gnss_enable = En;
		saveConfig(request);
	}
	else if (request->hasArg("commitMODBUS"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					// if (isValidNumber(request->arg(i)))
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "channel")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.modbus_channel = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "address")
			{
				if (isValidNumber(request->arg(i)))
				{
					config.modbus_address = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "de")
			{
				if (request->arg(i) != "")
				{
					config.modbus_de_gpio = request->arg(i).toInt();
				}
			}
		}

		config.modbus_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitONEWIRE"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					// if (isValidNumber(request->arg(i)))
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "data")
			{
				if (request->arg(i) != "")
				{
					config.onewire_gpio = request->arg(i).toInt();
				}
			}
		}

		config.onewire_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitI2C0"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "sda")
			{
				if (request->arg(i) != "")
				{
					config.i2c_sda_pin = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "sck")
			{
				if (request->arg(i) != "")
				{
					config.i2c_sck_pin = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "freq")
			{
				if (request->arg(i) != "")
				{
					config.i2c_freq = request->arg(i).toInt();
				}
			}
		}

		config.i2c_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitI2C1"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "sda")
			{
				if (request->arg(i) != "")
				{
					config.i2c1_sda_pin = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "sck")
			{
				if (request->arg(i) != "")
				{
					config.i2c1_sck_pin = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "freq")
			{
				if (request->arg(i) != "")
				{
					config.i2c1_freq = request->arg(i).toInt();
				}
			}
		}

		config.i2c1_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitCOUNTER0"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "gpio")
			{
				if (request->arg(i) != "")
				{
					config.counter0_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "active")
			{
				if (request->arg(i) != "")
				{
					config.counter0_active = (bool)request->arg(i).toInt();
				}
			}
		}

		config.counter0_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else if (request->hasArg("commitCOUNTER1"))
	{
		bool En = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			// Serial.print("SERVER ARGS ");
			// Serial.print(request->argName(i));
			// Serial.print("=");
			// Serial.println(request->arg(i));

			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}

			if (request->argName(i) == "gpio")
			{
				if (request->arg(i) != "")
				{
					config.counter1_gpio = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "active")
			{
				if (request->arg(i) != "")
				{
					config.counter1_active = (bool)request->arg(i).toInt();
				}
			}
		}

		config.counter0_enable = En;
		saveConfiguration("/default.cfg", config);
		String html = "OK";
		request->send(200, "text/html", html);
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_mod2 runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		JsonDocument data;
		int gpioMax = GPIO_NUM_MAX;
		data["gpioMax"] = gpioMax;

		data["oneWireEn"] = config.onewire_enable;
		data["oneWireGpio"] = config.onewire_gpio;

		data["i2c0En"] = config.i2c_enable;
		data["i2c0Sda"] = config.i2c_sda_pin;
		data["i2c0Sck"] = config.i2c_sck_pin;
		data["i2c0Freq"] = config.i2c_freq;

		data["counter0En"] = config.counter0_enable;
		data["counter0Gpio"] = config.counter0_gpio;
		data["counter0High"] = config.counter0_active;

		data["i2c1En"] = config.i2c1_enable;
		data["i2c1Sda"] = config.i2c1_sda_pin;
		data["i2c1Sck"] = config.i2c1_sck_pin;
		data["i2c1Freq"] = config.i2c1_freq;

		data["counter1En"] = config.counter1_enable;
		data["counter1Gpio"] = config.counter1_gpio;
		data["counter1High"] = config.counter1_active;

		data["gnssEn"] = config.gnss_enable;
		data["gnssAtCommand"] = config.gnss_at_command;
		data["gnssTcpHost"] = config.gnss_tcp_host;
		data["gnssTcpPort"] = config.gnss_tcp_port;

		data["modbusEn"] = config.modbus_enable;
		data["modbusAddress"] = config.modbus_address;
		data["modbusDeGpio"] = config.modbus_de_gpio;

		JsonArray gnssPortOptions = data["gnssPortOptions"].to<JsonArray>();
		JsonArray modbusPortOptions = data["modbusPortOptions"].to<JsonArray>();
		for (int i = 0; i < 5; i++)
		{
			JsonObject og = gnssPortOptions.add<JsonObject>();
			og["value"] = i;
			og["label"] = GNSS_PORT[i];
			og["selected"] = (config.gnss_channel == i);
			JsonObject om = modbusPortOptions.add<JsonObject>();
			om["value"] = i;
			om["label"] = GNSS_PORT[i];
			om["selected"] = (config.modbus_channel == i);
		}

		String html_str = renderTemplate(MOD2_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "MOD2");
	}
}

static const char *SYSTEM_TEMPLATE = R"SYSHTML(<script type="text/javascript">
$('form').submit(function (e) {
e.preventDefault();
var data = new FormData(e.currentTarget);
if(e.currentTarget.id==="formHostName") document.getElementById("updateHostName").disabled=true;
if(e.currentTarget.id==="formTime") document.getElementById("updateTime").disabled=true;
if(e.currentTarget.id==="formNTP") document.getElementById("updateTimeNtp").disabled=true;
if(e.currentTarget.id==="formTimeZone") document.getElementById("updateTimeZone").disabled=true;
if(e.currentTarget.id==="formReboot") document.getElementById("REBOOT").disabled=true;
if(e.currentTarget.id==="formDisp") document.getElementById("submitDISP").disabled=true;
if(e.currentTarget.id==="formWebAuth") document.getElementById("submitWebAuth").disabled=true;
if(e.currentTarget.id==="formPWR") document.getElementById("submitPWR").disabled=true;
if(e.currentTarget.id==="formPath") document.getElementById("submitPath").disabled=true;
$.ajax({
url: '/system',
type: 'POST',
data: data,
contentType: false,
processData: false,
success: function (data) {
alert("Submited Successfully");
},
error: function (data) {
alert("An error occurred.");
}
});
});
</script>
<table>
<th colspan="2"><span><b>System Setting</b></span></th>
<tr>
<td style="text-align: right;">Host Name:</td>
<td style="text-align: left;"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formHostName" method="post"><input name="SetHostName" type="text" value="{{hostName}}" />
<button type='submit' id='updateHostName'  name="updateHostName"> Apply </button>
<input type="hidden" name="updateHostName"/></form>
</td>
</tr>
<tr>
<td style="text-align: right;">LOCAL DATE/TIME </td>
<td style="text-align: left;"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formTime" method="post">
<input name="SetTime" type="text" value="{{setTime}}" />
<span class="input-group-addon">
<span class="glyphicon glyphicon-calendar">
</span></span>
<button type='submit' id='updateTime'  name="commit"> Time Update </button>
<input type="hidden" name="updateTime"/></form>
</td>
</tr>
<tr>
<td style="text-align: right;">NTP Host </td>
<td style="text-align: left;"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formNTP" method="post"><input name="SetTimeNtp" type="text" value="{{ntpHost}}" />
<button type='submit' id='updateTimeNtp'  name="commit"> NTP Update </button>
<input type="hidden" name="updateTimeNtp"/></form>
</td>
</tr>
<tr>
<td style="text-align: right;">Auto REBOOT:</td>
<td style="text-align: left;"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formAutoReset" method="post"><input  min="0" max="65535"  name="SetAutoReset" type="number" value="{{resetTimeout}}" /> Minutes
<button type='submit' id='updateAutoReset'  name="commit"> Update </button> *<i>0=No reset</i>
<input type="hidden" name="updateAutoReset"/></form>
</td>
</tr>
<tr>
<td style="text-align: right;">Time Zone </td>
<td style="text-align: left;"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formTimeZone" method="post">
<select name="SetTimeZone" id="SetTimeZone">
{{#each tzOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{label}} Sec</option>
{{/each}}</select><button type='submit' id='updateTimeZone'  name="commit"> TZ Update </button>
<input type="hidden" name="updateTimeZone"/></form>
</td>
</tr>
<tr>
<td style="text-align: right;">SYSTEM CONTROL </td>
<td style="text-align: left;"><table><tr><td width="100"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formReboot" method="post"> <button type='submit' id='REBOOT'  name="commit" style="background-color:red;color:white"> REBOOT </button>
 <input type="hidden" name="REBOOT"/></form></td><td width="100"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formFactory" method="post"> <button type='submit' id='Factory'  name="commit" style="background-color:orange;color:white"> Factory Reset </button>
 <input type="hidden" name="Factory"/></form></td><td width="100"><form accept-charset="UTF-8" action="#" enctype='multipart/form-data' id="formLoad" method="post"> <button type='submit' id='LoadCFG'  name="commit" style="background-color:green;color:white"> Load Default </button>
 <input type="hidden" name="LoadCFG"/></form></td></tr></table></td>
</tr></table><br /><br />
<form id='formWebAuth' method="POST" action='#' enctype='multipart/form-data'>
<table>
<th colspan="2"><span><b>Web Authentication</b></span></th>
<tr>
<td align="right"><b>Web USER:</b></td>
<td style="text-align: left;"><input size="32" maxlength="32" class="form-control" name="webauth_user" type="text" value="{{webUser}}" /></td>
</tr>
<tr>
<td align="right"><b>Web PASSWORD:</b></td>
<td style="text-align: left;"><input size="63" maxlength="63" class="form-control" name="webauth_pass" type="password" value="{{webPass}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitWebAuth'  name="commit"> Apply Change </button></div>
<input type="hidden" name="commitWebAuth"/>
</td></tr></table><br />
</form><br /><br />
<form accept-charset="UTF-8" action="#" class="form-horizontal" id="formPWR" method="post">
<table>
<th colspan="2"><span><b>Display StandBy</b></span></th>
<tr>
<td align="right"><b>StandBy Delay:</b></td>
<td style="text-align: left;"><input min="0" max="9999" name="stb" type="number" value="{{pwrStandbyDelay}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitPWR'  name="commitPWR"> Apply Change </button></div>
<input type="hidden" name="commitPWR"/>
</td></tr></table>
</form><br /><br />
{{#if logFile}}<form accept-charset="UTF-8" action="#" class="form-horizontal" id="formLOG" method="post">
<table>
<th colspan="2"><span><b>Log File</b></span></th>
<tr>
<td align="right"><b>Activate:</b></td>
<td style="text-align: left;">
<fieldset id="FilterGrp">
<legend>Events</legend>
<table style="text-align:unset;border-width:0px;background:unset">
<tr style="background:unset;"><td style="border:unset;"><input class="field_checkbox" name="logTracker" type="checkbox" value="OK" {{#if logTracker}}checked{{/if}}/>Tracker</td>
<td style="border:unset;"><input class="field_checkbox" name="logIgate" type="checkbox" value="OK" {{#if logIgate}}checked{{/if}}/>IGate</td>
<td style="border:unset;"><input class="field_checkbox" name="logDigi" type="checkbox" value="OK" {{#if logDigi}}checked{{/if}}/>DIGI</td>
<td style="border:unset;"><input class="field_checkbox" name="logWeather" type="checkbox" value="OK" {{#if logWeather}}checked{{/if}}/>Weather</td>
<td style="border:unset;"></td>
</tr></table></fieldset>
</td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitLOG'  name="commitLOG"> Apply Change </button></div>
<input type="hidden" name="commitLOG"/>
</td></tr></table>
</form><br /><br />
{{/if}}<form id='formPath' method="POST" action='#' enctype='multipart/form-data'>
<table>
<th colspan="2"><span><b>PATH USER Define</b></span></th>
<tr>
<td align="right"><b>PATH_1:</b></td>
<td style="text-align: left;"><input size="72" maxlength="72" class="form-control" name="path1" type="text" value="{{path1}}" /></td>
</tr>
<tr>
<td align="right"><b>PATH_2:</b></td>
<td style="text-align: left;"><input size="72" maxlength="72" class="form-control" name="path2" type="text" value="{{path2}}" /></td>
</tr>
<tr>
<td align="right"><b>PATH_3:</b></td>
<td style="text-align: left;"><input size="72" maxlength="72" class="form-control" name="path3" type="text" value="{{path3}}" /></td>
</tr>
<tr>
<td align="right"><b>PATH_4:</b></td>
<td style="text-align: left;"><input size="72" maxlength="72" class="form-control" name="path4" type="text" value="{{path4}}" /></td>
</tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitPath'  name="commitPath"> Apply Change </button></div>
<input type="hidden" name="commitPath"/>
</td></tr></table>
</form><br /><br />{{#if dispSupport}}<form id='formDisp' method="POST" action='#' enctype='multipart/form-data'>
<table>
<th colspan="2"><span><b>Display Setting</b></span></th>
<tr>
<td style="text-align: right;"><b>OLED/TFT Enable</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="oledEnable" value="OK" {{#if oledEnable}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td style="text-align: right;"><b>Flip Rotate</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="dispFlip" value="OK" {{#if dispFlip}}checked{{/if}}><span class="slider round"></span></label></td>
</tr>
<tr>
<td style="text-align: right;"><b>TX Display</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="txdispEnable" value="OK" {{#if txDisplay}}checked{{/if}}><span class="slider round"></span></label><label style="vertical-align: bottom;font-size: 8pt;"> <i>*All TX Packet for display affter filter.</i></label></td>
</tr>
<tr>
<td style="text-align: right;"><b>RX Display</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="rxdispEnable" value="OK" {{#if rxDisplay}}checked{{/if}}><span class="slider round"></span></label><label style="vertical-align: bottom;font-size: 8pt;"> <i>*All RX Packet for display affter filter.</i></label></td>
</tr>
<tr>
<td style="text-align: right;"><b>Head Up</b></td>
<td style="text-align: left;"><label class="switch"><input type="checkbox" name="hupEnable" value="OK" {{#if hUp}}checked{{/if}}><span class="slider round"></span></label><label style="vertical-align: bottom;font-size: 8pt;"> <i>*The compass will rotate in the direction of movement.</i></label></td>
</tr>
<tr>
<td style="text-align: right;"><b>TFT Brightness</b></td>
<td style="text-align: left;">
<select name="dispBright" id="dispBright">
{{#each brightOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}}</option>
{{/each}}</select>
</td></tr>
<tr>
<td style="text-align: right;"><b>Popup Delay</b></td>
<td style="text-align: left;">
<select name="dispDelay" id="dispDelay">
{{#each delayOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} Sec</option>
{{/each}}</select>
</td></tr>
<tr>
<td style="text-align: right;"><b>OLED/TFT Sleep</b></td>
<td style="text-align: left;">
<select name="oled_timeout" id="oled_timeout">
{{#each timeoutOptions}}<option value="{{value}}" {{#if selected}}selected{{/if}}>{{value}} Sec</option>
{{/each}}</select>
</td></tr>
<tr><td style="text-align: right;"><b>RX Channel</b></td><td style="text-align: left;"><input type="checkbox" name="dispRF" value="OK" {{#if dispRF}}checked{{/if}}/>RF <input type="checkbox" name="dispINET" value="OK" {{#if dispINET}}checked{{/if}}/>Internet </td></tr>
<tr>
<td align="right"><b>Filter DX:</b></td>
<td style="text-align: left;"><input type="number" name="filterDX" min="0" max="9999"
step="1" value="{{filterDistant}}" /> Km.  <label style="vertical-align: bottom;font-size: 8pt;"> <i>*Value 0 is all distant allow.</i></label></td>
</tr>
<tr>
<td align="right"><b>Filter:</b></td>
<td align="center">
<fieldset id="filterDispGrp">
<legend>Filter popup display</legend>
<table style="text-align:unset;border-width:0px;background:unset">
<tr style="background:unset;"><td style="border:unset;"><input class="field_checkbox" id="filterMessage" name="filterMessage" type="checkbox" value="OK" {{#if filterMessage}}checked{{/if}}/>Message</td>
<td style="border:unset;"><input class="field_checkbox" id="filterStatus" name="filterStatus" type="checkbox" value="OK" {{#if filterStatus}}checked{{/if}}/>Status</td>
<td style="border:unset;"><input class="field_checkbox" id="filterTelemetry" name="filterTelemetry" type="checkbox" value="OK" {{#if filterTelemetry}}checked{{/if}}/>Telemetry</td>
<td style="border:unset;"><input class="field_checkbox" id="filterWeather" name="filterWeather" type="checkbox" value="OK" {{#if filterWeather}}checked{{/if}}/>Weather</td>
<td style="border:unset;"><input class="field_checkbox" id="filterObject" name="filterObject" type="checkbox" value="OK" {{#if filterObject}}checked{{/if}}/>Object</td>
</tr><tr style="background:unset;"><td style="border:unset;"><input class="field_checkbox" id="filterItem" name="filterItem" type="checkbox" value="OK" {{#if filterItem}}checked{{/if}}/>Item</td>
<td style="border:unset;"><input class="field_checkbox" id="filterQuery" name="filterQuery" type="checkbox" value="OK" {{#if filterQuery}}checked{{/if}}/>Query</td>
<td style="border:unset;"><input class="field_checkbox" id="filterBuoy" name="filterBuoy" type="checkbox" value="OK" {{#if filterBuoy}}checked{{/if}}/>Buoy</td>
<td style="border:unset;"><input class="field_checkbox" id="filterPosition" name="filterPosition" type="checkbox" value="OK" {{#if filterPosition}}checked{{/if}}/>Position</td>
<td style="border:unset;"></td>
</tr></table></fieldset>
</td></tr>
<tr><td colspan="2" align="right">
<div><button class="button" type='submit' id='submitDISP'  name="commitDISP"> Apply Change </button></div>
<input type="hidden" name="commitDISP"/>
</td></tr></table><br />
</form><br />{{/if}})SYSHTML";

void handle_system(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("updateTimeZone"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "SetTimeZone")
			{
				if (request->arg(i) != "")
				{
					config.timeZone = request->arg(i).toFloat();
					// Serial.println("WEB Config Time Zone);
					configTime(3600 * config.timeZone, 0, config.ntp_host);
				}
				break;
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("updateHostName"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			log_d("%s", request->arg(i).c_str());
			if (request->argName(i) == "SetHostName")
			{
				if (request->arg(i) != "")
				{
					strncpy(config.host_name, request->arg(i).c_str(), sizeof(config.host_name) - 1);
					config.host_name[sizeof(config.host_name) - 1] = '\0'; // Null terminate
				}
				break;
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("updateTimeNtp"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "SetTimeNtp")
			{
				if (request->arg(i) != "")
				{
					// Serial.println("WEB Config NTP");
					strcpy(config.ntp_host, request->arg(i).c_str());
					configTime(3600 * config.timeZone, 0, config.ntp_host);
				}
				break;
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("updateAutoReset"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{

			if (request->argName(i) == "SetAutoReset")
			{
				if (request->arg(i) != "")
				{
					config.reset_timeout = request->arg(i).toInt();
				}
				break;
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("updateTime"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "SetTime")
			{
				if (request->arg(i) != "")
				{
					// struct tm tmn;
					String date = getValue(request->arg(i), ' ', 0);
					String time = getValue(request->arg(i), ' ', 1);
					int yyyy = getValue(date, '-', 0).toInt();
					int mm = getValue(date, '-', 1).toInt();
					int dd = getValue(date, '-', 2).toInt();
					int hh = getValue(time, ':', 0).toInt();
					int ii = getValue(time, ':', 1).toInt();
					int ss = getValue(time, ':', 2).toInt();
					// int ss = 0;

					tmElements_t timeinfo;
					timeinfo.Year = yyyy - 1970;
					timeinfo.Month = mm;
					timeinfo.Day = dd;
					timeinfo.Hour = hh;
					timeinfo.Minute = ii;
					timeinfo.Second = ss;
					time_t timeStamp = makeTime(timeinfo);

					// tmstruct.tm_year) + 1900, (tmstruct.tm_mon) + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec

					time_t rtc = timeStamp - (config.timeZone * 3600);
					timeval tv = {rtc, 0};
					timezone tz = {(0) + DST_MN, 0};
					settimeofday(&tv, &tz);

					// Serial.println("Update TIME " + request->arg(i));
					// Serial.print("Set New Time at ");
					// Serial.print(dd);
					// Serial.print("/");
					// Serial.print(mm);
					// Serial.print("/");
					// Serial.print(yyyy);
					// Serial.print(" ");
					// Serial.print(hh);
					// Serial.print(":");
					// Serial.print(ii);
					// Serial.print(":");
					// Serial.print(ss);
					// Serial.print(" ");
					// Serial.println(timeStamp);
				}
				break;
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("REBOOT"))
	{
		TLM_SEQ = 0;
		IGATE_TLM_SEQ = 0;
		DIGI_TLM_SEQ = 0;
		esp_restart();
	}
	else if (request->hasArg("Factory"))
	{
		defaultConfig();
	}
	else if (request->hasArg("LoadCFG"))
	{
		if (loadConfiguration("/default.cfg", config))
		{
			String html = "OK";
			request->send(200, "text/html", html);
		}
	}
	else if (request->hasArg("commitWebAuth"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "webauth_user")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.http_username, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "webauth_pass")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.http_password, request->arg(i).c_str());
				}
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("commitPath"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "path1")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.path[0], request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "path2")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.path[1], request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "path3")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.path[2], request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "path4")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.path[3], request->arg(i).c_str());
				}
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("commitPWR"))
	{
		// LU6JMF cleanup (Set/2026): this form used to have a whole dead
		// "Power Save Mode" (Enable/GPIO/Sleep Interval/Mode/Events) that
		// never actually did anything in the firmware. Only StandBy Delay
		// (OLED timeout) was real - kept, rest removed to save space.
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "stb")
			{
				if (request->arg(i) != "")
				{
					config.pwr_stanby_delay = request->arg(i).toInt();
				}
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("commitLOG"))
	{
		bool PwrEn = false;
		config.log = 0;

		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "logStatus")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.log |= LOG_STATUS;
				}
			}

			if (request->argName(i) == "logWeather")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.log |= LOG_WX;
				}
			}

			if (request->argName(i) == "logTracker")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.log |= LOG_TRACKER;
				}
			}

			if (request->argName(i) == "logIgate")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.log |= LOG_IGATE;
				}
			}

			if (request->argName(i) == "logDigi")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.log |= LOG_DIGI;
				}
			}
		}
		saveConfig(request);
	}
	else if (request->hasArg("commitDISP"))
	{
		bool dispRX = false;
		bool dispTX = false;
		bool dispRF = false;
		bool dispINET = false;
		bool oledEN = false;
		bool dispFlip = false;

		config.dispFilter = 0;

		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "oledEnable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						oledEN = true;
					}
				}
			}
			if (request->argName(i) == "dispFlip")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
					{
						dispFlip = true;
					}
				}
			}
			if (request->argName(i) == "filterMessage")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_MESSAGE;
				}
			}

			if (request->argName(i) == "filterTelemetry")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_TELEMETRY;
				}
			}

			if (request->argName(i) == "filterStatus")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_STATUS;
				}
			}

			if (request->argName(i) == "filterWeather")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_WX;
				}
			}

			if (request->argName(i) == "filterObject")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_OBJECT;
				}
			}

			if (request->argName(i) == "filterItem")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_ITEM;
				}
			}

			if (request->argName(i) == "filterQuery")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_QUERY;
				}
			}
			if (request->argName(i) == "filterBuoy")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_BUOY;
				}
			}
			if (request->argName(i) == "filterPosition")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.dispFilter |= FILTER_POSITION;
				}
			}

			if (request->argName(i) == "dispRF")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						dispRF = true;
				}
			}

			if (request->argName(i) == "dispINET")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						dispINET = true;
				}
			}
			if (request->argName(i) == "txdispEnable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						dispTX = true;
				}
			}
			if (request->argName(i) == "rxdispEnable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						dispRX = true;
				}
			}

			if (request->argName(i) == "dispBright")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.disp_brightness = request->arg(i).toInt();
#ifdef ST7735_LED_K_Pin
						ledcWrite(0, (uint32_t)config.disp_brightness);
#endif
					}
				}
			}

			if (request->argName(i) == "dispDelay")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.dispDelay = request->arg(i).toInt();
						if (config.dispDelay < 0)
							config.dispDelay = 0;
					}
				}
			}

			if (request->argName(i) == "oled_timeout")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.oled_timeout = request->arg(i).toInt();
						if (config.oled_timeout < 0)
							config.oled_timeout = 0;
					}
				}
			}
			if (request->argName(i) == "filterDX")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.filterDistant = request->arg(i).toInt();
					}
				}
			}
		}

#if defined OLED || defined GUI_LCD
		if (oledEN && !config.oled_enable)
		{
			// display.begin(SSD1306_SWITCHCAPVCC, 0x3C, false); // initialize with the I2C addr 0x3C (for the 128x64)
			//  Initialising the UI will init the display too.
			// #ifdef SH1106
			// 			display.begin(SH1106_SWITCHCAPVCC, SCREEN_ADDRESS, false);
			// #else
			// 			display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS, false, false);
			// #endif
			//			display.clearDisplay();
		}
		config.oled_enable = oledEN;
		config.dispINET = dispINET;
		config.dispRF = dispRF;
		config.rx_display = dispRX;
		config.tx_display = dispTX;
		config.disp_flip = dispFlip;
#endif // OLED
	   // config.filterMessage = filterMessage;
	   // config.filterStatus = filterStatus;
	   // config.filterTelemetry = filterTelemetry;
	   // config.filterWeather = filterWeather;
	   // config.filterTracker = filterTracker;
	   // config.filterMove = filterMove;
	   // config.filterPosition = filterPosition;
		saveConfig(request);
	}
	else
	{
		struct tm tmstruct;
		char strTime[30];
		tmstruct.tm_year = 0;
		getLocalTime(&tmstruct, 100);
		sprintf(strTime, "%d-%02d-%02d %02d:%02d:%02d", (tmstruct.tm_year) + 1900, (tmstruct.tm_mon) + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);

		JsonDocument data;
		data["hostName"] = config.host_name;
		data["setTime"] = strTime;
		data["ntpHost"] = config.ntp_host;
		data["resetTimeout"] = config.reset_timeout;

		JsonArray tzOptions = data["tzOptions"].to<JsonArray>();
		char tzValueBuf[8];
		for (int i = 0; i < 40; i++)
		{
			JsonObject o = tzOptions.add<JsonObject>();
			snprintf(tzValueBuf, sizeof(tzValueBuf), "%.1f", tzList[i].tz);
			o["value"] = tzValueBuf;
			o["label"] = tzList[i].name;
			o["selected"] = (config.timeZone == tzList[i].tz);
		}

		data["webUser"] = config.http_username;
		data["webPass"] = config.http_password;
		data["pwrStandbyDelay"] = config.pwr_stanby_delay;

#ifdef LOG_FILE
		data["logFile"] = true;
		data["logTracker"] = (bool)(config.log & LOG_TRACKER);
		data["logIgate"] = (bool)(config.log & LOG_IGATE);
		data["logDigi"] = (bool)(config.log & LOG_DIGI);
		data["logWeather"] = (bool)(config.log & LOG_WX);
#else
		data["logFile"] = false;
#endif

		data["path1"] = config.path[0];
		data["path2"] = config.path[1];
		data["path3"] = config.path[2];
		data["path4"] = config.path[3];

#if defined OLED || defined ST7735_160x80 || defined GUI_LCD
		data["dispSupport"] = true;
		data["oledEnable"] = config.oled_enable;
		data["dispFlip"] = config.disp_flip;
		data["txDisplay"] = config.tx_display;
		data["rxDisplay"] = config.rx_display;
		data["hUp"] = config.h_up;

		JsonArray brightOptions = data["brightOptions"].to<JsonArray>();
		for (int i = 0; i < 255; i += 25)
		{
			JsonObject o = brightOptions.add<JsonObject>();
			o["value"] = i;
			o["selected"] = (config.disp_brightness == i);
		}
		JsonArray delayOptions = data["delayOptions"].to<JsonArray>();
		for (int i = 0; i < 16; i += 1)
		{
			JsonObject o = delayOptions.add<JsonObject>();
			o["value"] = i;
			o["selected"] = (config.dispDelay == i);
		}
		JsonArray timeoutOptions = data["timeoutOptions"].to<JsonArray>();
		for (int i = 0; i <= 600; i += 30)
		{
			JsonObject o = timeoutOptions.add<JsonObject>();
			o["value"] = i;
			o["selected"] = (config.oled_timeout == i);
		}

		data["dispRF"] = config.dispRF;
		data["dispINET"] = config.dispINET;
		data["filterDistant"] = config.filterDistant;

		data["filterMessage"] = (bool)(config.dispFilter & FILTER_MESSAGE);
		data["filterStatus"] = (bool)(config.dispFilter & FILTER_STATUS);
		data["filterTelemetry"] = (bool)(config.dispFilter & FILTER_TELEMETRY);
		data["filterWeather"] = (bool)(config.dispFilter & FILTER_WX);
		data["filterObject"] = (bool)(config.dispFilter & FILTER_OBJECT);
		data["filterItem"] = (bool)(config.dispFilter & FILTER_ITEM);
		data["filterQuery"] = (bool)(config.dispFilter & FILTER_QUERY);
		data["filterBuoy"] = (bool)(config.dispFilter & FILTER_BUOY);
		data["filterPosition"] = (bool)(config.dispFilter & FILTER_POSITION);
#else
		data["dispSupport"] = false;
#endif

		String html_str = renderTemplate(SYSTEM_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "System");
	}
}

static const char *IGATE_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formIgate\") document.getElementById(\"submitIGATE\").disabled=true;\n"
	"if(e.currentTarget.id===\"formIgateFilter\") document.getElementById(\"submitIGATEfilter\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/igate',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n<script type=\"text/javascript\">\n"
	"function openWindowSymbol() {\n"
	"var i, l, options = [{\n"
	"value: 'first',\n"
	"text: 'First'\n"
	"}, {\n"
	"value: 'second',\n"
	"text: 'Second'\n"
	"}],\n"
	"newWindow = window.open(\"/symbol\", null, \"height=400,width=400,status=no,toolbar=no,menubar=no,location=no\");\n"
	"}\n"
	"function setValue(symbol,table) {\n"
	"document.getElementById('igateSymbol').value = String.fromCharCode(symbol);\n"
	"if(table==1){\n document.getElementById('igateTable').value='/';\n"
	"}else if(table==2){\n document.getElementById('igateTable').value='\\\\';\n}\n"
	"document.getElementById('igateImgSymbol').src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n"
	"\n}\n"
	"function calculatePHGR(){document.forms.formIgate.texttouse.value=\"PHG\"+calcPower(document.forms.formIgate.power.value)+calcHeight(document.forms.formIgate.haat.value)+calcGain(document.forms.formIgate.gain.value)+calcDirection(document.forms.formIgate.direction.selectedIndex)}function Log2(e){return Math.log(e)/Math.log(2)}function calcPerHour(e){return e<10?e:String.fromCharCode(65+(e-10))}function calcHeight(e){return String.fromCharCode(48+Math.round(Log2(e/10),0))}function calcPower(e){if(e<1)return 0;if(e>=1&&e<4)return 1;if(e>=4&&e<9)return 2;if(e>=9&&e<16)return 3;if(e>=16&&e<25)return 4;if(e>=25&&e<36)return 5;if(e>=36&&e<49)return 6;if(e>=49&&e<64)return 7;if(e>=64&&e<81)return 8;if(e>=81)return 9}function calcDirection(e){if(e==\"0\")return\"0\";if(e==\"1\")return\"1\";if(e==\"2\")return\"2\";if(e==\"3\")return\"3\";if(e==\"4\")return\"4\";if(e==\"5\")return\"5\";if(e==\"6\")return\"6\";if(e==\"7\")return\"7\";if(e==\"8\")return\"8\"}function calcGain(e){return e>9?\"9\":e<0?\"0\":Math.round(e,0)}\n"
	"function onRF2INETCheck() {\n"
	"if (document.querySelector('#rf2inetEnable').checked) {\n"
	"document.getElementById(\"rf2inetFilterGrp\").disabled=false;\n"
	"} else {\n"
	"document.getElementById(\"rf2inetFilterGrp\").disabled=true;\n"
	"}\n}\n"
	"function onINET2RFCheck() {\n"
	"if (document.querySelector('#inet2rfEnable').checked) {\n"
	"document.getElementById(\"inet2rfFilterGrp\").disabled=false;\n"
	"} else {\n"
	"document.getElementById(\"inet2rfFilterGrp\").disabled=true;\n"
	"}\n}\n"
	"function selPrecision(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"precision\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n"
	"}\n"
	"function selOffset(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"offset\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n"
	"}\n"
	"</script>\n"
	"<form id='formIgate' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[IGATE] Internet Gateway Mode</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateEnable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"{{myCall}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station SSID:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"mySSID\" id=\"mySSID\">\n"
	"{{#each ssidOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{value}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Symbol:</b></td>\n"
	"<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"igateTable\" name=\"igateTable\" type=\"text\" value=\"{{symTable}}\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"igateSymbol\" name=\"igateSymbol\" type=\"text\" value=\"{{symSymbol}}\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"igateImgSymbol\" onclick=\"openWindowSymbol();\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{symImgNum}}-{{symImgTable}}.png\"> <i>*Click icon for select symbol</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Item/Obj Name:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" id=\"igateObject\" name=\"igateObject\" type=\"text\" value=\"{{object}}\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"igatePath\" id=\"igatePath\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Server Host:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"20\" size=\"20\" id=\"aprsHost\" name=\"aprsHost\" type=\"text\" value=\"{{aprsHost}}\" /> *APRS-IS by T2THAI at <a href=\"http://aprs.nakhonthai.net:14501\" target=\"_t2thai\">aprs.nakhonthai.net:14580</a>,CBAPRS at <a href=\"http://aprs.nakhonthai.net:24501\" target=\"_t2thai\">aprs.nakhonthai.net:24580</a></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Server Port:</b></td>\n"
	"<td style=\"text-align: left;\"><input min=\"1\" max=\"65535\" step=\"1\" id=\"aprsPort\" name=\"aprsPort\" type=\"number\" value=\"{{aprsPort}}\" /> *AMPR Host at <a href=\"http://aprs.hs5tqa.ampr.org:14501\" target=\"_t2thai\">aprs.hs5tqa.ampr.org:14580</a></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Server Filter:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"30\" size=\"30\" id=\"aprsFilter\" name=\"aprsFilter\" type=\"text\" value=\"{{aprsFilter}}\" /> *Filter: <a target=\"_blank\" href=\"http://www.aprs-is.net/javAPRSFilter.aspx\">http://www.aprs-is.net/javAPRSFilter.aspx</a></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Comment:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"igateComment\" name=\"igateComment\" type=\"text\" value=\"{{comment}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Status:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"igateStatus\" name=\"igateStatus\" type=\"text\" value=\"{{status}}\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"igateSTSInv\" type=\"number\" value=\"{{statusInterval}}\" />Sec.</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>RF2INET:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"rf2inetEnable\" name=\"rf2inetEnable\" onclick=\"onRF2INETCheck()\" value=\"OK\" {{#if rf2inet}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch RF to Internet gateway</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>INET2RF:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"inet2rfEnable\" name=\"inet2rfEnable\" onclick=\"onINET2RFCheck()\" value=\"OK\" {{#if inet2rf}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch Internet to RF gateway</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Time Stamp:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateTimeStamp\" value=\"OK\" {{#if timestamp}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n<tr>"
	"<td align=\"right\"><b>POSITION:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Beacon:</td><td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateBcnEnable\" value=\"OK\" {{#if bcnEn}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\">  Interval:<input min=\"0\" max=\"3600\" step=\"1\" id=\"igatePosInv\" name=\"igatePosInv\" type=\"number\" value=\"{{posInterval}}\" />Sec.</label></td></tr>"
	"<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"igatePosSel\" value=\"0\" {{#if posFix}}checked=\"checked\"{{/if}}/>Fix <input type=\"radio\" name=\"igatePosSel\" value=\"1\" {{#if posGPS}}checked=\"checked\"{{/if}}/>GPS </td></tr>\n"
	"<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"igatePos2RF\" value=\"OK\" {{#if pos2RF}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"igatePos2INET\" value=\"OK\" {{#if pos2INET}}checked{{/if}}/>Internet </td></tr>\n"
	"<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"igatePosLat\" name=\"igatePosLat\" type=\"number\" value=\"{{lat}}\" />degrees (positive for North, negative for South)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"igatePosLon\" name=\"igatePosLon\" type=\"number\" value=\"{{lon}}\" />degrees (positive for East, negative for West)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"igatePosAlt\" name=\"igatePosAlt\" type=\"number\" value=\"{{alt}}\" /> meter. *Value 0 is not send height</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PHG:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr>\n"
	"<td align=\"right\">Radio TX Power</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"power\" id=\"power\">\n"
	"<option value=\"1\" selected>1</option>\n"
	"<option value=\"5\">5</option>\n"
	"<option value=\"10\">10</option>\n"
	"<option value=\"15\">15</option>\n"
	"<option value=\"25\">25</option>\n"
	"<option value=\"35\">35</option>\n"
	"<option value=\"50\">50</option>\n"
	"<option value=\"65\">65</option>\n"
	"<option value=\"80\">80</option>\n"
	"</select> Watts</td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">Antenna Gain</td><td style=\"text-align: left;\"><input size=\"3\" min=\"0\" max=\"100\" step=\"0.1\" id=\"gain\" name=\"gain\" type=\"number\" value=\"6\" /> dBi</td></tr>\n"
	"<tr>\n"
	"<td align=\"right\">Height</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"haat\" id=\"haat\">\n"
	"<option value=\"10\" selected>10</option>\n"
	"<option value=\"20\">20</option>\n"
	"<option value=\"40\">40</option>\n"
	"<option value=\"80\">80</option>\n"
	"<option value=\"160\">160</option>\n"
	"<option value=\"320\">320</option>\n"
	"<option value=\"640\">640</option>\n"
	"<option value=\"1280\">1280</option>\n"
	"<option value=\"2560\">2560</option>\n"
	"<option value=\"5120\">5120</option>\n"
	"</select> Feet</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\">Antenna/Direction</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"direction\" id=\"direction\">\n"
	"<option>Omni</option><option>NE</option><option>E</option><option>SE</option><option>S</option><option>SW</option><option>W</option><option>NW</option><option>N</option>\n"
	"</select></td>\n"
	"</tr>\n"
	"<tr><td align=\"right\"><b>PHG Text</b></td><td align=\"left\"><input name=\"texttouse\" type=\"text\" size=\"6\" style=\"background-color: rgb(97, 239, 170);\" value=\"{{phg}}\"/> <input type=\"button\" value=\"Calculate PHG\" onclick=\"javascript:calculatePHGR()\" /></td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitIGATE'  name=\"commitIGATE\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitIGATE\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />\n"
	"<form id='formIgateFilter' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[IGATE] Filter</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>RF2INET Filter:</b></td>\n"
	"<td align=\"center\">\n"
	"<fieldset id=\"rf2inetFilterGrp\" {{#if rf2inetGrpDisabled}}disabled{{/if}}>\n"
	"<legend>Filter RF to Internet</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterMessage\" type=\"checkbox\" value=\"OK\" {{#if rf2inetMsg}}checked{{/if}}/>Message</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterStatus\" type=\"checkbox\" value=\"OK\" {{#if rf2inetStatus}}checked{{/if}}/>Status</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterTelemetry\" type=\"checkbox\" value=\"OK\" {{#if rf2inetTlm}}checked{{/if}}/>Telemetry</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterWeather\" type=\"checkbox\" value=\"OK\" {{#if rf2inetWx}}checked{{/if}}/>Weather</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterObject\" type=\"checkbox\" value=\"OK\" {{#if rf2inetObj}}checked{{/if}}/>Object</td>\n"
	"</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterItem\" type=\"checkbox\" value=\"OK\" {{#if rf2inetItem}}checked{{/if}}/>Item</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterQuery\" type=\"checkbox\" value=\"OK\" {{#if rf2inetQuery}}checked{{/if}}/>Query</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterBuoy\" type=\"checkbox\" value=\"OK\" {{#if rf2inetBuoy}}checked{{/if}}/>Buoy</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterPosition\" type=\"checkbox\" value=\"OK\" {{#if rf2inetPos}}checked{{/if}}/>Position</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterAll\" type=\"checkbox\" value=\"OK\" {{#if rf2inetAll}}checked{{/if}}/>ALL</td>\n"
	"<td style=\"border:unset;\"></td>"
	"</tr></table></fieldset>\n"
	"</td></tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>INET2RF Filter:</b></td>\n"
	"<td align=\"center\">\n"
	"<fieldset id=\"inet2rfFilterGrp\" {{#if inet2rfGrpDisabled}}disabled{{/if}}>\n"
	"<legend>Filter Internet to RF</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterMessage\" type=\"checkbox\" value=\"OK\" {{#if inet2rfMsg}}checked{{/if}}/>Message</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterStatus\" type=\"checkbox\" value=\"OK\" {{#if inet2rfStatus}}checked{{/if}}/>Status</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterTelemetry\" type=\"checkbox\" value=\"OK\" {{#if inet2rfTlm}}checked{{/if}}/>Telemetry</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterWeather\" type=\"checkbox\" value=\"OK\" {{#if inet2rfWx}}checked{{/if}}/>Weather</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterObject\" type=\"checkbox\" value=\"OK\" {{#if inet2rfObj}}checked{{/if}}/>Object</td>\n"
	"</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterItem\" type=\"checkbox\" value=\"OK\" {{#if inet2rfItem}}checked{{/if}}/>Item</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterQuery\" type=\"checkbox\" value=\"OK\" {{#if inet2rfQuery}}checked{{/if}}/>Query</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterBuoy\" type=\"checkbox\" value=\"OK\" {{#if inet2rfBuoy}}checked{{/if}}/>Buoy</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterPosition\" type=\"checkbox\" value=\"OK\" {{#if inet2rfPos}}checked{{/if}}/>Position</td>\n"
	"<td style=\"border:unset;\"></td>"
	"</tr></table></fieldset>\n"
	"</td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitIGATEfilter'  name=\"commitIGATEfilter\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitIGATEfilter\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />\n"
	"<div style=\"margin:8px 0;padding:6px;background:#fff3cd;border:1px solid #ffe69c;font-size:9pt;\">Telemetry channels moved to their own page - see the <b>IGTLM</b> tab.</div>\n";

// LU6JMF - Marcelo Ferreira (Set/2026): Telemetry split out of the IGATE page
// into its own page/tab (RF2INET/INET2RF Filter moved back into IGATE_TEMPLATE
// per user request - only Telemetry, which has the heavier 5-channel loop,
// stays isolated here). Same commitIGATETlm POST contract as before.
static const char *IGATE2_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formIgateTlm\") document.getElementById(\"submitIGATETlm\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/igate2',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n<script type=\"text/javascript\">\n"
	"function selPrecision(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"precision\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n"
	"}\n"
	"function selOffset(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"offset\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n"
	"}\n"
	"</script>\n"
	"<form id='formIgateTlm' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[IGATE] Telemetry</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n"
	"<td align=\"center\"><table>\n"
	"<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"igateTlmInv\" name=\"igateTlmInv\" type=\"number\" value=\"{{tlmInterval}}\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>"
	"{{#each tlmChannels}}<tr><td align=\"right\"><b>CH A{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Sensor:</td>\n"
	"<td style=\"text-align: left;\">CH: "
	"<select name=\"sensorCH{{idx}}\" id=\"sensorCH{{idx}}\">\n"
	"{{#each sensorOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param{{idx}}\" type=\"text\" value=\"{{param}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit{{idx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision{{idx}}\" type=\"text\" value=\"{{precision}}\" onchange=\"selPrecision({{idx}})\"/></td></tr>\n"
	"<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}a\" type=\"number\" value=\"{{eqnsA}}\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}b\" type=\"number\" value=\"{{eqnsB}}\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}c\" type=\"number\" value=\"{{eqnsC}}\" /> (av<sup>2</sup>+bv+c) </td>\n"
	"<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset{{idx}}\" type=\"text\" value=\"{{offset}}\" onchange=\"selOffset({{idx}})\"/></td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"</table></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitIGATETlm'  name=\"commitIGATETlm\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitIGATETlm\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />";

void handle_igate(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	bool aprsEn = false;
	bool rf2inetEn = false;
	bool inet2rfEn = false;
	bool posGPS = false;
	bool bcnEN = false;
	bool pos2RF = false;
	bool pos2INET = false;
	bool timeStamp = false;

	if (request->hasArg("commitIGATE"))
	{

		for (int i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "igateEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						aprsEn = true;
				}
			}
			if (request->argName(i) == "myCall")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					name.toUpperCase();
					strcpy(config.aprs_mycall, name.c_str());
				}
			}
			if (request->argName(i) == "igateObject")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					strcpy(config.igate_object, name.c_str());
				}
				else
				{
					memset(config.igate_object, 0, sizeof(config.igate_object));
				}
			}
			if (request->argName(i) == "mySSID")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.aprs_ssid = request->arg(i).toInt();
					if (config.aprs_ssid > 15)
						config.aprs_ssid = 13;
				}
			}
			if (request->argName(i) == "igatePosInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "igateSTSInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_sts_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "igatePosLat")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_lat = request->arg(i).toFloat();
				}
			}

			if (request->argName(i) == "igatePosLon")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_lon = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "igatePosAlt")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_alt = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "igatePosSel")
			{
				if (request->arg(i) != "")
				{
					if (request->arg(i).toInt() == 1)
						posGPS = true;
				}
			}

			if (request->argName(i) == "igateTable")
			{
				if (request->arg(i) != "")
				{
					config.igate_symbol[0] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "igateSymbol")
			{
				if (request->arg(i) != "")
				{
					config.igate_symbol[1] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "aprsHost")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.aprs_host, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "aprsPort")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.aprs_port = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "aprsFilter")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.aprs_filter, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "igatePath")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "igateComment")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.igate_comment, request->arg(i).c_str());
				}
				else
				{
					memset(config.igate_comment, 0, sizeof(config.igate_comment));
				}
			}
			if (request->argName(i) == "igateStatus")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.igate_status, request->arg(i).c_str());
				}
				else
				{
					memset(config.igate_comment, 0, sizeof(config.igate_comment));
				}
			}
			if (request->argName(i) == "texttouse")
			{
				if (request->arg(i) != "")
				{
					strncpy(config.igate_phg, request->arg(i).c_str(), sizeof(config.igate_phg) - 1);
config.igate_phg[sizeof(config.igate_phg) - 1] = 0;
				}
			}

			if (request->argName(i) == "rf2inetEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						rf2inetEn = true;
				}
			}
			if (request->argName(i) == "inet2rfEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						inet2rfEn = true;
				}
			}
			if (request->argName(i) == "igatePos2RF")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2RF = true;
				}
			}
			if (request->argName(i) == "igatePos2INET")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2INET = true;
				}
			}
			if (request->argName(i) == "igateBcnEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						bcnEN = true;
				}
			}
			if (request->argName(i) == "igateTimeStamp")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						timeStamp = true;
				}
			}
		}

		// waitISRetry = millis() + 10000; // Retry connect 5Sec
		config.igate_en = aprsEn;
		config.rf2inet = rf2inetEn;
		config.inet2rf = inet2rfEn;
		config.igate_gps = posGPS;
		config.igate_bcn = bcnEN;
		config.igate_loc2rf = pos2RF;
		config.igate_loc2inet = pos2INET;
		config.igate_timestamp = timeStamp;

		initInterval = true;
		String html;
		if (saveConfiguration("/default.cfg", config))
		{
			html = "Setup completed successfully";
			request->send(200, "text/html", html); // send to someones browser when asked
		}
		else
		{
			html = "Save config failed.";
			request->send(501, "text/html", html); // Not Implemented
		}
		aprsClient.stop();
	}
	else if (request->hasArg("commitIGATEfilter"))
	{
		config.rf2inetFilter = 0;
		config.inet2rfFilter = 0;
		for (int i = 0; i < request->args(); i++)
		{
			// config rf2inet filter
			if (request->argName(i) == "rf2inetFilterMessage")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_MESSAGE;
				}
			}

			if (request->argName(i) == "rf2inetFilterTelemetry")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_TELEMETRY;
				}
			}

			if (request->argName(i) == "rf2inetFilterStatus")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_STATUS;
				}
			}

			if (request->argName(i) == "rf2inetFilterWeather")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_WX;
				}
			}

			if (request->argName(i) == "rf2inetFilterObject")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_OBJECT;
				}
			}

			if (request->argName(i) == "rf2inetFilterItem")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_ITEM;
				}
			}

			if (request->argName(i) == "rf2inetFilterQuery")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_QUERY;
				}
			}
			if (request->argName(i) == "rf2inetFilterBuoy")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_BUOY;
				}
			}
			if (request->argName(i) == "rf2inetFilterPosition")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_POSITION;
				}
			}
			if (request->argName(i) == "rf2inetFilterAll")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.rf2inetFilter |= FILTER_ENABLE_ALL;
				}
			}
			// config inet2rf filter

			if (request->argName(i) == "inet2rfFilterMessage")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_MESSAGE;
				}
			}

			if (request->argName(i) == "inet2rfFilterTelemetry")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_TELEMETRY;
				}
			}

			if (request->argName(i) == "inet2rfFilterStatus")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_STATUS;
				}
			}

			if (request->argName(i) == "inet2rfFilterWeather")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_WX;
				}
			}

			if (request->argName(i) == "inet2rfFilterObject")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_OBJECT;
				}
			}

			if (request->argName(i) == "inet2rfFilterItem")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_ITEM;
				}
			}

			if (request->argName(i) == "inet2rfFilterQuery")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_QUERY;
				}
			}
			if (request->argName(i) == "inet2rfFilterBuoy")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_BUOY;
				}
			}
			if (request->argName(i) == "inet2rfFilterPosition")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						config.inet2rfFilter |= FILTER_POSITION;
				}
			}
		}
		String html;
		if (saveConfiguration("/default.cfg", config))
		{
			html = "Setup completed successfully";
			request->send(200, "text/html", html); // send to someones browser when asked
		}
		else
		{
			html = "Save config failed.";
			request->send(501, "text/html", html); // Not Implemented
		}
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_igate runs on the async_tcp task itself,
		// this is still a large RF page even without Telemetry (moved to handle_igate2) -
		// can take long enough under heap pressure to trip the TWDT.
		JsonDocument data;
		data["enabled"] = config.igate_en;
		data["myCall"] = config.aprs_mycall;

		JsonArray ssidOptions = data["ssidOptions"].to<JsonArray>();
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			JsonObject o = ssidOptions.add<JsonObject>();
			o["value"] = ssid;
			o["selected"] = (config.aprs_ssid == ssid);
		}

		auto tableFor = [](char first) -> const char *
		{
			if (first == 92)
				return "2";
			return "1";
		};
		char cbuf[2];
		cbuf[0] = config.igate_symbol[0];
		cbuf[1] = 0;
		data["symTable"] = cbuf;
		cbuf[0] = config.igate_symbol[1];
		cbuf[1] = 0;
		data["symSymbol"] = cbuf;
		data["symImgNum"] = (int)config.igate_symbol[1];
		data["symImgTable"] = tableFor(config.igate_symbol[0]);

		data["object"] = config.igate_object;

		JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			JsonObject o = pathOptions.add<JsonObject>();
			o["value"] = pthIdx;
			o["label"] = PATH_NAME[pthIdx];
			o["selected"] = (config.igate_path == pthIdx);
		}

		data["aprsHost"] = config.aprs_host;
		data["aprsPort"] = config.aprs_port;
		data["aprsFilter"] = config.aprs_filter;
		data["comment"] = config.igate_comment;
		data["status"] = config.igate_status;
		data["statusInterval"] = config.igate_sts_interval;
		data["rf2inet"] = config.rf2inet;
		data["inet2rf"] = config.inet2rf;
		data["timestamp"] = config.igate_timestamp;
		data["bcnEn"] = config.igate_bcn;
		data["posInterval"] = config.igate_interval;
		data["posFix"] = !config.igate_gps;
		data["posGPS"] = config.igate_gps;
		data["pos2RF"] = config.igate_loc2rf;
		data["pos2INET"] = config.igate_loc2inet;

		char buf[32];
		snprintf(buf, sizeof(buf), "%.5f", config.igate_lat);
		data["lat"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.igate_lon);
		data["lon"] = buf;
		snprintf(buf, sizeof(buf), "%.2f", config.igate_alt);
		data["alt"] = buf;

		data["phg"] = config.igate_phg;

		data["rf2inetGrpDisabled"] = !config.rf2inet;
		data["rf2inetMsg"] = (config.rf2inetFilter & FILTER_MESSAGE) != 0;
		data["rf2inetStatus"] = (config.rf2inetFilter & FILTER_STATUS) != 0;
		data["rf2inetTlm"] = (config.rf2inetFilter & FILTER_TELEMETRY) != 0;
		data["rf2inetWx"] = (config.rf2inetFilter & FILTER_WX) != 0;
		data["rf2inetObj"] = (config.rf2inetFilter & FILTER_OBJECT) != 0;
		data["rf2inetItem"] = (config.rf2inetFilter & FILTER_ITEM) != 0;
		data["rf2inetQuery"] = (config.rf2inetFilter & FILTER_QUERY) != 0;
		data["rf2inetBuoy"] = (config.rf2inetFilter & FILTER_BUOY) != 0;
		data["rf2inetPos"] = (config.rf2inetFilter & FILTER_POSITION) != 0;
		data["rf2inetAll"] = (config.rf2inetFilter & FILTER_ENABLE_ALL) != 0;

		data["inet2rfGrpDisabled"] = !config.inet2rf;
		data["inet2rfMsg"] = (config.inet2rfFilter & FILTER_MESSAGE) != 0;
		data["inet2rfStatus"] = (config.inet2rfFilter & FILTER_STATUS) != 0;
		data["inet2rfTlm"] = (config.inet2rfFilter & FILTER_TELEMETRY) != 0;
		data["inet2rfWx"] = (config.inet2rfFilter & FILTER_WX) != 0;
		data["inet2rfObj"] = (config.inet2rfFilter & FILTER_OBJECT) != 0;
		data["inet2rfItem"] = (config.inet2rfFilter & FILTER_ITEM) != 0;
		data["inet2rfQuery"] = (config.inet2rfFilter & FILTER_QUERY) != 0;
		data["inet2rfBuoy"] = (config.inet2rfFilter & FILTER_BUOY) != 0;
		data["inet2rfPos"] = (config.inet2rfFilter & FILTER_POSITION) != 0;

		// Telemetry (tlmChannels) data moved to handle_igate2() - no longer
		// needed here, IGATE_TEMPLATE doesn't reference those keys anymore.

		// Page content (IGATE form + Filter form; Telemetry form moved to
		// handle_igate2/IGATE2_TEMPLATE) is smaller than before the original
		// split, but still sizeable - keep the generous reserve in line with
		// the other large pages that work reliably at this scale (MOD=55000,
		// System=48000).
		log_e("[IGATE] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		esp_task_wdt_reset();
		delay(1);
		String html_str = renderTemplate(IGATE_TEMPLATE, data.as<JsonVariant>());
		// Streamed response: avoids a single large calloc() for the whole page.
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		// instead of requiring one big contiguous heap block up front - this was
		// the reproducible trigger for a heap corruption assert (block_trim_free)
		// under real traffic, confirmed via heap-integrity diagnostics.
		sendHtmlChunked(request, html_str, "IGATE");
	}
}

// LU6JMF - Marcelo Ferreira (Set/2026): Telemetry split out of handle_igate()
// into its own page/handler (see IGATE2_TEMPLATE above). RF2INET/INET2RF
// Filter handling moved back into handle_igate() per user request - only
// Telemetry stays here. Same commitIGATETlm POST contract as before.
void handle_igate2(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitIGATETlm"))
	{
		for (int i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "igateTlmInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.igate_tlm_interval = request->arg(i).toInt();
				}
			}

			String arg;
			for (int x = 0; x < 5; x++)
			{
				arg = "sensorCH" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.igate_tlm_sensor[x] = request->arg(i).toInt();
				}
				arg = "param" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.igate_tlm_PARM[x], request->arg(i).c_str());
					}
				}
				arg = "unit" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.igate_tlm_UNIT[x], request->arg(i).c_str());
					}
				}
				arg = "precision" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.igate_tlm_precision[x] = request->arg(i).toInt();
				}
				arg = "offset" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.igate_tlm_offset[x] = request->arg(i).toFloat();
				}
				for (int y = 0; y < 3; y++)
				{
					arg = "eqns" + String(x) + String((char)(y + 'a'));
					if (request->argName(i) == arg)
					{
						if (isValidNumber(request->arg(i)))
							config.igate_tlm_EQNS[x][y] = request->arg(i).toFloat();
					}
				}
			}
		}

		String html;
		if (saveConfiguration("/default.cfg", config))
		{
			html = "Setup completed successfully";
			request->send(200, "text/html", html); // send to someones browser when asked
		}
		else
		{
			html = "Save config failed.";
			request->send(501, "text/html", html); // Not Implemented
		}
	}
	else
	{
		esp_task_wdt_reset();
		JsonDocument data;
		char buf[32];

		data["tlmInterval"] = config.igate_tlm_interval;

		JsonArray tlmChannels = data["tlmChannels"].to<JsonArray>();
		for (int ax = 0; ax < 5; ax++)
		{
			JsonObject c = tlmChannels.add<JsonObject>();
			c["idx"] = ax;
			c["num"] = ax + 1;
			c["param"] = config.igate_tlm_PARM[ax];
			c["unit"] = config.igate_tlm_UNIT[ax];
			c["precision"] = config.igate_tlm_precision[ax];
			snprintf(buf, sizeof(buf), "%.5f", config.igate_tlm_EQNS[ax][0]);
			c["eqnsA"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.igate_tlm_EQNS[ax][1]);
			c["eqnsB"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.igate_tlm_EQNS[ax][2]);
			c["eqnsC"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.igate_tlm_offset[ax]);
			c["offset"] = buf;

			JsonArray sensorOptions = c["sensorOptions"].to<JsonArray>();
			for (uint8_t idx = 0; idx < 11; idx++)
			{
				JsonObject o = sensorOptions.add<JsonObject>();
				o["value"] = idx;
				o["label"] = (idx == 0) ? String("NONE") : ("SENSOR#" + String(idx));
				o["selected"] = (config.igate_tlm_sensor[ax] == idx);
			}
		}

		esp_task_wdt_reset();
		String html_str = renderTemplate(IGATE2_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "IGATETlm");
	}
}

static const char *DIGI_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitDIGI\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/digi',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n<script type=\"text/javascript\">\n"
	"function openWindowSymbol() {\n"
	"var i, l, options = [{\n"
	"value: 'first',\n"
	"text: 'First'\n"
	"}, {\n"
	"value: 'second',\n"
	"text: 'Second'\n"
	"}],\n"
	"newWindow = window.open(\"/symbol\", null, \"height=400,width=400,status=no,toolbar=no,menubar=no,titlebar=no,location=no\");\n"
	"}\n"
	"function setValue(symbol,table) {\n"
	"document.getElementById('digiSymbol').value = String.fromCharCode(symbol);\n"
	"if(table==1){\n document.getElementById('digiTable').value='/';\n"
	"}else if(table==2){\n document.getElementById('digiTable').value='\\\\';\n}\n"
	"document.getElementById('digiImgSymbol').src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n"
	"\n}\n"
	"function calculatePHGR(){document.forms.formDIGI.texttouse.value=\"PHG\"+calcPower(document.forms.formDIGI.power.value)+calcHeight(document.forms.formDIGI.haat.value)+calcGain(document.forms.formDIGI.gain.value)+calcDirection(document.forms.formDIGI.direction.selectedIndex)}function Log2(e){return Math.log(e)/Math.log(2)}function calcPerHour(e){return e<10?e:String.fromCharCode(65+(e-10))}function calcHeight(e){return String.fromCharCode(48+Math.round(Log2(e/10),0))}function calcPower(e){if(e<1)return 0;if(e>=1&&e<4)return 1;if(e>=4&&e<9)return 2;if(e>=9&&e<16)return 3;if(e>=16&&e<25)return 4;if(e>=25&&e<36)return 5;if(e>=36&&e<49)return 6;if(e>=49&&e<64)return 7;if(e>=64&&e<81)return 8;if(e>=81)return 9}function calcDirection(e){if(e==\"0\")return\"0\";if(e==\"1\")return\"1\";if(e==\"2\")return\"2\";if(e==\"3\")return\"3\";if(e==\"4\")return\"4\";if(e==\"5\")return\"5\";if(e==\"6\")return\"6\";if(e==\"7\")return\"7\";if(e==\"8\")return\"8\"}function calcGain(e){return e>9?\"9\":e<0?\"0\":Math.round(e,0)}\n"
	"function selPrecision(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"precision\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n"
	"}\n"
	"function selOffset(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"offset\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n"
	"}\n"
	"</script>\n"
	"<form id='formDIGI' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[DIGI] Digital Repeater Mode</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiEnable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Auto Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiAuto\" value=\"OK\" {{#if digiAuto}}checked{{/if}}><span class=\"slider round\"></span></label> <i>*Automatic enable when APRS-IS disconnected</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"{{myCall}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station SSID:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"mySSID\" id=\"mySSID\">\n"
	"{{#each ssidOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{value}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Symbol:</b></td>\n"
	"<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"digiTable\" name=\"digiTable\" type=\"text\" value=\"{{symTable}}\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"digiSymbol\" name=\"digiSymbol\" type=\"text\" value=\"{{symSymbol}}\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"digiImgSymbol\" onclick=\"openWindowSymbol();\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{symImgNum}}-{{symImgTable}}.png\"> <i>*Click icon for select symbol</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"digiPath\" id=\"digiPath\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Comment:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"digiComment\" name=\"digiComment\" type=\"text\" value=\"{{comment}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Status:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"digiStatus\" name=\"digiStatus\" type=\"text\" value=\"{{status}}\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"digiSTSInv\" type=\"number\" value=\"{{statusInterval}}\" />Sec.</td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\"><b>Repeat Delay:</b></td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"100\" id=\"digiDelay\" name=\"digiDelay\" type=\"number\" value=\"{{delay}}\" /> mSec. <i>*0 is auto,Other random of delay time</i></td></tr>"
	"<tr>\n"
	"<td align=\"right\"><b>Time Stamp:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiTimeStamp\" value=\"OK\" {{#if timestamp}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr><td align=\"right\"><b>POSITION:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Beacon:</td><td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiBcnEnable\" value=\"OK\" {{#if bcnEn}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\">  Interval:<input min=\"0\" max=\"3600\" step=\"1\" id=\"digiPosInv\" name=\"digiPosInv\" type=\"number\" value=\"{{posInterval}}\" />Sec.</label></td></tr>"
	"<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"digiPosSel\" value=\"0\" {{#if posFix}}checked=\"checked\"{{/if}}/>Fix <input type=\"radio\" name=\"digiPosSel\" value=\"1\" {{#if posGPS}}checked=\"checked\"{{/if}}/>GPS </td></tr>\n"
	"<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"digiPos2RF\" value=\"OK\" {{#if pos2RF}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"digiPos2INET\" value=\"OK\" {{#if pos2INET}}checked{{/if}}/>Internet </td></tr>\n"
	"<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"digiPosLat\" name=\"digiPosLat\" type=\"number\" value=\"{{lat}}\" />degrees (positive for North, negative for South)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"digiPosLon\" name=\"digiPosLon\" type=\"number\" value=\"{{lon}}\" />degrees (positive for East, negative for West)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"digiPosAlt\" name=\"digiPosAlt\" type=\"number\" value=\"{{alt}}\" /> meter. *Value 0 is not send height</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PHG:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr>\n"
	"<td align=\"right\">Radio TX Power</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"power\" id=\"power\">\n"
	"<option value=\"1\" selected>1</option>\n"
	"<option value=\"5\">5</option>\n"
	"<option value=\"10\">10</option>\n"
	"<option value=\"15\">15</option>\n"
	"<option value=\"25\">25</option>\n"
	"<option value=\"35\">35</option>\n"
	"<option value=\"50\">50</option>\n"
	"<option value=\"65\">65</option>\n"
	"<option value=\"80\">80</option>\n"
	"</select> Watts</td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">Antenna Gain</td><td style=\"text-align: left;\"><input size=\"3\" min=\"0\" max=\"100\" step=\"0.1\" id=\"gain\" name=\"gain\" type=\"number\" value=\"6\" /> dBi</td></tr>\n"
	"<tr>\n"
	"<td align=\"right\">Height</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"haat\" id=\"haat\">\n"
	"<option value=\"10\" selected>10</option>\n"
	"<option value=\"20\">20</option>\n"
	"<option value=\"40\">40</option>\n"
	"<option value=\"80\">80</option>\n"
	"<option value=\"160\">160</option>\n"
	"<option value=\"320\">320</option>\n"
	"<option value=\"640\">640</option>\n"
	"<option value=\"1280\">1280</option>\n"
	"<option value=\"2560\">2560</option>\n"
	"<option value=\"5120\">5120</option>\n"
	"</select> Feet</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\">Antenna/Direction</td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"direction\" id=\"direction\">\n"
	"<option>Omni</option><option>NE</option><option>E</option><option>SE</option><option>S</option><option>SW</option><option>W</option><option>NW</option><option>N</option>\n"
	"</select></td>\n"
	"</tr>\n"
	"<tr><td align=\"right\"><b>PHG Text</b></td><td align=\"left\"><input name=\"texttouse\" type=\"text\" size=\"6\" style=\"background-color: rgb(97, 239, 170);\" value=\"{{phg}}\"/> <input type=\"button\" value=\"Calculate PHG\" onclick=\"javascript:calculatePHGR()\" /></td></tr>\n"
	"</table></td>"
	"<tr>\n"
	"<td align=\"right\"><b>Filter:</b></td>\n"
	"<td align=\"center\">\n"
	"<fieldset id=\"FilterGrp\">\n"
	"<legend>Filter repeater</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterMessage\" type=\"checkbox\" value=\"OK\" {{#if filterMessage}}checked{{/if}}/>Message</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterStatus\" type=\"checkbox\" value=\"OK\" {{#if filterStatus}}checked{{/if}}/>Status</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterTelemetry\" type=\"checkbox\" value=\"OK\" {{#if filterTelemetry}}checked{{/if}}/>Telemetry</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterWeather\" type=\"checkbox\" value=\"OK\" {{#if filterWeather}}checked{{/if}}/>Weather</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterObject\" type=\"checkbox\" value=\"OK\" {{#if filterObject}}checked{{/if}}/>Object</td>\n"
	"</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterItem\" type=\"checkbox\" value=\"OK\" {{#if filterItem}}checked{{/if}}/>Item</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterQuery\" type=\"checkbox\" value=\"OK\" {{#if filterQuery}}checked{{/if}}/>Query</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterBuoy\" type=\"checkbox\" value=\"OK\" {{#if filterBuoy}}checked{{/if}}/>Buoy</td>\n"
	"<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterPosition\" type=\"checkbox\" value=\"OK\" {{#if filterPosition}}checked{{/if}}/>Position</td>\n"
	"<td style=\"border:unset;\"></td>"
	"</tr></table></fieldset>\n"
	"</td></tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n"
	"<td align=\"center\"><table>\n"
	"<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"digiTlmInv\" name=\"digiTlmInv\" type=\"number\" value=\"{{tlmInterval}}\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>"
	"{{#each tlmChannels}}<tr><td align=\"right\"><b>CH A{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Sensor:</td>\n"
	"<td style=\"text-align: left;\">CH: "
	"<select name=\"sensorCH{{idx}}\" id=\"sensorCH{{idx}}\">\n"
	"{{#each sensorOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param{{idx}}\" type=\"text\" value=\"{{param}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit{{idx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision{{idx}}\" type=\"text\" value=\"{{precision}}\" onchange=\"selPrecision({{idx}})\"/></td></tr>\n"
	"<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}a\" type=\"number\" value=\"{{eqnsA}}\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}b\" type=\"number\" value=\"{{eqnsB}}\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}c\" type=\"number\" value=\"{{eqnsC}}\" /> (av<sup>2</sup>+bv+c) </td>\n"
	"<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset{{idx}}\" type=\"text\" value=\"{{offset}}\" onchange=\"selOffset({{idx}})\" /></td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"</table></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitDIGI'  name=\"commitDIGI\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitDIGI\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />";

void handle_digi(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	bool digiEn = false;
	bool digiAuto = false;
	bool posGPS = false;
	bool bcnEN = false;
	bool pos2RF = false;
	bool pos2INET = false;
	bool timeStamp = false;

	if (request->hasArg("commitDIGI"))
	{
		config.digiFilter = 0;
		for (int i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "digiEnable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						digiEn = true;
				}
			}
			if (request->argName(i) == "digiAuto")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						digiAuto = true;
				}
			}
			if (request->argName(i) == "myCall")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					name.toUpperCase();
					strcpy(config.digi_mycall, name.c_str());
				}
			}
			if (request->argName(i) == "mySSID")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_ssid = request->arg(i).toInt();
					if (config.digi_ssid > 15)
						config.digi_ssid = 3;
				}
			}
			if (request->argName(i) == "digiDelay")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_delay = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "digiPosInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "digiSTSInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_sts_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "digiPosLat")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_lat = request->arg(i).toFloat();
				}
			}

			if (request->argName(i) == "digiPosLon")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_lon = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "digiPosAlt")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_alt = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "digiPosSel")
			{
				if (request->arg(i) != "")
				{
					if (request->arg(i).toInt() == 1)
						posGPS = true;
				}
			}

			if (request->argName(i) == "digiTable")
			{
				if (request->arg(i) != "")
				{
					config.digi_symbol[0] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "digiSymbol")
			{
				if (request->arg(i) != "")
				{
					config.digi_symbol[1] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "digiPath")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "digiComment")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.digi_comment, request->arg(i).c_str());
				}
				else
				{
					memset(config.digi_comment, 0, sizeof(config.digi_comment));
				}
			}
			if (request->argName(i) == "texttouse")
			{
				if (request->arg(i) != "")
				{
					strncpy(config.digi_phg, request->arg(i).c_str(), sizeof(config.digi_phg) - 1);
config.digi_phg[sizeof(config.digi_phg) - 1] = 0;
				}
			}
			if (request->argName(i) == "digiStatus")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.digi_status, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "digiPos2RF")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						pos2RF = true;
				}
			}
			if (request->argName(i) == "digiPos2INET")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						pos2INET = true;
				}
			}
			if (request->argName(i) == "digiBcnEnable")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						bcnEN = true;
				}
			}
			// Filter
			if (request->argName(i) == "FilterMessage")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_MESSAGE;
				}
			}

			if (request->argName(i) == "FilterTelemetry")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_TELEMETRY;
				}
			}

			if (request->argName(i) == "FilterStatus")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_STATUS;
				}
			}

			if (request->argName(i) == "FilterWeather")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_WX;
				}
			}

			if (request->argName(i) == "FilterObject")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_OBJECT;
				}
			}

			if (request->argName(i) == "FilterItem")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_ITEM;
				}
			}

			if (request->argName(i) == "FilterQuery")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_QUERY;
				}
			}
			if (request->argName(i) == "FilterBuoy")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_BUOY;
				}
			}
			if (request->argName(i) == "FilterPosition")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						config.digiFilter |= FILTER_POSITION;
				}
			}
			if (request->argName(i) == "digiTimeStamp")
			{
				if (request->arg(i) != "")
				{
					if (strcmp(request->arg(i).c_str(), "OK") == 0)
						timeStamp = true;
				}
			}
			if (request->argName(i) == "digiTlmInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.digi_tlm_interval = request->arg(i).toInt();
				}
			}

			for (int x = 0; x < 5; x++)
			{
				{
					char *arg = allocateStringMemory(32);
					if (arg)
					{
						snprintf(arg, 32, "sensorCH%d", x);
						if (request->argName(i) == String(arg))
						{
							if (isValidNumber(request->arg(i)))
								config.digi_tlm_sensor[x] = request->arg(i).toInt();
						}
						free(arg);
					}
				}
				{
					char *arg = allocateStringMemory(32);
					if (arg)
					{
						snprintf(arg, 32, "param%d", x);
						if (request->argName(i) == String(arg))
						{
							if (request->arg(i) != "")
							{
								strcpy(config.digi_tlm_PARM[x], request->arg(i).c_str());
							}
						}
						free(arg);
					}
				}
				{
					char *arg = allocateStringMemory(32);
					if (arg)
					{
						snprintf(arg, 32, "unit%d", x);
						if (request->argName(i) == String(arg))
						{
							if (request->arg(i) != "")
							{
								strcpy(config.digi_tlm_UNIT[x], request->arg(i).c_str());
							}
						}
						free(arg);
					}
				}
				{
					char *arg = allocateStringMemory(32);
					if (arg)
					{
						snprintf(arg, 32, "precision%d", x);
						if (request->argName(i) == String(arg))
						{
							if (isValidNumber(request->arg(i)))
								config.digi_tlm_precision[x] = request->arg(i).toInt();
						}
						free(arg);
					}
				}
				{
					char *arg = allocateStringMemory(32);
					if (arg)
					{
						snprintf(arg, 32, "offset%d", x);
						if (request->argName(i) == String(arg))
						{
							if (isValidNumber(request->arg(i)))
								config.digi_tlm_offset[x] = request->arg(i).toFloat();
						}
						free(arg);
					}
				}
				for (int y = 0; y < 3; y++)
				{
					{
						char *arg = allocateStringMemory(32);
						if (arg)
						{
							snprintf(arg, 32, "eqns%d%c", x, (char)(y + 'a'));
							if (request->argName(i) == String(arg))
							{
								if (isValidNumber(request->arg(i)))
									config.digi_tlm_EQNS[x][y] = request->arg(i).toFloat();
							}
							free(arg);
						}
					}
				}
			}
		}
		config.digi_en = digiEn;
		config.digi_auto = digiAuto;
		config.digi_gps = posGPS;
		config.digi_bcn = bcnEN;
		config.digi_loc2rf = pos2RF;
		config.digi_loc2inet = pos2INET;
		config.digi_timestamp = timeStamp;

		initInterval = true;
		saveConfig(request);
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_digi runs on the async_tcp task itself,
		// same class of page as MSG/IGATE (5 TLM channels, 9-flag filter fieldset).
		JsonDocument data;
		data["enabled"] = config.digi_en;
		data["digiAuto"] = config.digi_auto;
		data["myCall"] = config.digi_mycall;

		JsonArray ssidOptions = data["ssidOptions"].to<JsonArray>();
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			JsonObject o = ssidOptions.add<JsonObject>();
			o["value"] = ssid;
			o["selected"] = (config.digi_ssid == ssid);
		}

		auto tableFor = [](char first) -> const char *
		{
			if (first == 92)
				return "2";
			return "1";
		};
		char cbuf[2];
		cbuf[0] = config.digi_symbol[0];
		cbuf[1] = 0;
		data["symTable"] = cbuf;
		cbuf[0] = config.digi_symbol[1];
		cbuf[1] = 0;
		data["symSymbol"] = cbuf;
		data["symImgNum"] = (int)config.digi_symbol[1];
		data["symImgTable"] = tableFor(config.digi_symbol[0]);

		JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			JsonObject o = pathOptions.add<JsonObject>();
			o["value"] = pthIdx;
			o["label"] = PATH_NAME[pthIdx];
			o["selected"] = (config.digi_path == pthIdx);
		}

		data["comment"] = config.digi_comment;
		data["status"] = config.digi_status;
		data["statusInterval"] = config.digi_sts_interval;
		data["delay"] = config.digi_delay;
		data["timestamp"] = config.digi_timestamp;
		data["bcnEn"] = config.digi_bcn;
		data["posInterval"] = config.digi_interval;
		data["posFix"] = !config.digi_gps;
		data["posGPS"] = config.digi_gps;
		data["pos2RF"] = config.digi_loc2rf;
		data["pos2INET"] = config.digi_loc2inet;

		char buf[32];
		snprintf(buf, sizeof(buf), "%.5f", config.digi_lat);
		data["lat"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.digi_lon);
		data["lon"] = buf;
		snprintf(buf, sizeof(buf), "%.2f", config.digi_alt);
		data["alt"] = buf;

		data["phg"] = config.digi_phg;

		data["filterMessage"] = (config.digiFilter & FILTER_MESSAGE) != 0;
		data["filterStatus"] = (config.digiFilter & FILTER_STATUS) != 0;
		data["filterTelemetry"] = (config.digiFilter & FILTER_TELEMETRY) != 0;
		data["filterWeather"] = (config.digiFilter & FILTER_WX) != 0;
		data["filterObject"] = (config.digiFilter & FILTER_OBJECT) != 0;
		data["filterItem"] = (config.digiFilter & FILTER_ITEM) != 0;
		data["filterQuery"] = (config.digiFilter & FILTER_QUERY) != 0;
		data["filterBuoy"] = (config.digiFilter & FILTER_BUOY) != 0;
		data["filterPosition"] = (config.digiFilter & FILTER_POSITION) != 0;

		data["tlmInterval"] = config.digi_tlm_interval;

		JsonArray tlmChannels = data["tlmChannels"].to<JsonArray>();
		for (int ax = 0; ax < 5; ax++)
		{
			JsonObject c = tlmChannels.add<JsonObject>();
			c["idx"] = ax;
			c["num"] = ax + 1;
			c["param"] = config.digi_tlm_PARM[ax];
			c["unit"] = config.digi_tlm_UNIT[ax];
			c["precision"] = config.digi_tlm_precision[ax];
			snprintf(buf, sizeof(buf), "%.5f", config.digi_tlm_EQNS[ax][0]);
			c["eqnsA"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.digi_tlm_EQNS[ax][1]);
			c["eqnsB"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.digi_tlm_EQNS[ax][2]);
			c["eqnsC"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.digi_tlm_offset[ax]);
			c["offset"] = buf;

			JsonArray sensorOptions = c["sensorOptions"].to<JsonArray>();
			for (uint8_t idx = 0; idx < 11; idx++)
			{
				JsonObject o = sensorOptions.add<JsonObject>();
				o["value"] = idx;
				o["label"] = (idx == 0) ? String("NONE") : ("SENSOR#" + String(idx));
				o["selected"] = (config.digi_tlm_sensor[ax] == idx);
			}
		}

		esp_task_wdt_reset();
		String html_str = renderTemplate(DIGI_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "digi");
	}
}

static const char *WX_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitWX\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/wx',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<form id='formWX' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[WX] Weather Station</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"{{myCall}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station SSID:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"mySSID\" id=\"mySSID\">\n"
	"{{#each ssidOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{value}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Object Name:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" name=\"Object\" type=\"text\" value=\"{{object}}\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"Path\" id=\"Path\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Comment:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"50\" name=\"Comment\" type=\"text\" value=\"{{comment}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Time Stamp:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wxTimeStamp\" value=\"OK\" {{#if timestamp}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr><td align=\"right\"><b>POSITION:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" id=\"PosInv\" name=\"PosInv\" type=\"number\" value=\"{{posInterval}}\" />Sec.</td></tr>"
	"<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"PosSel\" value=\"0\" {{#if posFix}}checked=\"checked\"{{/if}}/>Fix <input type=\"radio\" name=\"PosSel\" value=\"1\" {{#if posGPS}}checked=\"checked\"{{/if}}/>GPS </td></tr>\n"
	"<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"Pos2RF\" value=\"OK\" {{#if pos2RF}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"Pos2INET\" value=\"OK\" {{#if pos2INET}}checked{{/if}}/>Internet </td></tr>\n"
	"<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" name=\"PosLat\" type=\"number\" value=\"{{lat}}\" />degrees (positive for North, negative for South)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" name=\"PosLon\" type=\"number\" value=\"{{lon}}\" />degrees (positive for East, negative for West)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" name=\"PosAlt\" type=\"number\" value=\"{{alt}}\" /> meter. *The altitude in meters(m) above sea level</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitWX'  name=\"commitWX\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitWX\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />\n"
	"<div style=\"margin:8px 0;padding:6px;background:#fff3cd;border:1px solid #ffe69c;font-size:9pt;\">Sensor channel mapping (26 channels) moved to its own page - see the <b>WXCH</b> tab.</div>\n";

// LU6JMF - Marcelo Ferreira (Set/2026): Sensor channel mapping (26 channels)
// split out of the WX page into its own page/tab. Same senEn{idx}/sensorCH{idx}/
// avgSel{idx} POST contract as before, just no longer competing with the rest
// of the WX page for space/heap (this was the worst offender of the three
// split pages - WX_SENSOR_NUM=26 rows, each with an 11-option nested select).
static const char *WX2_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitWXch\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/wx2',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<form id='formWXch' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[WX] Sensor Channel Mapping</b></span></th>\n"
	"<tr><td align=\"right\"><b>SENSOR:<br />Selection</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"{{#each sensors}}<tr><td align=\"right\"><b>{{name}}:</b> \n"
	"<label class=\"switch\"><input type=\"checkbox\" name=\"senEn{{idx}}\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label>"
	"</td>\n"
	"<td style=\"text-align: left;\">Sensor Channel: "
	"<select name=\"sensorCH{{idx}}\" id=\"sensorCH{{idx}}\">\n"
	"{{#each chOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select>\n"
	"<input type=\"radio\" name=\"avgSel{{idx}}\" value=\"0\" {{#if avg}}{{else}}checked=\"checked\"{{/if}}/>Sample <input type=\"radio\" name=\"avgSel{{idx}}\" value=\"1\" {{#if avg}}checked=\"checked\"{{/if}}/>Average"
	"</td></tr>"
	"{{/each}}"
	"</table></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitWXch'  name=\"commitWXch\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitWXch\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />";

void handle_wx(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	bool En = false;
	bool posGPS = false;
	bool pos2RF = false;
	bool pos2INET = false;
	bool timeStamp = false;

	if (request->hasArg("commitWX"))
	{
		for (int i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}
			if (request->argName(i) == "Object")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					strcpy(config.wx_object, name.c_str());
				}
				else
				{
					config.wx_object[0] = 0;
				}
			}
			if (request->argName(i) == "myCall")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					name.toUpperCase();
					strcpy(config.wx_mycall, name.c_str());
				}
			}
			if (request->argName(i) == "mySSID")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_ssid = request->arg(i).toInt();
					if (config.wx_ssid > 15)
						config.wx_ssid = 3;
				}
			}
			// if (request->argName(i) == "channel")
			// {
			// 	if (request->arg(i) != "")
			// 	{
			// 		if (isValidNumber(request->arg(i)))
			// 			config.wx_channel = request->arg(i).toInt();
			// 	}
			// }
			if (request->argName(i) == "PosInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "PosLat")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_lat = request->arg(i).toFloat();
				}
			}

			if (request->argName(i) == "PosLon")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_lon = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "PosAlt")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_alt = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "PosSel")
			{
				if (request->arg(i) != "")
				{
					if (request->arg(i).toInt() == 1)
						posGPS = true;
				}
			}

			if (request->argName(i) == "Path")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.wx_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "Comment")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wx_comment, request->arg(i).c_str());
				}
				else
				{
					memset(config.wx_comment, 0, sizeof(config.wx_comment));
				}
			}
			if (request->argName(i) == "Pos2RF")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2RF = true;
				}
			}
			if (request->argName(i) == "Pos2INET")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2INET = true;
				}
			}
			if (request->argName(i) == "wxTimeStamp")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						timeStamp = true;
				}
			}
		}
		config.wx_en = En;
		config.wx_gps = posGPS;
		config.wx_2rf = pos2RF;
		config.wx_2inet = pos2INET;
		config.wx_timestamp = timeStamp;

		initInterval = true;
		String html_msg;
		if (saveConfiguration("/default.cfg", config))
		{
			html_msg = "Setup completed successfully";
			request->send(200, "text/html", html_msg); // send to someones browser when asked
		}
		else
		{
			html_msg = "Save config failed.";
			request->send(501, "text/html", html_msg); // Not Implemented
		}
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_wx runs on the async_tcp task itself,
		// same class of page as MSG/IGATE/DIGI (per-sensor channel selects, WX_SENSOR_NUM rows).
		JsonDocument data;
		data["enabled"] = config.wx_en;
		data["myCall"] = config.wx_mycall;

		JsonArray ssidOptions = data["ssidOptions"].to<JsonArray>();
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			JsonObject o = ssidOptions.add<JsonObject>();
			o["value"] = ssid;
			o["selected"] = (config.wx_ssid == ssid);
		}

		data["object"] = config.wx_object;

		JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			JsonObject o = pathOptions.add<JsonObject>();
			o["value"] = pthIdx;
			o["label"] = PATH_NAME[pthIdx];
			o["selected"] = (config.wx_path == pthIdx);
		}

		data["comment"] = config.wx_comment;
		data["timestamp"] = config.wx_timestamp;
		data["posInterval"] = config.wx_interval;
		data["posFix"] = !config.wx_gps;
		data["posGPS"] = config.wx_gps;
		data["pos2RF"] = config.wx_2rf;
		data["pos2INET"] = config.wx_2inet;

		char buf[32];
		snprintf(buf, sizeof(buf), "%.5f", config.wx_lat);
		data["lat"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.wx_lon);
		data["lon"] = buf;
		snprintf(buf, sizeof(buf), "%.2f", config.wx_alt);
		data["alt"] = buf;

		log_e("[WX] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		esp_task_wdt_reset();
		String html_str = renderTemplate(WX_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "Weather");
	}
}

// LU6JMF - Marcelo Ferreira (Set/2026): Sensor channel mapping (26 channels)
// split out of handle_wx() into their own page/handler (see WX2_TEMPLATE
// above). Same senEn{idx}/sensorCH{idx}/avgSel{idx} POST contract as before.
void handle_wx2(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitWXch"))
	{
		for (int x = 0; x < WX_SENSOR_NUM; x++)
			config.wx_sensor_enable[x] = false;

		String arg;
		for (int i = 0; i < request->args(); i++)
		{
			for (int x = 0; x < WX_SENSOR_NUM; x++)
			{
				arg = "senEn" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						if (String(request->arg(i)) == "OK")
							config.wx_sensor_enable[x] = true;
					}
				}
				arg = "sensorCH" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.wx_sensor_ch[x] = request->arg(i).toInt();
				}
				arg = "avgSel" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						if (request->arg(i).toInt() == 1)
							config.wx_sensor_avg[x] = true;
						else if (request->arg(i).toInt() == 0)
							config.wx_sensor_avg[x] = false;
					}
				}
			}
		}

		String html_msg;
		if (saveConfiguration("/default.cfg", config))
		{
			html_msg = "Setup completed successfully";
			request->send(200, "text/html", html_msg); // send to someones browser when asked
		}
		else
		{
			html_msg = "Save config failed.";
			request->send(501, "text/html", html_msg); // Not Implemented
		}
	}
	else
	{
		esp_task_wdt_reset(); // feed watchdog: handle_wx2 runs on the async_tcp task itself,
		// same class of page as MSG/IGATE/DIGI (per-sensor channel selects, WX_SENSOR_NUM rows).
		JsonDocument data;

		JsonArray sensors = data["sensors"].to<JsonArray>();
		for (int ax = 0; ax < WX_SENSOR_NUM; ax++)
		{
			JsonObject s = sensors.add<JsonObject>();
			s["idx"] = ax;
			s["name"] = WX_SENSOR[ax];
			s["enabled"] = config.wx_sensor_enable[ax];
			s["avg"] = config.wx_sensor_avg[ax];

			JsonArray chOptions = s["chOptions"].to<JsonArray>();
			for (uint8_t idx = 0; idx < 11; idx++)
			{
				JsonObject o = chOptions.add<JsonObject>();
				o["value"] = idx;
				o["label"] = (idx == 0) ? String("NONE") : ("SENSOR#" + String(idx));
				o["selected"] = (config.wx_sensor_ch[ax] == idx);
			}
		}

		log_e("[WX2] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		esp_task_wdt_reset();
		String html_str = renderTemplate(WX2_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "WeatherCh");
	}
}

static const char *TLM_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitTLM\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/tlm',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<form id='formTLM' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>System Telemetry</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"{{myCall}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station SSID:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"mySSID\" id=\"mySSID\">\n"
	"{{#each ssidOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{value}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"Path\" id=\"Path\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Comment:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"50\" name=\"Comment\" type=\"text\" value=\"{{comment}}\" /></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">Info Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" name=\"infoInv\" type=\"number\" value=\"{{infoInterval}}\" />Sec.</td></tr>"
	"<tr><td style=\"text-align: right;\">Data Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" name=\"dataInv\" type=\"number\" value=\"{{dataInterval}}\" />Sec.</td></tr>"
	"<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"Pos2RF\" value=\"OK\" {{#if pos2RF}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"Pos2INET\" value=\"OK\" {{#if pos2INET}}checked{{/if}}/>Internet </td></tr>\n"
	"{{#each channelsA}}<tr><td align=\"right\"><b>Channel A{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Type/Name:</td>\n"
	"<td style=\"text-align: left;\">Sensor Type: "
	"<select name=\"sensorCH{{chIdx}}\" id=\"sensorCH{{chIdx}}\">\n"
	"{{#each sensorOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Parameter: <input maxlength=\"10\" size=\"8\" name=\"param{{chIdx}}\" type=\"text\" value=\"{{param}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit{{chIdx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns{{chIdx}}a\" type=\"number\" value=\"{{eqnsA}}\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns{{chIdx}}b\" type=\"number\" value=\"{{eqnsB}}\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns{{chIdx}}c\" type=\"number\" value=\"{{eqnsC}}\" /> (av<sup>2</sup>+bv+c)</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"{{#each channelsB}}<tr><td align=\"right\"><b>Channel B{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<td style=\"text-align: left;\">Type: "
	"<select name=\"sensorCH{{selIdx}}\" id=\"sensorCH{{bitIdx}}\">\n"
	"{{#each sensorOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Parameter: <input maxlength=\"10\" size=\"8\" name=\"param{{selIdx}}\" type=\"text\" value=\"{{param}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit{{selIdx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"<td style=\"text-align: left;\"> Active:<input type=\"radio\" name=\"bitact{{bitIdx}}\" value=\"0\" {{#if low}}checked=\"checked\"{{/if}}/>LOW <input type=\"radio\" name=\"bitact{{bitIdx}}\" value=\"1\" {{#if high}}checked=\"checked\"{{/if}}/>HIGH </td>\n"
	"</tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitTLM'  name=\"commitTLM\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitTLM\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />";

void handle_tlm(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	bool En = false;
	bool pos2RF = false;
	bool pos2INET = false;
	String arg = "";

	if (request->hasArg("commitTLM"))
	{
		for (int i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "Enable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						En = true;
				}
			}
			if (request->argName(i) == "myCall")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					name.toUpperCase();
					strcpy(config.tlm0_mycall, name.c_str());
				}
			}
			if (request->argName(i) == "mySSID")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tlm0_ssid = request->arg(i).toInt();
					if (config.tlm0_ssid > 15)
						config.tlm0_ssid = 3;
				}
			}
			if (request->argName(i) == "infoInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tlm0_info_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "dataInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tlm0_data_interval = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "Path")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.tlm0_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "Comment")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.tlm0_comment, request->arg(i).c_str());
				}
				else
				{
					memset(config.tlm0_comment, 0, sizeof(config.tlm0_comment));
				}
			}
			if (request->argName(i) == "Pos2RF")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2RF = true;
				}
			}
			if (request->argName(i) == "Pos2INET")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2INET = true;
				}
			}
			for (int x = 0; x < 13; x++)
			{
				arg = "sensorCH" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.tml0_data_channel[x] = request->arg(i).toInt();
				}
				arg = "param" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.tlm0_PARM[x], request->arg(i).c_str());
					}
				}
				arg = "unit" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.tlm0_UNIT[x], request->arg(i).c_str());
					}
				}
				if (x < 5)
				{
					for (int y = 0; y < 3; y++)
					{
						arg = "eqns" + String(x) + String((char)(y + 'a'));
						if (request->argName(i) == arg)
						{
							if (isValidNumber(request->arg(i)))
								config.tlm0_EQNS[x][y] = request->arg(i).toFloat();
						}
					}
				}
			}
			uint8_t b = 1;
			for (int x = 0; x < 8; x++)
			{
				arg = "bitact" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
					{
						if (request->arg(i).toInt() == 1)
						{
							config.tlm0_BITS_Active |= b;
						}
						else
						{
							config.tlm0_BITS_Active &= ~b;
						}
					}
				}
				b <<= 1;
			}
		}
		config.tlm0_en = En;
		config.tlm0_2rf = pos2RF;
		config.tlm0_2inet = pos2INET;

		initInterval = true;
		String html_msg;
		if (saveConfiguration("/default.cfg", config))
		{
			html_msg = "Setup completed successfully";
			request->send(200, "text/html", html_msg); // send to someones browser when asked
		}
		else
		{
			html_msg = "Save config failed.";
			request->send(501, "text/html", html_msg); // Not Implemented
		}
	}
	else
	{
		JsonDocument data;
		data["enabled"] = config.tlm0_en;
		data["myCall"] = config.tlm0_mycall;

		JsonArray ssidOptions = data["ssidOptions"].to<JsonArray>();
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			JsonObject o = ssidOptions.add<JsonObject>();
			o["value"] = ssid;
			o["selected"] = (config.tlm0_ssid == ssid);
		}

		JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			JsonObject o = pathOptions.add<JsonObject>();
			o["value"] = pthIdx;
			o["label"] = PATH_NAME[pthIdx];
			o["selected"] = (config.tlm0_path == pthIdx);
		}

		data["comment"] = config.tlm0_comment;
		data["infoInterval"] = config.tlm0_info_interval;
		data["dataInterval"] = config.tlm0_data_interval;
		data["pos2RF"] = config.tlm0_2rf;
		data["pos2INET"] = config.tlm0_2inet;

		JsonArray channelsA = data["channelsA"].to<JsonArray>();
		for (int ax = 0; ax < 5; ax++)
		{
			JsonObject c = channelsA.add<JsonObject>();
			c["num"] = ax + 1;
			c["chIdx"] = ax;
			c["param"] = config.tlm0_PARM[ax];
			c["unit"] = config.tlm0_UNIT[ax];
			char buf[24];
			snprintf(buf, sizeof(buf), "%.3f", config.tlm0_EQNS[ax][0]);
			c["eqnsA"] = buf;
			snprintf(buf, sizeof(buf), "%.3f", config.tlm0_EQNS[ax][1]);
			c["eqnsB"] = buf;
			snprintf(buf, sizeof(buf), "%.3f", config.tlm0_EQNS[ax][2]);
			c["eqnsC"] = buf;

			JsonArray sensorOptions = c["sensorOptions"].to<JsonArray>();
			for (uint8_t idx = 0; idx < SYSTEM_LEN; idx++)
			{
				JsonObject o = sensorOptions.add<JsonObject>();
				o["value"] = idx;
				o["label"] = SYSTEM_NAME[idx];
				o["selected"] = (config.tml0_data_channel[ax] == idx);
			}
		}

		JsonArray channelsB = data["channelsB"].to<JsonArray>();
		uint8_t b = 1;
		for (int ax = 0; ax < 8; ax++)
		{
			JsonObject c = channelsB.add<JsonObject>();
			c["num"] = ax + 1;
			c["selIdx"] = ax + 5;
			c["bitIdx"] = ax;
			c["param"] = config.tlm0_PARM[ax + 5];
			c["unit"] = config.tlm0_UNIT[ax + 5];
			c["high"] = (config.tlm0_BITS_Active & b) != 0;
			c["low"] = !(config.tlm0_BITS_Active & b);

			JsonArray sensorOptions = c["sensorOptions"].to<JsonArray>();
			for (uint8_t idx = 0; idx < SYSTEM_BIT_LEN; idx++)
			{
				JsonObject o = sensorOptions.add<JsonObject>();
				o["value"] = idx;
				o["label"] = SYSTEM_BITS_NAME[idx];
				o["selected"] = (config.tml0_data_channel[ax + 5] == idx);
			}
			b <<= 1;
		}

		esp_task_wdt_reset();
		String html_str = renderTemplate(TLM_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "Telemetry");
	}
}

extern TaskHandle_t taskSensorHandle;

static const char *SENSOR_TEMPLATE =
	"<table>\n"
	"<th colspan=\"5\"><span><b>Sensor Monitor</b></span></th>\n"
	"{{#each monitorRows}}<tr>\n"
	"{{#each this}}<td align=\"center\">\n"
	"<fieldset id=\"SenGrp{{idx}}\" {{#if enabled}}{{else}}disabled{{/if}}>\n"
	"<legend>{{legend}}</legend>\n"
	"<input id=\"sVal{{idx}}\" style=\"text-align:right;\" size=\"5\" type=\"text\" value=\"{{value}}\" readonly/> {{unit}}\n"
	"</fieldset>\n"
	"</td>\n"
	"{{/each}}"
	"</tr>\n"
	"{{/each}}"
	"</table><br />\n"
	"<div style=\"margin:8px 0;padding:6px;background:#fff3cd;border:1px solid #ffe69c;font-size:9pt;\">Sensor configuration (10 channels) moved to its own page - see the <b>SCFG</b> tab.</div>\n";

// LU6JMF - Marcelo Ferreira (Set/2026): Sensor configuration (10 channels)
// split out of the SENSOR page into its own page/tab. Same En{idx}/sensorCH{idx}/
// sensorP{idx}/address{idx}/sample{idx}/avg{idx}/param{idx}/unit{idx}/eqns{idx}{a,b,c}
// POST contract as before, just no longer competing with the live Sensor
// Monitor table for space/heap.
static const char *SENSOR2_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitSENSOR\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/sensor2',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"function setElm(name,val) {\n"
	"document.getElementById(name).value=val;\n"
	"};\n"
	"function selSensorType(idx) {\n"
	"var x=0;\n"
	"var parm=\"param\"+idx;\n"
	"var unit=\"unit\"+idx;\n"
	"x = document.getElementById(\"sensorCH\"+idx).value;\n"
	"if (x==1) {\n"
	"setElm(parm,\"Co2\");setElm(unit,\"ppm\");\n"
	"}else if (x==2) {\n"
	"setElm(parm,\"CH2O\");setElm(unit,\"\xce\xbcg/m\xc2\xb3\");\n"
	"}else if (x==3) {\n"
	"setElm(parm,\"TVOC\");setElm(unit,\"\xce\xbcg/m\xc2\xb3\");\n"
	"}else if (x==4) {\n"
	"setElm(parm,\"PM2.5\");setElm(unit,\"\xce\xbcg/m\xc2\xb3\");\n"
	"}else if (x==5) {\n"
	"setElm(parm,\"PM10.0\");setElm(unit,\"\xce\xbcg/m\xc2\xb3\");\n"
	"}else if (x==6) {\n"
	"setElm(parm,\"Temperature\");setElm(unit,\"\xc2\xb0""C\");\n"
	"}else if (x==7) {\n"
	"setElm(parm,\"Humidity\");setElm(unit,\"%RH\");\n"
	"}else if (x==8) {\n"
	"setElm(parm,\"Pressure\");setElm(unit,\"hPa\");\n"
	"}else if (x==9) {\n"
	"setElm(parm,\"WindSpeed\");setElm(unit,\"kPh\");\n"
	"}else if (x==10) {\n"
	"setElm(parm,\"WindCourse\");setElm(unit,\"\xc2\xb0\");\n"
	"}else if (x==11) {\n"
	"setElm(parm,\"Rain\");setElm(unit,\"mm\");\n"
	"}else if (x==12) {\n"
	"setElm(parm,\"Luminosity\");setElm(unit,\"W/m\xc2\xb3\");\n"
	"}else if (x==13) {\n"
	"setElm(parm,\"SoilTemp\");setElm(unit,\"\xc2\xb0""C\");\n"
	"}else if (x==14) {\n"
	"setElm(parm,\"SoilMoisture\");setElm(unit,\"%VWC\");\n"
	"}else if (x==15) {\n"
	"setElm(parm,\"WaterTemp\");setElm(unit,\"\xc2\xb0""C\");\n"
	"}else if (x==16) {\n"
	"setElm(parm,\"WaterTDS\");setElm(unit,\" \");\n"
	"}else if (x==17) {\n"
	"setElm(parm,\"WaterLevel\");setElm(unit,\"mm\");\n"
	"}else if (x==18) {\n"
	"setElm(parm,\"WaterFlow\");setElm(unit,\"L/min\");\n"
	"}else if (x==19) {\n"
	"setElm(parm,\"Voltage\");setElm(unit,\"V\");\n"
	"}else if (x==20) {\n"
	"setElm(parm,\"Current\");setElm(unit,\"A\");\n"
	"}else if (x==21) {\n"
	"setElm(parm,\"Power\");setElm(unit,\"W\");\n"
	"}else if (x==22) {\n"
	"setElm(parm,\"Energy\");setElm(unit,\"Wh\");\n"
	"}else if (x==23) {\n"
	"setElm(parm,\"Frequency\");setElm(unit,\"Hz\");\n"
	"}else if (x==24) {\n"
	"setElm(parm,\"PF\");setElm(unit,\" \");\n"
	"}else if (x==25) {\n"
	"setElm(parm,\"Satellite\");setElm(unit,\" \");\n"
	"}else if (x==26) {\n"
	"setElm(parm,\"HDOP\");setElm(unit,\" \");\n"
	"}else if (x==27) {\n"
	"setElm(parm,\"Battery\");setElm(unit,\"V\");\n"
	"}else if (x==28) {\n"
	"setElm(parm,\"BattLevel\");setElm(unit,\"%\");\n"
	"}\n}\n"
	"function selSensor(idx) {\n"
	"var x=0;\n"
	"x = document.getElementById(\"sensorP\"+idx).value;\n"
	"if (x>=10 && x<=13) {\n"
	"document.getElementById(\"address\"+idx).value={{#if beamS3}}119{{else}}118{{/if}};\n"
	"}else if (x==16 || x==17) {\n"
	"document.getElementById(\"address\"+idx).value=90;\n"
	"}else if (x==23) {\n"
	"document.getElementById(\"address\"+idx).value=1;\n"
	"}else if (x==24 || x==25) {\n"
	"document.getElementById(\"address\"+idx).value=1000;\n"
	"}else{\n"
	"document.getElementById(\"address\"+idx).value=0;\n"
	"}\n}\n"
	"</script>\n"
	"<form id='formSENSOR' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>Sensor Config</b></span></th>\n"
	"{{#each sensorConfigs}}<tr><td align=\"right\"><b>SENSOR#{{num}}:</b><br />\n"
	"<label class=\"switch\"><input type=\"checkbox\" name=\"En{{idx}}\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label>"
	"</td><td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Type:</td>\n"
	"<td style=\"text-align: left;\"><select name=\"sensorCH{{idx}}\" id=\"sensorCH{{idx}}\" onchange=\"selSensorType({{idx}})\">\n"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Name: <input maxlength=\"15\" size=\"15\" name=\"param{{idx}}\" id=\"param{{idx}}\" type=\"text\" value=\"{{parm}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"10\" size=\"5\" name=\"unit{{idx}}\" id=\"unit{{idx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">PORT:</td>\n<td style=\"text-align: left;\">\n<select name=\"sensorP{{idx}}\" id=\"sensorP{{idx}}\" onchange=\"selSensor({{idx}})\">\n"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Addr/Reg/GPIO: <input style=\"text-align:right;\" min=\"0\" max=\"6500\" step=\"1\" name=\"address{{idx}}\" id=\"address{{idx}}\" type=\"number\" value=\"{{port}}\" /></td>\n"
	"<td style=\"text-align: left;\">Sample: <input style=\"text-align:right;\" min=\"0\" max=\"9999\" step=\"1\" name=\"sample{{idx}}\" type=\"number\" value=\"{{sample}}\" />Sec.\n"
	"Average: <input style=\"text-align:right;\" min=\"0\" max=\"999\" step=\"1\" name=\"avg{{idx}}\" type=\"number\" value=\"{{avg}}\" />Sec.</td></tr>\n"
	"<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns{{idx}}a\" type=\"number\" value=\"{{eqnsA}}\" />  b:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns{{idx}}b\" type=\"number\" value=\"{{eqnsB}}\" /> c:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns{{idx}}c\" type=\"number\" value=\"{{eqnsC}}\" /> (av<sup>2</sup>+bv+c)</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitSENSOR'  name=\"commitSENSOR\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitSENSOR\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />"
	"<script type=\"text/javascript\">\n"
	"if (typeof typeArry === 'undefined'){let typeArry = [];};\n"
	"typeArry = new Array({{#each typeNames}}'{{.}}',{{/each}});\n"
	"if (typeof portArry === 'undefined'){let portArry = [];};\n"
	"portArry = new Array({{#each portNames}}'{{.}}',{{/each}});\n"
	"if (typeof typeSel === 'undefined'){var typeSel = [];};\n"
	"if (typeof listType === 'undefined'){var listType = [];};\n"
	"if (typeof portSel === 'undefined'){var portSel = [];};\n"
	"if (typeof listPort === 'undefined'){var listPort = [];};\n"
	"{{#each initSensors}}listType[{{i}}] = document.querySelector('#sensorCH{{i}}');typeSel[{{i}}]={{type}};\n"
	"listPort[{{i}}] = document.querySelector('#sensorP{{i}}');portSel[{{i}}]={{port}};\n"
	"{{/each}}"
	"for (let n = 0; n < 10; n++){\n"
	"for (let i = 0; i < typeArry.length; i++) {\n"
	"const optionType = new Option(typeArry[i], i);\n"
	"listType[n].add(optionType, undefined);\n"
	"};\n"
	"listType[n].options[typeSel[n]].selected = true;\n"
	"for (let p = 0; p < portArry.length; p++) {\n"
	"const optionPort = new Option(portArry[p], p);\n"
	"listPort[n].add(optionPort, undefined);\n"
	"};\n"
	"listPort[n].options[portSel[n]].selected = true;\n"
	"};\n"
	"</script>\n";

void handle_sensor(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	{
		JsonDocument data;

		JsonArray monitorRows = data["monitorRows"].to<JsonArray>();
		{
			int ax = 0;
			for (int r = 0; r < 3; r++)
			{
				JsonArray row = monitorRows.add<JsonArray>();
				for (int c = 0; c < 5; c++)
				{
					JsonObject cell = row.add<JsonObject>();
					cell["idx"] = ax;
					cell["enabled"] = config.sensor[ax].enable;
					cell["legend"] = "SEN#" + String(ax + 1) + "-" + String(config.sensor[ax].parm);
					char buf[24];
					snprintf(buf, sizeof(buf), "%.2f", sen[ax].sample);
					cell["value"] = buf;
					cell["unit"] = config.sensor[ax].unit;
					ax++;
					if (ax >= SENSOR_NUMBER)
						break;
				}
				if (ax >= SENSOR_NUMBER)
					break;
			}
		}

		log_e("[SENSOR] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		esp_task_wdt_reset();
		String html_str = renderTemplate(SENSOR_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "Sensor");
	}
}

// LU6JMF - Marcelo Ferreira (Set/2026): Sensor configuration (10 channels)
// split out of handle_sensor() into their own page/handler (see SENSOR2_TEMPLATE
// above). Same En{idx}/sensorCH{idx}/sensorP{idx}/address{idx}/sample{idx}/
// avg{idx}/param{idx}/unit{idx}/eqns{idx}{a,b,c} POST contract as before.
void handle_sensor2(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);
	String arg = "";

	if (request->hasArg("commitSENSOR"))
	{
		// vTaskSuspend(taskSensorHandle);
		for (int x = 0; x < SENSOR_NUMBER; x++)
		{
			config.sensor[x].enable = false;
		}
		for (int i = 0; i < request->args(); i++)
		{
			// log_d("Arg %s: %s", request->argName(i).c_str(), request->arg(i).c_str());
			for (int x = 0; x < SENSOR_NUMBER; x++)
			{
				arg = "En" + String(x);
				if (request->argName(i) == arg)
				{

					if (request->arg(i) != "")
					{
						if (String(request->arg(i)) == "OK")
							config.sensor[x].enable = true;
					}
				}
				arg = "sensorCH" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.sensor[x].type = request->arg(i).toInt();
				}
				arg = "sensorP" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.sensor[x].port = request->arg(i).toInt();
				}
				arg = "address" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.sensor[x].address = request->arg(i).toInt();
				}
				arg = "sample" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.sensor[x].samplerate = request->arg(i).toInt();
				}
				arg = "avg" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.sensor[x].averagerate = request->arg(i).toInt();
				}
				arg = "param" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.sensor[x].parm, request->arg(i).c_str());
					}
				}
				arg = "unit" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.sensor[x].unit, request->arg(i).c_str());
					}
				}
				for (int y = 0; y < 3; y++)
				{
					arg = "eqns" + String(x) + String((char)(y + 'a'));
					if (request->argName(i) == arg)
					{
						if (isValidNumber(request->arg(i)))
							config.sensor[x].eqns[y] = request->arg(i).toFloat();
					}
				}
				//}
			}
		}

		sensorInit(true);
		log_d("Sensor Config Updated.");
		String html_msg;
		if (saveConfiguration("/default.cfg", config))
		{
			html_msg = "Setup completed successfully";
			request->send(200, "text/html", html_msg); // send to someones browser when asked
		}
		else
		{
			html_msg = "Save config failed.";
			request->send(501, "text/html", html_msg); // Not Implemented
		}
		// vTaskResume(taskSensorHandle);
	}
	else
	{
		JsonDocument data;
#ifdef TTGO_T_Beam_S3_SUPREME_V3
		data["beamS3"] = true;
#else
		data["beamS3"] = false;
#endif

		JsonArray sensorConfigs = data["sensorConfigs"].to<JsonArray>();
		for (int ax = 0; ax < SENSOR_NUMBER; ax++)
		{
			JsonObject s = sensorConfigs.add<JsonObject>();
			s["idx"] = ax;
			s["num"] = ax + 1;
			s["enabled"] = config.sensor[ax].enable;
			s["parm"] = config.sensor[ax].parm;
			s["unit"] = config.sensor[ax].unit;
			s["port"] = config.sensor[ax].address;
			s["sample"] = config.sensor[ax].samplerate;
			s["avg"] = config.sensor[ax].averagerate;
			char buf[24];
			snprintf(buf, sizeof(buf), "%.5f", config.sensor[ax].eqns[0]);
			s["eqnsA"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.sensor[ax].eqns[1]);
			s["eqnsB"] = buf;
			snprintf(buf, sizeof(buf), "%.5f", config.sensor[ax].eqns[2]);
			s["eqnsC"] = buf;
		}

		JsonArray typeNames = data["typeNames"].to<JsonArray>();
		for (uint8_t idx = 0; idx < SENSOR_NAME_NUM; idx++)
			typeNames.add(SENSOR_NAME[idx]);

		JsonArray portNames = data["portNames"].to<JsonArray>();
		for (uint8_t idx = 0; idx < SENSOR_PORT_NUM; idx++)
			portNames.add(SENSOR_PORT[idx]);

		JsonArray initSensors = data["initSensors"].to<JsonArray>();
		for (int i = 0; i < 10; i++)
		{
			JsonObject s = initSensors.add<JsonObject>();
			s["i"] = i;
			s["type"] = config.sensor[i].type;
			s["port"] = config.sensor[i].port;
		}

		log_e("[SENSOR2] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		esp_task_wdt_reset(); // feed watchdog: handle_sensor2 runs on the async_tcp task itself
		String html_str = renderTemplate(SENSOR2_TEMPLATE, data.as<JsonVariant>());
		sendHtmlChunked(request, html_str, "SensorCfg");
	}
}

static const char *TRACKER_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"document.getElementById(\"submitTRACKER\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/tracker',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n<script type=\"text/javascript\">\n"
	"function openWindowSymbol(sel) {\n"
	"var i, l, options = [{\n"
	"value: 'first',\n"
	"text: 'First'\n"
	"}, {\n"
	"value: 'second',\n"
	"text: 'Second'\n"
	"}],\n"
	"newWindow = window.open(\"/symbol?sel=\"+sel.toString(), null, \"height=400,width=400,status=no,toolbar=no,menubar=no,location=no\");\n"
	"}\n"
	"function setValue(sel,symbol,table) {\n"
	"var txtsymbol=document.getElementById('trackerSymbol');\n"
	"var txttable=document.getElementById('trackerTable');\n"
	"var imgicon=document.getElementById('trackerImgSymbol');\n"
	"if(sel==1){\n"
	"txtsymbol=document.getElementById('moveSymbol');\n"
	"txttable=document.getElementById('moveTable');\n"
	"imgicon= document.getElementById('moveImgSymbol');\n"
	"}else if(sel==2){\n"
	"txtsymbol=document.getElementById('stopSymbol');\n"
	"txttable=document.getElementById('stopTable');\n"
	"imgicon= document.getElementById('stopImgSymbol');\n"
	"}\n"
	"txtsymbol.value = String.fromCharCode(symbol);\n"
	"if(table==1){\n txttable.value='/';\n"
	"}else if(table==2){\n txttable.value='\\\\';\n}\n"
	"imgicon.src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n"
	"\n}\n"
	"function onSmartCheck() {\n"
	"if (document.querySelector('#smartBcnEnable').checked) {\n"
	"document.getElementById(\"smartbcnGrp\").disabled=false;\n"
	"} else {\n"
	"document.getElementById(\"smartbcnGrp\").disabled=true;\n"
	"}\n}\n"
	"function selPrecision(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"precision\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n"
	"}\n"
	"function selOffset(idx) {\n"
	"var x=0;\n"
	"x = document.getElementsByName(\"offset\"+idx)[0].value;\n"
	"document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n"
	"}\n"
	"</script>\n"
	"<form id='formtracker' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>[TRACKER] Tracker Position Mode</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"trackerEnable\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station Callsign:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"{{myCall}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Station SSID:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"mySSID\" id=\"mySSID\">\n"
	"{{#each ssidOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{value}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Item/Obj Name:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" id=\"trackerObject\" name=\"trackerObject\" type=\"text\" value=\"{{object}}\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PATH:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"trackerPath\" id=\"trackerPath\">\n"
	"{{#each pathOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Comment:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"trackerComment\" name=\"trackerComment\" type=\"text\" value=\"{{comment}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Text Status:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"trkStatus\" name=\"trkStatus\" type=\"text\" value=\"{{status}}\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"trkSTSInv\" type=\"number\" value=\"{{statusInterval}}\" />Sec.</td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Smart Beacon:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"smartBcnEnable\" name=\"smartBcnEnable\" onclick=\"onSmartCheck()\" value=\"OK\" {{#if smartBcnEn}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch use to smart beacon mode</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Compress:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"compressEnable\" value=\"OK\" {{#if compressEn}}checked{{/if}}><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch compress packet</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Mic-E Type:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"trkMicEType\" id=\"trkMicEType\">\n"
	"{{#each micEOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select><label style=\"vertical-align: bottom;font-size: 8pt;\"><i>*Support if Compress is enabled and not use Item/Obj,Time Stamp or UnUsed</i></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Time Stamp:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"trackerTimeStamp\" value=\"OK\" {{#if timestamp}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"trackerPos2RF\" value=\"OK\" {{#if pos2RF}}checked{{/if}}/>RF <input type=\"checkbox\" name=\"trackerPos2INET\" value=\"OK\" {{#if pos2INET}}checked{{/if}}/>Internet </td></tr>\n"
	"<tr><td style=\"text-align: right;\"><b>Option:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"trackerOptCST\" value=\"OK\" {{#if optCST}}checked{{/if}}/>Telemetry <input type=\"checkbox\" name=\"trackerOptAlt\" value=\"OK\" {{#if optAlt}}checked{{/if}}/>Altutude <input type=\"checkbox\" name=\"trackerOptBat\" value=\"OK\" {{#if optBat}}checked{{/if}}/>Audio Request </td></tr>\n"
	"<tr>"
	"<td align=\"right\"><b>POSITION:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" id=\"trackerPosInv\" name=\"trackerPosInv\" type=\"number\" value=\"{{posInterval}}\" />Sec.</label></td></tr>"
	"<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"trackerPosSel\" value=\"0\" {{#if posFix}}checked=\"checked\"{{/if}}/>Fix <input type=\"radio\" name=\"trackerPosSel\" value=\"1\" {{#if posGPS}}checked=\"checked\"{{/if}}/>GPS </td></tr>\n"
	"<tr>\n"
	"<td align=\"right\">Symbol Icon:</td>\n"
	"<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"trackerTable\" name=\"trackerTable\" type=\"text\" value=\"{{symTable}}\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"trackerSymbol\" name=\"trackerSymbol\" type=\"text\" value=\"{{symSymbol}}\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"trackerImgSymbol\" onclick=\"openWindowSymbol(0);\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{symImgNum}}-{{symImgTable}}.png\"> <i>*Click icon for select symbol</i></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"trackerPosLat\" name=\"trackerPosLat\" type=\"number\" value=\"{{lat}}\" />degrees (positive for North, negative for South)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"trackerPosLon\" name=\"trackerPosLon\" type=\"number\" value=\"{{lon}}\" />degrees (positive for East, negative for West)</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"trackerPosAlt\" name=\"trackerPosAlt\" type=\"number\" value=\"{{alt}}\" /> meter. *Value 0 is not send height</td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Smart Beacon:</b></td>\n"
	"<td align=\"center\">\n"
	"<fieldset id=\"smartbcnGrp\" {{#if smartbeaconDisabled}}disabled{{/if}}>\n"
	"<legend>Smart beacon configuration</legend>\n<table>"
	"<tr>\n"
	"<td align=\"right\">Move Symbol:</td>\n"
	"<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"moveTable\" name=\"moveTable\" type=\"text\" value=\"{{moveTable}}\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"moveSymbol\" name=\"moveSymbol\" type=\"text\" value=\"{{moveSymbol}}\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"moveImgSymbol\" onclick=\"openWindowSymbol(1);\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{moveImgNum}}-{{moveImgTable}}.png\"> <i>*Click icon for select MOVE symbol</i></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\">Stop Symbol:</td>\n"
	"<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"stopTable\" name=\"stopTable\" type=\"text\" value=\"{{stopTable}}\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"stopSymbol\" name=\"stopSymbol\" type=\"text\" value=\"{{stopSymbol}}\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"stopImgSymbol\" onclick=\"openWindowSymbol(2);\" src=\"http://aprs.nakhonthai.net/symbols/icons/{{stopImgNum}}-{{stopImgTable}}.png\"> <i>*Click icon for select STOP symbol</i></td>\n"
	"</tr>\n"
	"<tr><td style=\"text-align: right;\">High Speed:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"10\" max=\"1000\" step=\"1\" id=\"hspeed\" name=\"hspeed\" type=\"number\" value=\"{{hspeed}}\" /> km/h</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Low Speed:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"250\" step=\"1\" id=\"lspeed\" name=\"lspeed\" type=\"number\" value=\"{{lspeed}}\" /> km/h</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Slow Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"60\" max=\"3600\" step=\"1\" id=\"slowInterval\" name=\"slowInterval\" type=\"number\" value=\"{{slowInterval}}\" /> Sec.</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Max Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"10\" max=\"255\" step=\"1\" id=\"maxInterval\" name=\"maxInterval\" type=\"number\" value=\"{{maxInterval}}\" /> Sec.</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Min Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"100\" step=\"1\" id=\"minInterval\" name=\"minInterval\" type=\"number\" value=\"{{minInterval}}\" /> Sec.</td></tr>\n"
	"<tr><td style=\"text-align: right;\">Min Angle:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"359\" step=\"1\" id=\"minAngle\" name=\"minAngle\" type=\"number\" value=\"{{minAngle}}\" /> Degree.</td></tr>\n"
	"</table></fieldset></tr>"
	"<tr>\n"
	"<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n"
	"<td align=\"center\"><table>\n"
	"<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"trkTlmInv\" name=\"trkTlmInv\" type=\"number\" value=\"{{tlmInterval}}\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>"
	"{{#each tlmChannels}}<tr><td align=\"right\"><b>CH A{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<table>"
	"<tr><td style=\"text-align: right;\">Sensor:</td>\n"
	"<td style=\"text-align: left;\">CH: "
	"<select name=\"sensorCH{{idx}}\" id=\"sensorCH{{idx}}\">\n"
	"{{#each sensorOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select></td>\n"
	"<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param{{idx}}\" type=\"text\" value=\"{{param}}\" /></td>\n"
	"<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit{{idx}}\" type=\"text\" value=\"{{unit}}\" /></td>\n"
	"<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision{{idx}}\" type=\"text\" value=\"{{precision}}\" onchange=\"selPrecision({{idx}})\" /></td></tr>\n"
	"<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}a\" type=\"number\" value=\"{{eqnsA}}\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}b\" type=\"number\" value=\"{{eqnsB}}\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns{{idx}}c\" type=\"number\" value=\"{{eqnsC}}\" /> (av<sup>2</sup>+bv+c) </td>\n"
	"<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset{{idx}}\" type=\"text\" value=\"{{offset}}\"  onchange=\"selOffset({{idx}})\" /></td></tr>\n"
	"</table></td>"
	"</tr>\n"
	"{{/each}}"
	"</table></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitTRACKER'  name=\"commitTRACKER\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitTRACKER\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />";

void handle_tracker(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	bool trakerEn = false;
	bool smartEn = false;
	bool compEn = false;

	bool posGPS = false;
	bool bcnEN = false;
	bool pos2RF = false;
	bool pos2INET = false;
	bool optCST = false;
	bool optAlt = false;
	bool optBat = false;
	bool optSat = false;
	bool timeStamp = false;

	if (request->hasArg("commitTRACKER"))
	{
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "trackerEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						trakerEn = true;
				}
			}
			if (request->argName(i) == "smartBcnEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						smartEn = true;
				}
			}
			if (request->argName(i) == "compressEnable")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						compEn = true;
				}
			}
			if (request->argName(i) == "trackerOptCST")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						optCST = true;
				}
			}
			if (request->argName(i) == "trackerOptAlt")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						optAlt = true;
				}
			}
			if (request->argName(i) == "trackerOptBat")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						optBat = true;
				}
			}
			if (request->argName(i) == "trackerOptSat")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						optSat = true;
				}
			}
			if (request->argName(i) == "myCall")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					name.toUpperCase();
					strcpy(config.trk_mycall, name.c_str());
				}
			}
			if (request->argName(i) == "trackerObject")
			{
				if (request->arg(i) != "")
				{
					String name = request->arg(i);
					name.trim();
					strcpy(config.trk_item, name.c_str());
				}
				else
				{
					memset(config.trk_item, 0, sizeof(config.trk_item));
				}
			}
			if (request->argName(i) == "mySSID")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_ssid = request->arg(i).toInt();
					if (config.trk_ssid > 15)
						config.trk_ssid = 13;
				}
			}
			if (request->argName(i) == "trackerPosInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "trkSTSInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_sts_interval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "trackerPosLat")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_lat = request->arg(i).toFloat();
				}
			}

			if (request->argName(i) == "trackerPosLon")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_lon = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "trackerPosAlt")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_alt = request->arg(i).toFloat();
				}
			}
			if (request->argName(i) == "trackerPosSel")
			{
				if (request->arg(i) != "")
				{
					if (request->arg(i).toInt() == 1)
						posGPS = true;
				}
			}
			if (request->argName(i) == "hspeed")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_hspeed = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "lspeed")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_lspeed = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "slowInterval")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_slowinterval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "maxInterval")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_maxinterval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "minInterval")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_mininterval = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "minAngle")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_minangle = request->arg(i).toInt();
				}
			}

			if (request->argName(i) == "trackerTable")
			{
				if (request->arg(i) != "")
				{
					config.trk_symbol[0] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "trackerSymbol")
			{
				if (request->arg(i) != "")
				{
					config.trk_symbol[1] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "moveTable")
			{
				if (request->arg(i) != "")
				{
					config.trk_symmove[0] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "moveSymbol")
			{
				if (request->arg(i) != "")
				{
					config.trk_symmove[1] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "stopTable")
			{
				if (request->arg(i) != "")
				{
					config.trk_symstop[0] = request->arg(i).charAt(0);
				}
			}
			if (request->argName(i) == "stopSymbol")
			{
				if (request->arg(i) != "")
				{
					config.trk_symstop[1] = request->arg(i).charAt(0);
				}
			}

			if (request->argName(i) == "trackerPath")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_path = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "trkMicEType")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_mice_type = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "trackerComment")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.trk_comment, request->arg(i).c_str());
				}
				else
				{
					memset(config.trk_comment, 0, sizeof(config.trk_comment));
				}
			}
			if (request->argName(i) == "trkStatus")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.trk_status, request->arg(i).c_str());
				}
				else
				{
					memset(config.trk_status, 0, sizeof(config.trk_status));
				}
			}

			if (request->argName(i) == "trackerPos2RF")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2RF = true;
				}
			}
			if (request->argName(i) == "trackerPos2INET")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						pos2INET = true;
				}
			}
			if (request->argName(i) == "trackerTimeStamp")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
						timeStamp = true;
				}
			}
			if (request->argName(i) == "trkTlmInv")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.trk_tlm_interval = request->arg(i).toInt();
				}
			}
			String arg;
			for (int x = 0; x < 5; x++)
			{
				arg = "sensorCH" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.trk_tlm_sensor[x] = request->arg(i).toInt();
				}
				arg = "param" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.trk_tlm_PARM[x], request->arg(i).c_str());
					}
				}
				arg = "unit" + String(x);
				if (request->argName(i) == arg)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.trk_tlm_UNIT[x], request->arg(i).c_str());
					}
				}
				arg = "precision" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.trk_tlm_precision[x] = request->arg(i).toInt();
				}
				arg = "offset" + String(x);
				if (request->argName(i) == arg)
				{
					if (isValidNumber(request->arg(i)))
						config.trk_tlm_offset[x] = request->arg(i).toFloat();
				}
				for (int y = 0; y < 3; y++)
				{
					arg = "eqns" + String(x) + String((char)(y + 'a'));
					if (request->argName(i) == arg)
					{
						if (isValidNumber(request->arg(i)))
							config.trk_tlm_EQNS[x][y] = request->arg(i).toFloat();
					}
				}
			}
		}
		config.trk_en = trakerEn;
		config.trk_smartbeacon = smartEn;
		config.trk_compress = compEn;

		config.trk_gps = posGPS;
		config.trk_loc2rf = pos2RF;
		config.trk_loc2inet = pos2INET;

		config.trk_log = optCST;
		config.trk_altitude = optAlt;
		config.trk_rssi = optBat;
		config.trk_sat = optSat;
		config.trk_timestamp = timeStamp;

		initInterval = true;
		saveConfig(request);
	}

	JsonDocument data;
	data["enabled"] = config.trk_en;
	data["myCall"] = config.trk_mycall;

	JsonArray ssidOptions = data["ssidOptions"].to<JsonArray>();
	for (uint8_t ssid = 0; ssid <= 15; ssid++)
	{
		JsonObject o = ssidOptions.add<JsonObject>();
		o["value"] = ssid;
		o["selected"] = (config.trk_ssid == ssid);
	}

	data["object"] = config.trk_item;

	JsonArray pathOptions = data["pathOptions"].to<JsonArray>();
	for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
	{
		JsonObject o = pathOptions.add<JsonObject>();
		o["value"] = pthIdx;
		o["label"] = PATH_NAME[pthIdx];
		o["selected"] = (config.trk_path == pthIdx);
	}

	data["comment"] = config.trk_comment;
	data["status"] = config.trk_status;
	data["statusInterval"] = config.trk_sts_interval;
	data["smartBcnEn"] = config.trk_smartbeacon;
	data["compressEn"] = config.trk_compress;

	JsonArray micEOptions = data["micEOptions"].to<JsonArray>();
	for (uint8_t micEIdx = 0; micEIdx < 9; micEIdx++)
	{
		JsonObject o = micEOptions.add<JsonObject>();
		o["value"] = micEIdx;
		o["label"] = MIC_E_MSG[micEIdx];
		o["selected"] = (config.trk_mice_type == micEIdx);
	}

	data["timestamp"] = config.trk_timestamp;
	data["pos2RF"] = config.trk_loc2rf;
	data["pos2INET"] = config.trk_loc2inet;
	data["optCST"] = config.trk_log;
	data["optAlt"] = config.trk_altitude;
	data["optBat"] = config.trk_rssi;

	data["posInterval"] = config.trk_interval;
	data["posFix"] = !config.trk_gps;
	data["posGPS"] = config.trk_gps;

	char cbuf[2];
	auto tableFor = [](char first) -> const char *
	{
		if (first == 92)
			return "2";
		return "1"; // default and table==47 ('/') both map to "1", matching original
	};

	cbuf[0] = config.trk_symbol[0];
	cbuf[1] = 0;
	data["symTable"] = cbuf;
	cbuf[0] = config.trk_symbol[1];
	cbuf[1] = 0;
	data["symSymbol"] = cbuf;
	data["symImgNum"] = (int)config.trk_symbol[1];
	data["symImgTable"] = tableFor(config.trk_symbol[0]);

	char buf[32];
	snprintf(buf, sizeof(buf), "%.5f", config.trk_lat);
	data["lat"] = buf;
	snprintf(buf, sizeof(buf), "%.5f", config.trk_lon);
	data["lon"] = buf;
	snprintf(buf, sizeof(buf), "%.2f", config.trk_alt);
	data["alt"] = buf;

	data["smartbeaconDisabled"] = !config.trk_smartbeacon;

	cbuf[0] = config.trk_symmove[0];
	cbuf[1] = 0;
	data["moveTable"] = cbuf;
	cbuf[0] = config.trk_symmove[1];
	cbuf[1] = 0;
	data["moveSymbol"] = cbuf;
	data["moveImgNum"] = (int)config.trk_symmove[1];
	data["moveImgTable"] = tableFor(config.trk_symmove[0]);

	cbuf[0] = config.trk_symstop[0];
	cbuf[1] = 0;
	data["stopTable"] = cbuf;
	cbuf[0] = config.trk_symstop[1];
	cbuf[1] = 0;
	data["stopSymbol"] = cbuf;
	data["stopImgNum"] = (int)config.trk_symstop[1];
	data["stopImgTable"] = tableFor(config.trk_symstop[0]);

	data["hspeed"] = config.trk_hspeed;
	data["lspeed"] = config.trk_lspeed;
	data["slowInterval"] = config.trk_slowinterval;
	data["maxInterval"] = config.trk_maxinterval;
	data["minInterval"] = config.trk_mininterval;
	data["minAngle"] = config.trk_minangle;

	data["tlmInterval"] = config.trk_tlm_interval;

	JsonArray tlmChannels = data["tlmChannels"].to<JsonArray>();
	for (int ax = 0; ax < 5; ax++)
	{
		JsonObject c = tlmChannels.add<JsonObject>();
		c["idx"] = ax;
		c["num"] = ax + 1;
		c["param"] = config.trk_tlm_PARM[ax];
		c["unit"] = config.trk_tlm_UNIT[ax];
		c["precision"] = config.trk_tlm_precision[ax];
		snprintf(buf, sizeof(buf), "%.5f", config.trk_tlm_EQNS[ax][0]);
		c["eqnsA"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.trk_tlm_EQNS[ax][1]);
		c["eqnsB"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.trk_tlm_EQNS[ax][2]);
		c["eqnsC"] = buf;
		snprintf(buf, sizeof(buf), "%.5f", config.trk_tlm_offset[ax]);
		c["offset"] = buf;

		JsonArray sensorOptions = c["sensorOptions"].to<JsonArray>();
		for (uint8_t idx = 0; idx < 11; idx++)
		{
			JsonObject o = sensorOptions.add<JsonObject>();
			o["value"] = idx;
			o["label"] = (idx == 0) ? String("NONE") : ("SENSOR#" + String(idx));
			o["selected"] = (config.trk_tlm_sensor[ax] == idx);
		}
	}

	delay(1);
	esp_task_wdt_reset();
	String html_str = renderTemplate(TRACKER_TEMPLATE, data.as<JsonVariant>());
	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	sendHtmlChunked(request, html_str, "Tracker");
}

static const char *WIRELESS_TEMPLATE =
	"<script type=\"text/javascript\">\n"
	"$('form').submit(function (e) {\n"
	"e.preventDefault();\n"
	"var data = new FormData(e.currentTarget);\n"
	"if(e.currentTarget.id===\"formBluetooth\") document.getElementById(\"submitBluetooth\").disabled=true;\n"
	"if(e.currentTarget.id===\"formWiFiAP\") document.getElementById(\"submitWiFiAP\").disabled=true;\n"
	"if(e.currentTarget.id===\"formWiFiClient\") document.getElementById(\"submitWiFiClient\").disabled=true;\n"
	"$.ajax({\n"
	"url: '/wireless',\n"
	"type: 'POST',\n"
	"data: data,\n"
	"contentType: false,\n"
	"processData: false,\n"
	"success: function (data) {\n"
	"alert(\"Submited Successfully\");\n"
	"},\n"
	"error: function (data) {\n"
	"alert(\"An error occurred.\");\n"
	"}\n"
	"});\n"
	"});\n"
	"</script>\n"
	"<form id='formWiFiAP' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>WiFi Access Point</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\" width=\"120\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wifiAP\" value=\"OK\" {{#if wifiAPEnabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi AP SSID:</b></td>\n"
	"<td style=\"text-align: left;\"><input size=\"32\" maxlength=\"32\" class=\"form-control\" id=\"wifi_ssidAP\" name=\"wifi_ssidAP\" type=\"text\" value=\"{{apSSID}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi AP PASSWORD:</b></td>\n"
	"<td style=\"text-align: left;\"><input size=\"63\" maxlength=\"63\" class=\"form-control\" id=\"wifi_passAP\" name=\"wifi_passAP\" type=\"password\" value=\"{{apPass}}\" /></td>\n"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitWiFiAP'  name=\"commitWiFiAP\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitWiFiAP\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />"
	"<br />\n"
	"<form id='formWiFiClient' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>WiFi Multi Station</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi STA Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wificlient\" value=\"OK\" {{#if wifiClientEnabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi RF Power:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"wifi_pwr\" id=\"wifi_pwr\">\n"
	"{{#each wifiPowerOptions}}<option value=\"{{value}}\" {{#if selected}}selected{{/if}}>{{label}}</option>\n"
	"{{/each}}"
	"</select>\n"
	"</td>\n"
	"</tr>\n"
	"{{#each stations}}<tr>\n"
	"<td align=\"right\"><b>Station #{{num}}:</b></td>\n"
	"<td align=\"center\">\n"
	"<fieldset id=\"filterDispGrp{{num}}\">\n"
	"<legend>WiFi Station #{{num}}</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">"
	"<tr style=\"background:unset;\">"
	"<td align=\"right\" width=\"120\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wifiStation{{n}}\" value=\"OK\" {{#if enabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi SSID:</b></td>\n"
	"<td style=\"text-align: left;\"><input size=\"32\" maxlength=\"32\" name=\"wifi_ssid{{n}}\" type=\"text\" value=\"{{ssid}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>WiFi PASSWORD:</b></td>\n"
	"<td style=\"text-align: left;\"><input size=\"63\" maxlength=\"63\" name=\"wifi_pass{{n}}\" type=\"password\" value=\"{{pass}}\" /></td>\n"
	"</tr>\n"
	"</tr></table></fieldset>\n"
	"</td></tr>\n"
	"{{/each}}"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitWiFiClient'  name=\"commitWiFiClient\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitWiFiClient\"/>\n"
	"</td></tr></table><br />\n"
	"</form><br />"
	"{{#if bluetooth}}"
	"<br />\n"
	"<form id='formBluetooth' method=\"POST\" action='#' enctype='multipart/form-data'>\n"
	"<table>\n"
	"<th colspan=\"2\"><span><b>Bluetooth Master (BLE)</b></span></th>\n"
	"<tr>\n"
	"<td align=\"right\"><b>Enable:</b></td>\n"
	"<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"btMaster\" value=\"OK\" {{#if btEnabled}}checked{{/if}}><span class=\"slider round\"></span></label></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>NAME:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"20\" id=\"bt_name\" name=\"bt_name\" type=\"text\" value=\"{{btName}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>PIN:</b></td>\n"
	"<td style=\"text-align: left;\"><input min=\"0\" max=\"999999\" id=\"bt_pin\" name=\"bt_pin\" type=\"number\" value=\"{{btPin}}\" /></td> <i>*Value 0 is no auth.</i>\n"
	"</tr>\n"
	"{{#if btUuidSupport}}"
	"<tr>\n"
	"<td align=\"right\"><b>UUID:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid\" name=\"bt_uuid\" type=\"text\" value=\"{{btUuid}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>UUID RX:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid_rx\" name=\"bt_uuid_rx\" type=\"text\" value=\"{{btUuidRx}}\" /></td>\n"
	"</tr>\n"
	"<tr>\n"
	"<td align=\"right\"><b>UUID TX:</b></td>\n"
	"<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid_tx\" name=\"bt_uuid_tx\" type=\"text\" value=\"{{btUuidTx}}\" /></td>\n"
	"</tr>\n"
	"{{/if}}"
	"<td align=\"right\"><b>MODE:</b></td>\n"
	"<td style=\"text-align: left;\">\n"
	"<select name=\"bt_mode\" id=\"bt_mode\">\n"
	"<option value=\"0\" {{#if btModeOff}}selected{{/if}}>NONE</option>\n"
	"<option value=\"1\" {{#if btModeTNC2}}selected{{/if}}>TNC2</option>\n"
	"<option value=\"2\" {{#if btModeKISS}}selected{{/if}}>KISS</option>\n"
	"</select>\n"
	"<label style=\"font-size: 8pt;text-align: right;\">*See the following for generating UUIDs: <a href=\"https://www.uuidgenerator.net\" target=\"_blank\">https://www.uuidgenerator.net</a></label></td>\n"
	"</tr>\n"
	"<tr><td colspan=\"2\" align=\"right\">\n"
	"<div><button class=\"button\" type='submit' id='submitBluetooth'  name=\"commitBluetooth\"> Apply Change </button></div>\n"
	"<input type=\"hidden\" name=\"commitBluetooth\"/>\n"
	"</td></tr></table><br />\n"
	"</form>"
	"{{/if}}";

void handle_wireless(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	if (request->hasArg("commitWiFiAP"))
	{
		bool wifiAP = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "wifiAP")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						wifiAP = true;
					}
				}
			}

			if (request->argName(i) == "wifi_ssidAP")
			{
				if (request->arg(i) != "")
				{
					strlcpy(config.wifi_ap_ssid, request->arg(i).c_str(), sizeof(config.wifi_ap_ssid));
				}
			}
			if (request->argName(i) == "wifi_passAP")
			{
				if (request->arg(i) != "")
				{
					strlcpy(config.wifi_ap_pass, request->arg(i).c_str(), sizeof(config.wifi_ap_pass));
				}
			}
		}
		if (wifiAP)
		{
			config.wifi_mode |= WIFI_AP_FIX;
		}
		else
		{
			config.wifi_mode &= ~WIFI_AP_FIX;
		}
		saveConfig(request);
	}
	else if (request->hasArg("commitWiFiClient"))
	{
		bool wifiSTA = false;
		for (int n = 0; n < 5; n++)
			config.wifi_sta[n].enable = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "wificlient")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						wifiSTA = true;
					}
				}
			}

			for (int n = 0; n < 5; n++)
			{
				String nameSSID = "wifiStation" + String(n);
				if (request->argName(i) == nameSSID)
				{
					if (request->arg(i) != "")
					{
						if (String(request->arg(i)) == "OK")
						{
							config.wifi_sta[n].enable = true;
						}
					}
				}
				nameSSID = "wifi_ssid" + String(n);
				if (request->argName(i) == nameSSID)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.wifi_sta[n].wifi_ssid, request->arg(i).c_str());
					}
				}
				String namePASS = "wifi_pass" + String(n);
				if (request->argName(i) == namePASS)
				{
					if (request->arg(i) != "")
					{
						strcpy(config.wifi_sta[n].wifi_pass, request->arg(i).c_str());
					}
				}
			}

			if (request->argName(i) == "wifi_pwr")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
					{
						config.wifi_power = (int8_t)request->arg(i).toInt();
						WiFi.setTxPower((wifi_power_t)config.wifi_power);
					}
				}
			}
		}
		if (wifiSTA)
		{
			config.wifi_mode |= WIFI_STA_FIX;
		}
		else
		{
			config.wifi_mode &= ~WIFI_STA_FIX;
		}
		saveConfig(request);
	}
	#ifdef BLUETOOTH
	else if (request->hasArg("commitBluetooth"))
	{
		bool btMaster = false;
		for (uint8_t i = 0; i < request->args(); i++)
		{
			if (request->argName(i) == "btMaster")
			{
				if (request->arg(i) != "")
				{
					if (String(request->arg(i)) == "OK")
					{
						btMaster = true;
					}
				}
			}

			if (request->argName(i) == "bt_name")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.bt_name, request->arg(i).c_str());
				}
			}
			#if !defined(CONFIG_IDF_TARGET_ESP32)
			if (request->argName(i) == "bt_uuid")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.bt_uuid, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "bt_uuid_rx")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.bt_uuid_rx, request->arg(i).c_str());
				}
			}
			if (request->argName(i) == "bt_uuid_tx")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.bt_uuid_tx, request->arg(i).c_str());
				}
			}
			#endif
			if (request->argName(i) == "bt_mode")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.bt_mode = request->arg(i).toInt();
				}
			}
			if (request->argName(i) == "bt_pin")
			{
				if (request->arg(i) != "")
				{
					if (isValidNumber(request->arg(i)))
						config.bt_pin = request->arg(i).toInt();
				}
			}
		}
		config.bt_master = btMaster;
		saveConfig(request);
	}
	#endif
	else
	{
		JsonDocument data;
		data["wifiAPEnabled"] = (config.wifi_mode & WIFI_AP_FIX) != 0;
		data["apSSID"] = config.wifi_ap_ssid;
		data["apPass"] = config.wifi_ap_pass;
		data["wifiClientEnabled"] = (config.wifi_mode & WIFI_STA_FIX) != 0;

		JsonArray wifiPowerOptions = data["wifiPowerOptions"].to<JsonArray>();
		for (int i = 0; i < 12; i++)
		{
			JsonObject o = wifiPowerOptions.add<JsonObject>();
			o["value"] = (int8_t)wifiPwr[i][0];
			char buf[16];
			snprintf(buf, sizeof(buf), "%.1f dBm", wifiPwr[i][1]);
			o["label"] = buf;
			o["selected"] = (config.wifi_power == (int8_t)wifiPwr[i][0]);
		}

		JsonArray stations = data["stations"].to<JsonArray>();
		for (int n = 0; n < 5; n++)
		{
			JsonObject s = stations.add<JsonObject>();
			s["n"] = n;
			s["num"] = n + 1;
			s["enabled"] = config.wifi_sta[n].enable;
			s["ssid"] = config.wifi_sta[n].wifi_ssid;
			s["pass"] = config.wifi_sta[n].wifi_pass;
		}

#ifdef BLUETOOTH
		data["bluetooth"] = true;
		data["btEnabled"] = config.bt_master;
		data["btName"] = config.bt_name;
		data["btPin"] = config.bt_pin;
#if !defined(CONFIG_IDF_TARGET_ESP32)
		data["btUuidSupport"] = true;
		data["btUuid"] = config.bt_uuid;
		data["btUuidRx"] = config.bt_uuid_rx;
		data["btUuidTx"] = config.bt_uuid_tx;
#else
		data["btUuidSupport"] = false;
#endif
		data["btModeOff"] = (config.bt_mode == 0);
		data["btModeTNC2"] = (config.bt_mode == 1);
		data["btModeKISS"] = (config.bt_mode == 2);
#else
		data["bluetooth"] = false;
#endif

		esp_task_wdt_reset();
		String html_str = renderTemplate(WIRELESS_TEMPLATE, data.as<JsonVariant>());
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		sendHtmlChunked(request, html_str, "wifi");
	}
}

// extern String lastPkgRaw;
// extern float dBV;
// extern int mVrms;
//  void handle_realtime(AsyncWebServerRequest *request)
//  {
//  	// char jsonMsg[1000];
//  	char *jsonMsg;
//  	time_t timeStamp;
//  	time(&timeStamp);

// 	if (afskSync && (lastPkgRaw.length() > 5))
// 	{
// 		int input_length = lastPkgRaw.length();
// 		jsonMsg = (char *)malloc((input_length * 2) + 200);
// 		char *input_buffer = (char *)malloc(input_length + 2);
// 		char *output_buffer = (char *)malloc(input_length * 2);
// 		if (output_buffer)
// 		{
// 			// lastPkgRaw.toCharArray(input_buffer, lastPkgRaw.length(), 0);
// 			memcpy(input_buffer, lastPkgRaw.c_str(), lastPkgRaw.length());
// 			lastPkgRaw.clear();
// 			encode_base64((unsigned char *)input_buffer, input_length, (unsigned char *)output_buffer);
// 			// Serial.println(output_buffer);
// 			sprintf(jsonMsg, "{\"Active\":\"1\",\"mVrms\":\"%d\",\"RAW\":\"%s\",\"timeStamp\":\"%li\"}", mVrms, output_buffer, timeStamp);
// 			// Serial.println(jsonMsg);
// 			free(input_buffer);
// 			free(output_buffer);
// 		}
// 	}
// 	else
// 	{
// 		jsonMsg = (char *)malloc(100);
// 		if (afskSync)
// 			sprintf(jsonMsg, "{\"Active\":\"1\",\"mVrms\":\"%d\",\"RAW\":\"REVDT0RFIEZBSUwh\",\"timeStamp\":\"%li\"}", mVrms, timeStamp);
// 		else
// 			sprintf(jsonMsg, "{\"Active\":\"0\",\"mVrms\":\"0\",\"RAW\":\"\",\"timeStamp\":\"%li\"}", timeStamp);
// 	}
// 	afskSync = false;
// 	request->send(200, "text/html", String(jsonMsg));

// 	delay(100);
// 	free(jsonMsg);
// }

// void handle_ws(String Raw,uint16_t mVrms)
void handle_ws(char *Raw, size_t len, uint16_t mVrms)
{
	if (ws.count() < 1)
		return;

	char *jsonMsg;
	time_t timeStamp;
	time(&timeStamp);

	if (len > 5)
	{
		int input_length = len;
		jsonMsg = (char *)calloc((input_length * 2) + 200, sizeof(char));
		if (jsonMsg)
		{
			char *input_buffer = (char *)calloc(input_length + 2, sizeof(char));
			char *output_buffer = (char *)calloc(input_length * 2, sizeof(char));
			if (output_buffer)
			{
				memset(input_buffer, 0, (input_length + 2));
				memset(output_buffer, 0, (input_length * 2));
				// lastPkgRaw.toCharArray(input_buffer, input_length, 0);
				memcpy(input_buffer, Raw, len);
				encode_base64((unsigned char *)input_buffer, input_length, (unsigned char *)output_buffer);
				// Serial.println(output_buffer);
				sprintf(jsonMsg, "{\"Active\":\"1\",\"mVrms\":\"%d\",\"RAW\":\"%s\",\"timeStamp\":\"%li\"}", mVrms, output_buffer, timeStamp);
				// Serial.println(jsonMsg);
				free(input_buffer);
				free(output_buffer);
			}
			ws.textAll(jsonMsg);
			free(jsonMsg);
		}
	}
	else
	{
		jsonMsg = (char *)calloc(300, sizeof(char));
		if (jsonMsg)
		{
			if (mVrms > 0)
				sprintf(jsonMsg, "{\"Active\":\"1\",\"mVrms\":\"%d\",\"RAW\":\"REVDT0RFIEZBSUwh\",\"timeStamp\":\"%li\"}", mVrms, timeStamp);
			else
				sprintf(jsonMsg, "{\"Active\":\"0\",\"mVrms\":\"0\",\"RAW\":\"\",\"timeStamp\":\"%li\"}", timeStamp);
			ws.textAll(jsonMsg);
			free(jsonMsg);
		}
	}
}

void handle_ws_gnss(char *nmea, size_t size)
{
	if (ws_gnss.count() < 1)
		return;

	time_t timeStamp;
	time(&timeStamp);
	// unsigned int output_length = encode_base64_length(size);
	// unsigned char nmea_enc[output_length];
	// char jsonMsg[output_length + 100];
	// encode_base64((unsigned char *)nmea, size, (unsigned char *)nmea_enc);
	// sprintf(jsonMsg, "{\"en\":\"%d\",\"lat\":\"%.5f\",\"lng\":\"%.5f\",\"alt\":\"%.2f\",\"spd\":\"%.2f\",\"csd\":\"%.1f\",\"hdop\":\"%.2f\",\"sat\":\"%d\",\"time\":\"%d\",\"timeStamp\":\"%li\",\"RAW\":\"", (int)config.gnss_enable, gps.location.lat(), gps.location.lng(), gps.altitude.meters(), gps.speed.kmph(), gps.course.deg(), gps.hdop.hdop(), gps.satellites.value(), gps.time.value(), timeStamp);
	// strncat(jsonMsg, (const char *)nmea_enc, output_length);
	// strcat(jsonMsg, "\"}");
	unsigned int output_length = encode_base64_length(size);
	unsigned char *nmea_enc = (unsigned char *)calloc(output_length + 2, sizeof(unsigned char));
	char *jsonMsg = (char *)calloc(output_length + 200, sizeof(char));
	if (nmea_enc && jsonMsg)
	{
		encode_base64((unsigned char *)nmea, size, (unsigned char *)nmea_enc);
		sprintf(jsonMsg, "{\"en\":\"%d\",\"lat\":\"%.5f\",\"lng\":\"%.5f\",\"alt\":\"%.2f\",\"spd\":\"%.2f\",\"csd\":\"%.1f\",\"hdop\":\"%.2f\",\"sat\":\"%d\",\"time\":\"%d\",\"timeStamp\":\"%li\",\"RAW\":\"", (int)config.gnss_enable, gps.location.lat(), gps.location.lng(), gps.altitude.meters(), gps.speed.kmph(), gps.course.deg(), gps.hdop.hdop(), gps.satellites.value(), gps.time.value(), timeStamp);
		strncat(jsonMsg, (const char *)nmea_enc, output_length);
		strcat(jsonMsg, "\"}");
		ws_gnss.textAll(jsonMsg);
		free(nmea_enc);
		free(jsonMsg);
	}
	
}

void handle_test(AsyncWebServerRequest *request)
{
	// if (request->hasArg("sendBeacon"))
	// {
	// 	String tnc2Raw = send_fix_location();
	// 	if (config.rf_en)
	// 		pkgTxPush(tnc2Raw.c_str(), tnc2Raw.length(), 0);
	// 	// APRS_sendTNC2Pkt(tnc2Raw); // Send packet to RF
	// }
	// else if (request->hasArg("sendRaw"))
	// {
	// 	for (uint8_t i = 0; i < request->args(); i++)
	// 	{
	// 		if (request->argName(i) == "raw")
	// 		{
	// 			if (request->arg(i) != "")
	// 			{
	// 				String tnc2Raw = request->arg(i);
	// 				if (config.rf_en)
	// 				{
	// 					pkgTxPush(tnc2Raw.c_str(), tnc2Raw.length(), 0);
	// 					// APRS_sendTNC2Pkt(request->arg(i)); // Send packet to RF
	// 					// Serial.println("Send RAW: " + tnc2Raw);
	// 				}
	// 			}
	// 			break;
	// 		}
	// 	}
	// }
	// setHTML(6);

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 9000);
	if (!webString)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	webString->print("<html>\n<head>\n");
	webString->print("<script src=\"https://apps.bdimg.com/libs/jquery/2.1.4/jquery.min.js\"></script>\n");
	webString->print("<script src=\"https://code.highcharts.com/highcharts.js\"></script>\n");
	webString->print("<script src=\"https://code.highcharts.com/highcharts-more.js\"></script>\n");
	webString->print("<script language=\"JavaScript\">");
	webString->print("$(document).ready(function() {\nvar chart = {\ntype: 'gauge',plotBorderWidth: 1,plotBackgroundColor: {linearGradient: { x1: 0, y1: 0, x2: 0, y2: 1 },stops: [[0, '#FFFFC6'],[0.3, '#FFFFFF'],[1, '#FFF4C6']]},plotBackgroundImage: null,height: 200};\n");
	webString->print("var credits = {enabled: false};\n");
	webString->print("var title = {text: 'RX/AUDIO VU Meter'};\n");
	webString->print("var pane = [{startAngle: -45,endAngle: 45,background: null,center: ['50%', '145%'],size: 300}];\n");
	webString->print("var yAxis = [{min: -40,max: 1,minorTickPosition: 'outside',tickPosition: 'outside',labels: {rotation: 'auto',distance: 20},\n");
	webString->print("plotBands: [{from: -10,to: 1,color: '#C02316',innerRadius: '100%',outerRadius: '105%'},{from: -20,to: -10,color: '#00C000',innerRadius: '100%',outerRadius: '105%'},{from: -30,to: -20,color: '#AFFF0F',innerRadius: '100%',outerRadius: '105%'},{from: -40,to: -30,color: '#C0A316',innerRadius: '100%',outerRadius: '105%'}],\n");
	webString->print("pane: 0,title: {text: '<span style=\"font-size:12px\">dBV</span>',y: -40}}];\n");
	webString->print("var plotOptions = {gauge: {dataLabels: {enabled: false},dial: {radius: '100%'}}};\n");
	webString->print("var series= [{data: [-40],yAxis: 0}];\n");
	webString->print("var json = {};\n json.chart = chart;\n json.credits = credits;\n json.title = title;\n json.pane = pane;\n json.yAxis = yAxis;\n json.plotOptions = plotOptions;\n json.series = series;\n");
	// Add some life
	webString->print("var chartFunction = function (chart) { \n"); // the chart may be destroyed
	webString->print("var Vrms=0;\nvar dBV=-40;\nvar active=0;var raw=\"\";var timeStamp;\n");
	webString->print("if (chart.series) {\n");
	webString->print("var left = chart.series[0].points[0];\n");
	webString->print("var host='ws://'+location.hostname+':81/ws'\n");
	webString->print("const ws = new WebSocket(host);\n");
	webString->print("ws.onopen = function() { console.log('Connection opened');};\n ws.onclose = function() { console.log('Connection closed');};\n");
	webString->print("ws.onmessage = function(event) {\n  console.log(event.data);\n");
	webString->print("const jsonR=JSON.parse(event.data);\n");
	webString->print("active=parseInt(jsonR.Active);\n");
	webString->print("Vrms=parseFloat(jsonR.mVrms)/1000;\n");
	webString->print("dBV=20.0*Math.log10(Vrms);\n");
	webString->print("if(dBV<-40) dBV=-40;\n");
	webString->print("raw=jsonR.RAW;\n");
	webString->print("timeStamp=Number(jsonR.timeStamp);\n");
	webString->print("if(active==1){\nleft.update(dBV,false);\nchart.redraw();\n");
	webString->print("var date=new Date(timeStamp * 1000);\n");
	webString->print("var head=date+\"[\"+Vrms.toFixed(3)+\"Vrms,\"+dBV.toFixed(1)+\"dBV]\\n\";\n");
	// webString->print("document.getElementById(\"raw_txt\").value+=head+atob(raw)+\"\\n\";\n");
	webString->print("var textArea=document.getElementById(\"raw_txt\");\n");
	webString->print("textArea.value+=head+atob(raw)+\"\\n\";\n");
	webString->print("textArea.scrollTop = textArea.scrollHeight;\n");
	webString->print("}\n");
	webString->print("}\n");
	webString->print("}};\n");
	webString->print("$('#vumeter').highcharts(json, chartFunction);\n");
	webString->print("});\n</script>\n");
	webString->print("</head><body>\n<table>\n");
	// webString->print("<tr><td><form accept-charset=\"UTF-8\" action=\"/test\" class=\"form-horizontal\" id=\"test_form\" method=\"post\">\n");
	// webString->print("<div style=\"margin-left: 20px;\"><input type='submit' class=\"btn btn-danger\" name=\"sendBeacon\" value='SEND BEACON'></div><br />\n");
	// webString->print("<div style=\"margin-left: 20px;\">TNC2 RAW: <input id=\"raw\" name=\"raw\" type=\"text\" size=\"60\" value=\"" + String(config.aprs_mycall) + ">APE32I,WIDE1-1:>Test Status\"/></div>\n");
	// webString->print("<div style=\"margin-left: 20px;\"><input type='submit' class=\"btn btn-primary\" name=\"sendRaw\" value='SEND RAW'></div> <br />\n");
	// webString->print("</form></td></tr>\n");
	// webString->print("<tr><td><hr width=\"80%\" /></td></tr>\n");
	webString->print("<tr><td><div id=\"vumeter\" style=\"width: 300px; height: 200px; margin: 10px;\"></div></td>\n");
	webString->print("<tr><td><div style=\"margin: 15px;\">Terminal<br /><textarea id=\"raw_txt\" name=\"raw_txt\" rows=\"50\" cols=\"80\" /></textarea></div></td></tr>\n");
	webString->print("</table>\n");

	webString->print("</body></html>\n");

	webString->addHeader("Test", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
}
// Remote version file published by copy_firmware.py, checked against the
// running VERSION/VERSION_BUILD to let the "about" page report new releases.
#define VERSION_CHECK_URL "https://raw.githubusercontent.com/marceloferreirachile/ESP32APRS_Audio/master/version.json"

void handle_check_version(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	HTTPClient http;
	http.begin(VERSION_CHECK_URL);
	http.setTimeout(5000);
	int httpCode = http.GET();

	if (httpCode != HTTP_CODE_OK)
	{
		http.end();
		char errBuf[100];
		snprintf(errBuf, sizeof(errBuf), "{\"error\":\"HTTP GET failed, code=%d\"}", httpCode);
		request->send(500, "application/json", errBuf);
		return;
	}

	String payload = http.getString();
	http.end();

	JsonDocument doc;
	DeserializationError err = deserializeJson(doc, payload);
	if (err)
	{
		request->send(500, "application/json", "{\"error\":\"Invalid version data\"}");
		return;
	}

	const char *latestVersion = doc["version"] | "";
	const char *latestBuild = doc["build"] | "";
	const char *latestDate = doc["date"] | "";
	const char *latestTag = doc["tag"] | "";
	const char *latestFilename = doc["filename"] | "";

	bool updateAvailable = (strcmp(latestVersion, VERSION) != 0) || (strcmp(latestBuild, VERSION_BUILD) != 0);

	char resp[400];
	snprintf(resp, sizeof(resp),
			 "{\"current_version\":\"%s\",\"current_build\":\"%s\","
			 "\"latest_version\":\"%s\",\"latest_build\":\"%s\",\"latest_date\":\"%s\","
			 "\"latest_tag\":\"%s\",\"latest_filename\":\"%s\",\"update_available\":%s}",
			 VERSION, VERSION_BUILD, latestVersion, latestBuild, latestDate,
			 latestTag, latestFilename, updateAvailable ? "true" : "false");
	request->send(200, "application/json", resp);
}

// LU6JMF - Marcelo Ferreira:
// Fase 6 (motor de templates): handle_about reescrito pra usar renderTemplate()
// em vez de ~430 linhas de webString->print()/snprintf espalhados. O template
// fica embutido no firmware (string normal, NAO no LittleFS - nao precisamos
// de espaco extra de filesystem pra isso, so estabilidade). Mesma saida HTML
// de antes, so que montada como 1 String e enviada com tamanho exato (elimina
// a classe de bug do buffer "chutado" que achamos na pagina IGATE).
static const char *ABOUT_TEMPLATE =
	"<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;\"><td width=\"49%\" style=\"border:unset;\">"
	"<table style=\"height:100%;\">"
	"<th colspan=\"2\"><span><b>System Information</b></span></th>\n"
	"<tr><td align=\"right\"><b>Hardware Version: </b></td><td align=\"left\">{{hwVersion}}</td></tr>"
	"<tr><td align=\"right\"><b>Firmware Version: </b></td><td align=\"left\"> V{{firmwareVersion}}</td></tr>\n"
	"<tr><td align=\"right\"><b>RF Module: </b></td><td align=\"left\"> {{rfModule}}</td></tr>\n"
	"<tr><td align=\"right\"><b>ESP32 Model: </b></td><td align=\"left\"> {{chipModel}}</td></tr>"
	"<tr><td align=\"right\"><b>Revision: </b></td><td align=\"left\"> {{chipRevision}}</td></tr>"
	"<tr><td align=\"right\"><b>Chip ID: </b></td><td align=\"left\"> {{chipId}}</td></tr>"
	"<tr><td align=\"right\"><b>Flash: </b></td><td align=\"left\">{{flashKB}} KByte</td></tr>"
	"<tr><td align=\"right\"><b>PSRAM: </b></td><td align=\"left\">{{psram}} KByte</td></tr>"
	"<tr><td align=\"right\"><b>FILE SYSTEM: </b></td><td align=\"left\">{{filesystem}} KByte</td></tr>"
	"</table>"
	"</td><td width=\"2%\" style=\"border:unset;\"></td>"
	"<td width=\"49%\" style=\"border:unset;\">"
	"<table style=\"height:100%;\">"
	"<th colspan=\"2\"><span><b>Developer/Support Information</b></span></th>\n"
	"<tr><td align=\"right\"><b>Author: </b></td><td align=\"left\">Marcelo Ferreira (fork/mods)</td></tr>"
	"<tr><td align=\"right\"><b>Callsign: </b></td><td align=\"left\">LU6JMF-10</td></tr>"
	"<tr><td align=\"right\"><b>Country: </b></td><td align=\"left\">Concepcion del Uruguay, Entre Rios, Argentina</td></tr>\n"
	"<tr><td align=\"right\"><b>Github: </b></td><td align=\"left\"><a href=\"https://github.com/marceloferreirachile/ESP32APRS_Audio\" target=\"_blank\">github.com/marceloferreirachile/ESP32APRS_Audio</a></td></tr>"
	"<tr><td align=\"right\"><b>Original: </b></td><td align=\"left\">Mr.Somkiat Nakhonthai (HS5TQA) - <a href=\"https://github.com/nakhonthai\" target=\"_blank\">github.com/nakhonthai</a></td></tr>\n"
	"<tr><td align=\"right\"><b>WhatsApp: </b></td><td align=\"left\"><a href=\"https://wa.me/5493442311119\" target=\"_blank\">wa.me/5493442311119</a></td></tr>"
	"<tr><td align=\"right\"><b>Donate: </b></td><td align=\"left\"><a href=\"https://www.paypal.me/MarceloFerreira673\" target=\"_blank\">paypal.me/MarceloFerreira673</a></td></tr>"
	"</table>"
	"</td></tr></table><br />"
	"<table>\n"
	"<tr><td align=\"right\"><b>Contributor: </b></td><td align=\"left\">LU6JMF Marcelo</td></tr>\n"
	"<tr><td align=\"right\"><b>Version: </b></td><td align=\"left\">v2.0-lu6jmf (custom mods on top of V1.8a)</td></tr>\n"
	"<tr><td align=\"right\" style=\"vertical-align:top;\"><b>Changes: </b></td><td align=\"left\" style=\"white-space:normal;\">\n"
	"See README / git log in the repository for full change history, or "
	"<a href=\"https://github.com/marceloferreirachile/ESP32APRS_Audio/releases\" target=\"_blank\">"
	"the Releases page</a> for per-version notes."
	"</td></tr>\n"
	"</table><br />\n"
	"<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;\"><td width=\"49%\" style=\"border:unset;\">"
	"<table>\n"
	"<th colspan=\"2\"><span><b>WiFi Status</b></span></th>\n"
	"<tr><td align=\"right\"><b>Mode:</b></td>\n"
	"<td align=\"left\">{{wifiMode}} ({{wifiProto}}){{wifiPowerSuffix}}</td></tr>\n"
	"<tr><td align=\"right\" width=\"30%\"><b>MAC:</b></td>\n<td align=\"left\">{{mac}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Channel:</b></td>\n<td align=\"left\">{{channel}}</td></tr>\n"
	"<tr><td align=\"right\"><b>TX Power:</b></td>\n<td align=\"left\">{{wifiPower}}</td></tr>\n"
	"<tr><td align=\"right\"><b>SSID:</b></td>\n<td align=\"left\">{{ssid}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Local IP:</b></td>\n<td align=\"left\">{{localIP}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Gateway IP:</b></td>\n<td align=\"left\">{{gatewayIP}}</td></tr>\n"
	"<tr><td align=\"right\"><b>DNS:</b></td>\n<td align=\"left\">{{dns}}</td></tr>\n"
	"</table>\n"
	"</td><td width=\"2%\" style=\"border:unset;\"></td>"
	"<td width=\"49%\" style=\"border:unset;\">"
	"<table>\n"
	"{{#if pppos}}"
	"<th colspan=\"2\"><span><b>PPPoS Status</b></span></th>\n"
	"<tr><td align=\"right\" width=\"30%\"><b>Manufacturer:</b></td>\n<td align=\"left\">{{pppos.manufacturer}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Model:</b></td>\n<td align=\"left\">{{pppos.model}}</td></tr>\n"
	"<tr><td align=\"right\"><b>IMEI:</b></td>\n<td align=\"left\">{{pppos.imei}}</td></tr>\n"
	"<tr><td align=\"right\"><b>IMSI:</b></td>\n<td align=\"left\">{{pppos.imsi}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Operator:</b></td>\n<td align=\"left\">{{pppos.oper}}</td></tr>\n"
	"<tr><td align=\"right\"><b>RSSI:</b></td>\n<td align=\"left\">{{pppos.rssi}} dBm</td></tr>\n"
	"<tr><td align=\"right\"><b>IP:</b></td>\n<td align=\"left\">{{pppos.ip}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Gateway:</b></td>\n<td align=\"left\">{{pppos.gateway}}</td></tr>\n"
	"{{/if}}"
	"</table>\n"
	"</td></tr></table><br />"
	"{{#if ota}}"
	"<form method='POST' action='#' enctype='multipart/form-data' id='upload_form' class=\"form-horizontal\">\n"
	"<table>"
	"<th colspan=\"2\"><span><b>Manual Firmware Update</b></span></th>\n"
	"<tr><td align=\"right\"><b>File:</b></td><td align=\"left\"><input id=\"file\" name=\"update\" type=\"file\" onchange='sub(this)' /></td></tr>\n"
	"<tr><td align=\"right\"><b>Progress:</b></td><td><div id='prgbar'><div id='bar' style=\"width: 0px;\"><label id='prg'></label></div></div></td></tr>\n"
	"<tr><td align=\"right\"><b>Support Firmware:</b></td><td align=\"left\"><a target=\"_download\" href=\"https://github.com/marceloferreirachile/ESP32APRS_Audio/releases\">https://github.com/marceloferreirachile/ESP32APRS_Audio/releases</a></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\"><div class=\"col-sm-3 col-xs-4\"><input type='submit' class=\"btn btn-danger\" id=\"update_sumbit\" value='Firmware UpLoad'></div></td></tr>\n"
	"</table><br />\n"
	"</form>\n"
	"<script>"
	"function sub(obj){"
	"var fileName = obj.value.split('\\\\');"
	"document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
	"};"
	"$('#upload_form').submit(function(e){"
	"e.preventDefault();"
	"var form = $('#upload_form')[0];"
	"var data = new FormData(form);"
	"document.getElementById('update_sumbit').disabled = true;"
	"$.ajax({"
	"url: '/update',"
	"type: 'POST',"
	"data: data,"
	"contentType: false,"
	"processData:false,"
	"xhr: function() {"
	"var xhr = new window.XMLHttpRequest();"
	"xhr.upload.addEventListener('progress', function(evt) {"
	"if (evt.lengthComputable) {"
	"var per = evt.loaded / evt.total;"
	"$('#prg').html(Math.round(per*100) + '%');"
	"$('#bar').css('width',Math.round(per*100) + '%');"
	"}"
	"}, false);"
	"return xhr;"
	"},"
	"success:function(d, s) {"
	"alert('Wait for system reboot 10sec');"
	"},"
	"error: function (a, b, c) {"
	"}"
	"});"
	"});"
	"</script>"
	// LU6JMF - Marcelo Ferreira: "Manual Filesystem Update (icons/data)" form
	// removido antes (postava pro /updatefs, que apaga o LittleFS inteiro
	// incluindo a config salva). O endpoint /updatefs continua existindo no
	// servidor, so o botao que convidava a usa-lo por engano foi tirado.
	"<table>"
	"<th colspan=\"2\"><span><b>OTA Online Firmware Update</b></span></th>\n"
	"<tr><td align=\"right\"><b>Firmware File:</b></td><td align=\"left\"><a id=\"fw_file_link\" href=\"{{otaUrl}}\" target=\"_blank\">{{otaFile}}</a></td></tr>\n"
	"<tr><td align=\"right\"><b>Current Version:</b></td><td align=\"left\">V{{firmwareVersion}}</td></tr>\n"
	"<tr><td align=\"right\"><b>Status:</b></td><td><span id='ota_prg'>Ready</span></td></tr>\n"
	"<tr><td colspan=\"2\" align=\"right\"><div class=\"col-sm-3 col-xs-4\"><input type='button' class=\"btn btn-danger\" id=\"check_ver_btn\" value='Check New Version'></div></td></tr>\n"
	"</table><br />\n"
	"<script>"
	"document.getElementById('check_ver_btn').addEventListener('click', function(){"
	"document.getElementById('check_ver_btn').disabled = true;"
	"document.getElementById('ota_prg').innerHTML = 'Checking...';"
	"$.ajax({"
	"url: '/check_version',"
	"type: 'GET',"
	"dataType: 'json',"
	"success: function(d) {"
	"document.getElementById('check_ver_btn').disabled = false;"
	"if (d.update_available) {"
	"var newUrl = 'https://github.com/marceloferreirachile/ESP32APRS_Audio/releases/download/' + d.latest_tag + '/' + d.latest_filename;"
	"var fwLink = document.getElementById('fw_file_link');"
	"fwLink.href = newUrl;"
	"fwLink.innerHTML = d.latest_filename;"
	"document.getElementById('ota_prg').innerHTML = 'New version available: V' + d.latest_version + d.latest_build + '. Download link above, install via Manual Firmware Update.';"
	"} else {"
	"document.getElementById('ota_prg').innerHTML = 'You are using the latest version.';"
	"}"
	"},"
	"error: function(a, b, c) {"
	"document.getElementById('check_ver_btn').disabled = false;"
	"document.getElementById('ota_prg').innerHTML = 'Error checking version: ' + c;"
	"}"
	"});"
	"});"
	"</script>"
	"{{/if}}"
	"</body></html>\n";

void handle_about(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	char strCID[50];
	uint64_t chipid = ESP.getEfuseMac();
	sprintf(strCID, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);

	JsonDocument data;
	char temp_buffer[512];

	char FirmwareOTA[50];
	char hwVersion[48];
	// Build a dot-free version string for use in firmware filenames
	char ver[20];
	sprintf(ver, "v%s-%s.bin", VERSION, VERSION_BUILD);
#if defined(TTGO_TWR)
	strcpy(hwVersion, "LiLyGo T-TWRPlus");
	sprintf(FirmwareOTA, "ESP32S3_TWR_%s", ver);
#elif defined(CONFIG_IDF_TARGET_ESP32)
	#ifdef SH1106
		strcpy(hwVersion, "ESP32-WROOM+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32_SH1106_%s", ver);
	#elif defined(SSD1306)
		strcpy(hwVersion, "ESP32-WROOM+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		strcpy(hwVersion, "ESP32-WROOM NO OTA,ESP32 DoIt DevKit");
		sprintf(FirmwareOTA, "ESP32_NOOTA_%s", ver);
	#else
		strcpy(hwVersion, "ESP32-WROOM,ESP32 DoIt DevKit");
		sprintf(FirmwareOTA, "ESP32_NODISP_%s", ver);
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
	#ifdef SH1106
		strcpy(hwVersion, "ESP32C3+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32C3_SH1106_%s", ver);
	#elif defined(SSD1306)
		strcpy(hwVersion, "ESP32C3+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32C3_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		strcpy(hwVersion, "ESP32C3 NO OTA,ESP32-C3 DIY");
		sprintf(FirmwareOTA, "ESP32C3_NOOTA_%s", ver);
	#else
		strcpy(hwVersion, "ESP32C3,ESP32-C3 DIY");
		sprintf(FirmwareOTA, "ESP32C3_NODISP_%s", ver);
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
	#ifdef SH1106
		strcpy(hwVersion, "ESP32C6+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32C6_SH1106_%s", ver);
	#elif defined(SSD1306)
		strcpy(hwVersion, "ESP32C6+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32C6_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		strcpy(hwVersion, "ESP32C6_NOOTA,ESP32-C6 DIY");
		sprintf(FirmwareOTA, "ESP32C6_NOOTA_%s", ver);
	#else
		strcpy(hwVersion, "ESP32C6,ESP32-C6 DIY");
		sprintf(FirmwareOTA, "ESP32C6_%s", ver);
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
	#ifdef ESP32S3_N16R8
		strcpy(hwVersion, "ESP32-S3_16MB,ESP32-S3-DevKit");
		sprintf(FirmwareOTA, "ESP32S3_N16R8_%s", ver);
	#elif defined(SH1106)
		strcpy(hwVersion, "ESP32-S3_8MB,SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32S3_SH1106_%s", ver);
	#elif defined(SSD1306)
		strcpy(hwVersion, "ESP32-S3_8MB,SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32S3_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		strcpy(hwVersion, "ESP32-S3 Super mini,No OTA");
		sprintf(FirmwareOTA, "ESP32S3_NOOTA_%s", ver);
	#endif
#else
	strcpy(hwVersion, "UNKNOWN,ESP32 DIY");
#endif

	data["hwVersion"] = hwVersion;
	snprintf(temp_buffer, sizeof(temp_buffer), "%s%s", VERSION, VERSION_BUILD);
	data["firmwareVersion"] = temp_buffer;
	data["rfModule"] = RF_TYPE[config.rf_type];
	data["chipModel"] = ESP.getChipModel();
	data["chipRevision"] = ESP.getChipRevision();
	data["chipId"] = strCID;
	data["flashKB"] = ESP.getFlashChipSize() / 1024;
	snprintf(temp_buffer, sizeof(temp_buffer), "%.1f/%.1f", (float)ESP.getFreePsram() / 1024, (float)ESP.getPsramSize() / 1024);
	data["psram"] = temp_buffer;
	snprintf(temp_buffer, sizeof(temp_buffer), "%.1f/%.1f", (float)LITTLEFS.usedBytes() / 1024, (float)LITTLEFS.totalBytes() / 1024);
	data["filesystem"] = temp_buffer;

	if (config.wifi_mode == WIFI_AP_FIX)
		data["wifiMode"] = "AP";
	else if (config.wifi_mode == WIFI_STA_FIX)
		data["wifiMode"] = "STA";
	else if (config.wifi_mode == WIFI_AP_STA_FIX)
		data["wifiMode"] = "AP+STA";
	else
		data["wifiMode"] = "OFF";

	uint8_t proto = 0;
	esp_wifi_get_protocol(WIFI_IF_STA, &proto);
	{
		String protoStr = "802.11";
		if (proto & WIFI_PROTOCOL_11B) protoStr += "b";
		if (proto & WIFI_PROTOCOL_11G) protoStr += "g";
		if (proto & WIFI_PROTOCOL_11N) protoStr += "n";
		if (proto & WIFI_PROTOCOL_LR) protoStr += "lr";
		data["wifiProto"] = protoStr;
	}
	data["wifiPowerSuffix"] = ""; // kept for template parity, unused (power shown in its own row below, same as before)

	wifi_power_t wpr = WiFi.getTxPower();
	const char *wifipower;
	if (wpr < 8) wifipower = "-1 dBm";
	else if (wpr < 21) wifipower = "2 dBm";
	else if (wpr < 29) wifipower = "5 dBm";
	else if (wpr < 35) wifipower = "8.5 dBm";
	else if (wpr < 45) wifipower = "11 dBm";
	else if (wpr < 53) wifipower = "13 dBm";
	else if (wpr < 61) wifipower = "15 dBm";
	else if (wpr < 69) wifipower = "17 dBm";
	else if (wpr < 75) wifipower = "18.5 dBm";
	else if (wpr < 77) wifipower = "19 dBm";
	else if (wpr < 80) wifipower = "19.5 dBm";
	else wifipower = "20 dBm";
	data["wifiPower"] = wifipower;

	data["mac"] = WiFi.macAddress();
	data["channel"] = WiFi.channel();
	data["ssid"] = WiFi.SSID();
	data["localIP"] = WiFi.localIP().toString();
	data["gatewayIP"] = WiFi.gatewayIP().toString();
	data["dns"] = WiFi.dnsIP().toString();

#ifdef PPPOS
	JsonObject pppos = data["pppos"].to<JsonObject>();
	pppos["manufacturer"] = pppStatus.manufacturer;
	pppos["model"] = pppStatus.model;
	pppos["imei"] = pppStatus.imei;
	pppos["imsi"] = pppStatus.imsi;
	pppos["oper"] = pppStatus.oper;
	pppos["rssi"] = pppStatus.rssi;
	pppos["ip"] = IPAddress(pppStatus.ip).toString();
	pppos["gateway"] = IPAddress(pppStatus.gateway).toString();
#endif

#ifndef NO_OTA
	data["ota"] = true;
	char ota_url_buf[200];
	snprintf(ota_url_buf, sizeof(ota_url_buf), "https://github.com/marceloferreirachile/ESP32APRS_Audio/releases/download/v%s-%s/%s", VERSION, VERSION_BUILD, FirmwareOTA);
	data["otaUrl"] = ota_url_buf;
	data["otaFile"] = FirmwareOTA;
#endif

	String html = renderTemplate(ABOUT_TEMPLATE, data.as<JsonVariant>());
	sendHtmlChunked(request, html, "About");
}

// -----------------------------------------------------------------------------
// LU6JMF - Marcelo Ferreira: PRIME account system, ported from TX-SCHEDULER-TEST.
// -----------------------------------------------------------------------------
void handle_prime_login(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	if (!request->hasParam("email", true) ||
		!request->hasParam("password", true))
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"missing_credentials\"}"
		);
		return;
	}

	String email = request->getParam("email", true)->value();
	String password = request->getParam("password", true)->value();

	email.trim();

	if (email.length() == 0 || password.length() == 0)
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"missing_credentials\"}"
		);
		return;
	}

	bool success = primeLogin(email, password);

	// Remove credentials from the temporary Strings as soon as possible.
	password = "";

	if (!success)
	{
		request->send(
			401,
			"application/json",
			"{\"success\":false,\"error\":\"login_failed\"}"
		);
		return;
	}

	request->send(
		200,
		"application/json",
		"{\"success\":true}"
	);
}

// -----------------------------------------------------------------------------
// LU6JMF - Marcelo Ferreira:
// PRIME account registration bridge.
// -----------------------------------------------------------------------------
void handle_prime_register(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	if (!request->hasParam("callsign", true) ||
		!request->hasParam("email", true) ||
		!request->hasParam("password", true))
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"missing_fields\"}"
		);
		return;
	}

	String callsign = request->getParam("callsign", true)->value();
	String email = request->getParam("email", true)->value();
	String password = request->getParam("password", true)->value();

	callsign.trim();
	callsign.toUpperCase();
	email.trim();
	email.toLowerCase();

	if (callsign.length() < 2 ||
		callsign.length() > 20 ||
		email.length() == 0 ||
		password.length() < 8)
	{
		password = "";

		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"invalid_fields\"}"
		);
		return;
	}

	bool success = primeRegister(callsign, email, password);

	password = "";

	if (!success)
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"register_failed\"}"
		);
		return;
	}

	request->send(
		200,
		"application/json",
		"{\"success\":true}"
	);
}

// -----------------------------------------------------------------------------
// LU6JMF - Marcelo Ferreira:
// PRIME password recovery bridge.
// -----------------------------------------------------------------------------
void handle_prime_recover(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	if (!request->hasParam("email", true))
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"missing_email\"}"
		);
		return;
	}

	String email = request->getParam("email", true)->value();

	email.trim();
	email.toLowerCase();

	if (email.length() == 0)
	{
		request->send(
			400,
			"application/json",
			"{\"success\":false,\"error\":\"missing_email\"}"
		);
		return;
	}

	bool success = primeRecover(email);

	if (!success)
	{
		request->send(
			502,
			"application/json",
			"{\"success\":false,\"error\":\"recovery_failed\"}"
		);
		return;
	}

	request->send(
		200,
		"application/json",
		"{\"success\":true}"
	);
}

// LU6JMF - Marcelo Ferreira: fase 6 - template embutido no firmware (ver about).
// A pagina tem 0 dados de config interpolados - so 2 booleans (autenticado,
// feature txdebug liberada) decidem qual bloco estatico aparece.
static const char *PRIME_TEMPLATE =
	"{{#if authenticated}}"
	"<div style='width:100%;max-width:520px;margin:20px auto;'>"
	"<table style='width:100%;table-layout:fixed;'>"
	"<tr><th>"
	"<span><b>AR-APRS PRIME</b></span>"
	"</th></tr>"
	"<tr><td style='text-align:center;padding:15px;white-space:normal;'>"
	"<h2 style='margin:5px 0 8px;'>PRIME</h2>"
	"<div style='font-weight:bold;'>Authenticated</div>"
	"</td></tr>"
	"<tr><td style='padding:15px;white-space:normal;'>"
	"<h3 style='text-align:center;margin:0 0 15px;'>PRIME RESOURCES</h3>"
	"{{#if txDebugAllowed}}"
	"<div style='border:1px solid #aaa;padding:15px;"
	"box-sizing:border-box;width:100%;white-space:normal;"
	"overflow-wrap:anywhere;text-align:center;'>"
	"<div style='font-size:16px;font-weight:bold;margin-bottom:8px;'>"
	"TX DEBUG"
	"</div>"
	"<div style='margin-bottom:15px;white-space:normal;'>"
	"Advanced scheduler and RF transmission diagnostics."
	"</div>"
	"<a href='/txdebug' target='_blank' rel='noopener' "
	"style='display:inline-block;border:1px solid #777;"
	"border-radius:3px;padding:6px 16px;"
	"background:#eee;color:#000;text-decoration:none;'>"
	"OPEN"
	"</a>"
	"</div>"
	"{{else}}"
	"<div style='text-align:center;white-space:normal;'>"
	"No PRIME resources available for this account."
	"</div>"
	"{{/if}}"
	"</td></tr>"
	"</table>"
	"</div>"
	// LU6JMF - Marcelo Ferreira:
	// Refresh the authenticated view every 5s so a plan/feature change on
	// the PRIME server reflects here without a manual reload - but only
	// while this tab is actually visible AND the user hasn't navigated
	// away from PRIME inside the app's own tab system (checked via the
	// marker text still being present in #contentmain).
	"<script>"
	"(function(){"
	"var primeTimer=null;"
	"function primeTick(){"
	"if(document.hidden){return;}"
	"var c=document.getElementById('contentmain');"
	"if(!c||c.innerHTML.indexOf('AR-APRS PRIME')===-1){"
	"clearInterval(primeTimer);primeTimer=null;return;"
	"}"
	"$('#contentmain').load('/prime');"
	"}"
	"primeTimer=setInterval(primeTick,5000);"
	"document.addEventListener('visibilitychange',function(){"
	"if(document.hidden&&primeTimer){clearInterval(primeTimer);primeTimer=null;}"
	"});"
	"})();"
	"</script>"
	"{{else}}"
	"<div style='width:100%;max-width:520px;margin:20px auto;'>"
	"<table style='width:100%;table-layout:fixed;'>"
	"<tr><th colspan='2'>"
	"<span><b>AR-APRS PRIME</b></span>"
	"</th></tr>"
	"<tr><td colspan='2' style='text-align:center;padding:15px;'>"
	"<h2 style='margin:5px 0 8px;'>PRIME</h2>"
	"<p style='margin:0;'>Exclusive features and services for AR-APRS users.</p>"
	"</td></tr>"
	"<tr><td colspan='2' style='padding:15px;'>"
	"<div id='primeLoginBox'>"
	"<form id='primeLoginForm' style='width:100%;box-sizing:border-box;'>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>E-mail</b></label>"
	"<input id='primeEmail' type='email' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 14px;padding:8px;' "
	"required>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>Password</b></label>"
	"<input id='primePassword' type='password' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 16px;padding:8px;' "
	"required>"
	"<button type='submit' class='btn btn-primary' style='display:block;margin:0 auto;'>"
	"ENTRAR</button>"
	"</form>"
	"<hr style='margin:20px 0;'>"
	"<div style='text-align:center;white-space:normal;'>"
	"<button type='button' class='btn btn-success' "
	"onclick='primeShowRegister()'>CRIAR CONTA</button>"
	"&nbsp;"
	"<button type='button' class='btn btn-secondary' "
	"onclick='primeRecover()'>RECUPERAR SENHA</button>"
	"</div>"
	"</div>"
	"<div id='primeRegisterBox' style='display:none;'>"
	"<h3 style='text-align:center;margin-top:0;'>Criar conta PRIME</h3>"
	"<form id='primeRegisterForm' style='width:100%;box-sizing:border-box;'>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>Indicativo</b></label>"
	"<input id='primeRegisterCallsign' type='text' maxlength='20' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 14px;padding:8px;' "
	"required>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>E-mail</b></label>"
	"<input id='primeRegisterEmail' type='email' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 14px;padding:8px;' "
	"required>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>Password</b></label>"
	"<input id='primeRegisterPassword' type='password' minlength='8' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 14px;padding:8px;' "
	"required>"
	"<label style='display:block;text-align:left;margin-bottom:4px;'><b>Confirm Password</b></label>"
	"<input id='primeRegisterPassword2' type='password' minlength='8' "
	"style='display:block;width:100%;box-sizing:border-box;margin:0 0 16px;padding:8px;' "
	"required>"
	"<div style='text-align:center;'>"
	"<button type='submit' class='btn btn-success'>CRIAR CONTA</button>"
	"&nbsp;"
	"<button type='button' class='btn btn-secondary' "
	"onclick='primeShowLogin()'>VOLTAR</button>"
	"</div>"
	"</form>"
	"</div>"
	"<div id='primeMessage' style='margin:15px auto 0;width:100%;max-width:100%;padding:0 8px;box-sizing:border-box;text-align:center;white-space:normal;overflow-wrap:anywhere;word-break:normal;'></div>"
	"</td></tr>"
	"</table>"
	"</div>"
	"<script>"
	"$('#primeLoginForm').on('submit',function(e){"
	"e.preventDefault();"
	"$('#primeMessage').html('Autenticando...');"
	"$.post('/prime/login',"
	"{email:$('#primeEmail').val(),password:$('#primePassword').val()},"
	"function(data){"
	"if(data.success){"
	"$('#contentmain').load('/prime');"
	"}else{"
	"$('#primeMessage').html('<b>Login não autorizado.</b>');"
	"}"
	"},'json').fail(function(xhr){"
	"if(xhr.status===401){"
	"$('#primeMessage').html('<b>E-mail ou senha incorretos.</b>');"
	"}else{"
	"$('#primeMessage').html('<b>Erro ao conectar ao PRIME.</b>');"
	"}"
	"});"
	"});"
	"function primeShowRegister(){"
	"$('#primeMessage').html('');"
	"$('#primeLoginBox').hide();"
	"$('#primeRegisterBox').show();"
	"var e=$('#primeEmail').val();"
	"if(e){$('#primeRegisterEmail').val(e);}"
	"}"
	"function primeShowLogin(){"
	"$('#primeMessage').html('');"
	"$('#primeRegisterBox').hide();"
	"$('#primeLoginBox').show();"
	"}"
	"$('#primeRegisterForm').on('submit',function(e){"
	"e.preventDefault();"
	"var callsign=$('#primeRegisterCallsign').val().trim().toUpperCase();"
	"var email=$('#primeRegisterEmail').val().trim();"
	"var p1=$('#primeRegisterPassword').val();"
	"var p2=$('#primeRegisterPassword2').val();"
	"if(p1.length<8){"
	"$('#primeMessage').html('<b>A senha deve ter pelo menos 8 caracteres.</b>');"
	"return;"
	"}"
	"if(p1!==p2){"
	"$('#primeMessage').html('<b>As senhas não coincidem.</b>');"
	"return;"
	"}"
	"$('#primeMessage').html('Criando conta PRIME...');"
	"$.post('/prime/register',"
	"{callsign:callsign,email:email,password:p1},"
	"function(data){"
	"if(data.success){"
	"$('#primeEmail').val(email);"
	"$('#primePassword').val('');"
	"$('#primeRegisterPassword').val('');"
	"$('#primeRegisterPassword2').val('');"
	"$('#primeRegisterBox').hide();"
	"$('#primeLoginBox').show();"
	"$('#primeMessage').html('<b>Conta PRIME criada com sucesso. Agora você pode entrar.</b>');"
	"}else{"
	"$('#primeMessage').html('<b>Não foi possível criar a conta.</b>');"
	"}"
	"},'json').fail(function(){"
	"$('#primeMessage').html('<b>Não foi possível criar a conta. Verifique os dados ou se a conta já existe.</b>');"
	"});"
	"});"
	"function primeRecover(){"
	"var email=$('#primeEmail').val().trim();"
	"if(!email){"
	"$('#primeMessage').html('<b>Digite seu e-mail para recuperar a senha.</b>');"
	"$('#primeEmail').focus();"
	"return;"
	"}"
	"$('#primeMessage').html('Solicitando recuperação de senha...');"
	"$.post('/prime/recover',"
	"{email:email},"
	"function(data){"
	"if(data.success){"
	"$('#primeMessage').html('<b>Se o e-mail estiver cadastrado, você receberá as instruções para redefinir sua senha.</b>');"
	"}else{"
	"$('#primeMessage').html('<b>Não foi possível solicitar a recuperação.</b>');"
	"}"
	"},'json').fail(function(){"
	"$('#primeMessage').html('<b>Não foi possível conectar ao serviço de recuperação.</b>');"
	"});"
	"}"
	"</script>"
	"{{/if}}";

void handle_prime(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	// LU6JMF - Marcelo Ferreira:
	// A valid PRIME token changes this page from the login form to the
	// authenticated PRIME resources view.
	JsonDocument data;
	bool authenticated = primeValidate();
	data["authenticated"] = authenticated;
	if (authenticated)
	{
		data["txDebugAllowed"] = primeFeature("txdebug");
	}

	String html_str = renderTemplate(PRIME_TEMPLATE, data.as<JsonVariant>());
	AsyncResponseStream *response = request->beginResponseStream("text/html", html_str.length() + 200);
	response->addHeader("Cache-Control", "no-store");
	response->print(html_str);
	request->send(response);
}

void handle_txdebug(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
    // LU6JMF - Marcelo Ferreira:
    // TX DEBUG is a PRIME resource. Hiding the link is not authorization;
    // direct access to /txdebug must pass the same PRIME checks.
    if (!request->authenticate(config.http_username, config.http_password))
    {
        return request->requestAuthentication();
    }

    if (!primeValidate() || !primeFeature("txdebug"))
    {
        request->send(
            403,
            "text/plain",
            "PRIME authorization required."
        );
        return;
    }

    bool schedulerBusy = false;
    bool pttSeen = false;
    bool txNow = false;
    bool pttOffPending = false;
    bool dcdBusy = false;

    uint32_t completed = 0;
    uint32_t rejected = 0;
    uint32_t lastCycle = 0;
    uint32_t lastWait = 0;
    uint32_t currentCycle = 0;
    uint32_t watchdogRecoveries = 0;

    lu6jmfTxDebugSnapshot(
        &schedulerBusy,
        &pttSeen,
        &txNow,
        &pttOffPending,
        &dcdBusy,
        &completed,
        &rejected,
        &lastCycle,
        &lastWait,
        &currentCycle,
        &watchdogRecoveries
    );

    // LU6JMF - Marcelo Ferreira
    // Copy the circular RAM log to a local snapshot before rendering.
    Lu6jmfTxDebugEvent curEvents[40]; // must match LU6JMF_TX_EVENT_COUNT in main.cpp
    uint8_t eventCount = lu6jmfTxDebugEvents(curEvents, 40);

    // LU6JMF (Oct/2026): crash black box - last TX events of the previous run,
    // only present when it ended in PANIC/WDT/BROWNOUT/SW reset.
    //
    // Restricted to the owner: this runs only after the PRIME "txdebug" gate
    // above, and additionally requires the PRIME-validated account callsign to
    // be the owner's. primeUserCallsign() is the value returned by the PRIME
    // server and is already in RAM (no extra network call per refresh).
    //
    // To open it to EVERYONE who already has the txdebug feature, set
    // LU6JMF_BLACKBOX_OWNER to "" (empty string): the callsign check is skipped.
    // 2.1.5: empty = visible to everyone who already has the /txdebug (PRIME) access,
    // not only to the owner. Set to "LU6JMF" to restrict it to the owner again.
    static const char *LU6JMF_BLACKBOX_OWNER = "";
    Lu6jmfTxDebugEvent prevEvents[16];
    char prevReason[24];
    prevReason[0] = '\0';
    uint8_t prevCount = 0;
    if (LU6JMF_BLACKBOX_OWNER[0] == '\0' ||
        primeUserCallsign().equalsIgnoreCase(LU6JMF_BLACKBOX_OWNER))
        prevCount = lu6jmfCrumbPrevEvents(prevEvents, 16, prevReason, sizeof(prevReason));

    // LU6JMF - Marcelo Ferreira
    // Phase 2J: local read-only snapshot of learned RF airtime.
    Lu6jmfTxTimingDebug timing[15];
    uint8_t timingCount = lu6jmfTxDebugTiming(timing, 15);

    // -------------------------------------------------------------------------
    // LU6JMF - Marcelo Ferreira
    // Phase 2K: local read-only snapshot of central queue health and
    // queue-to-RF-acceptance wait statistics.
    // -------------------------------------------------------------------------
    Lu6jmfTxQueueDebug queueDebug = {};
    lu6jmfTxDebugQueue(&queueDebug);

    Lu6jmfTxQueueWaitDebug queueWait[5]; // LU6JMF: 5 priorities incl. VERY_LOW
    uint8_t queueWaitCount = lu6jmfTxDebugQueueWait(queueWait, 5);

    // -------------------------------------------------------------------------
    // LU6JMF - Marcelo Ferreira
    // Phase 2L-A: persistent rejected-admission identity by producer.
    // -------------------------------------------------------------------------
    Lu6jmfTxQueueRejectDebug queueReject[15];
    uint8_t queueRejectCount =
        lu6jmfTxDebugQueueRejectedByJob(queueReject, 15);

    const char *queueHealth = "NORMAL";
    const char *queueHealthClass = "ok";

    if (queueDebug.current >= queueDebug.capacity)
    {
        queueHealth = "SATURATED";
        queueHealthClass = "bad";
    }
    else if (queueDebug.current >= 3)
    {
        queueHealth = "BUSY";
        queueHealthClass = "on";
    }

    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->addHeader("Cache-Control", "no-store");

    response->print(
        "<!DOCTYPE html>"
        "<html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta http-equiv='refresh' content='5'>"
        "<title>AR-APRS TX Debug</title>"
        "<style>"
        "body{font-family:Arial,sans-serif;background:#111;color:#eee;"
        "max-width:1180px;margin:24px auto;padding:15px}"
        "h2{margin-bottom:4px}"
        "h3{margin:0 0 4px 0}"
        "table{width:100%;border-collapse:collapse;margin-top:14px}"
        "td,th{padding:8px;border-bottom:1px solid #444}"
        "td:last-child{text-align:right;font-weight:bold}"
        "th{text-align:left;color:#aaa;font-size:12px}"
        ".on{color:#ffb000}.ok{color:#5fd35f}.bad{color:#ff5f5f}"
        ".log td{font-family:monospace;font-size:12px}"
        ".log td:last-child{text-align:right;font-weight:normal}"
        "small{color:#aaa}"
        ".grid{display:grid;"
        "grid-template-columns:repeat(2,minmax(0,1fr));"
        "gap:18px;margin-top:24px;align-items:start}"
        ".card{border:1px solid #333;border-radius:8px;"
        "padding:15px;background:#151515;min-width:0}"
        ".full{grid-column:1/-1}"
        "@media(max-width:820px){"
        ".grid{grid-template-columns:1fr}"
        ".full{grid-column:auto}"
        "body{margin:10px auto;padding:10px}"
        "}"
        "</style></head><body>"
        "<h2>AR-APRS TX DEBUG</h2>"
        "<small>Argentina APRS System | refresh 5s<br>LU6JMF &mdash; Marcelo Ferreira &middot; Creator</small>"
        "<div class='grid'>"
        "<div class='card'>"
        "<h3>RF SCHEDULER</h3>"
        "<table>"
    );

    response->printf(
        "<tr><td>RF Scheduler</td><td class='%s'>%s</td></tr>"
        "<tr><td>PTT observed</td><td class='%s'>%s</td></tr>"
        "<tr><td>TX state now</td><td class='%s'>%s</td></tr>"
        "<tr><td>Deferred PTT OFF</td><td class='%s'>%s</td></tr>"
        "<tr><td>DCD / Channel</td><td class='%s'>%s</td></tr>"
        "<tr><td>Completed TX</td><td>%lu</td></tr>"
        "<tr><td>AX.25 rejected</td><td>%lu</td></tr>"
        "<tr><td>Last full cycle</td><td>%lu ms</td></tr>"
        "<tr><td>Last wait to PTT</td><td>%lu ms</td></tr>"
        "<tr><td>Current cycle</td><td>%lu ms</td></tr>"
        "<tr><td>Watchdog recoveries</td><td class='%s'>%lu</td></tr>"
        "</table>",

        schedulerBusy ? "on" : "ok",
        schedulerBusy ? "BUSY" : "FREE",

        pttSeen ? "on" : "ok",
        pttSeen ? "YES" : "NO",

        txNow ? "on" : "ok",
        txNow ? "YES" : "NO",

        pttOffPending ? "on" : "ok",
        pttOffPending ? "YES" : "NO",

        dcdBusy ? "on" : "ok",
        dcdBusy ? "BUSY" : "FREE",

        (unsigned long)completed,
        (unsigned long)rejected,
        (unsigned long)lastCycle,
        (unsigned long)lastWait,
        (unsigned long)currentCycle,

        watchdogRecoveries ? "on" : "ok",
        (unsigned long)watchdogRecoveries
    );

    // -------------------------------------------------------------------------
    // LU6JMF - Marcelo Ferreira
    // Phase 2K: central queue health.
    //
    // NORMAL / BUSY / SATURATED is diagnostic presentation only.
    // It NEVER changes scheduler arbitration or queue admission.
    // -------------------------------------------------------------------------
    response->print(
        "</div>"
        "<div class='card'>"
        "<h3>QUEUE HEALTH</h3>"
        "<small>Central TX queue | RAM only | diagnostic</small>"
        "<table>"
    );

    response->printf(
        "<tr><td>Status</td><td class='%s'>%s</td></tr>"
        "<tr><td>Current</td><td>%u / %u</td></tr>"
        "<tr><td>Peak</td><td>%u / %u</td></tr>"
        "<tr><td>Saturation events</td><td>%lu</td></tr>"
        "<tr><td>Rejected admissions</td><td>%lu</td></tr>"
        "</table>",
        queueHealthClass,
        queueHealth,
        (unsigned int)queueDebug.current,
        (unsigned int)queueDebug.capacity,
        (unsigned int)queueDebug.peak,
        (unsigned int)queueDebug.capacity,
        (unsigned long)queueDebug.saturated,
        (unsigned long)queueDebug.rejected
    );

    // 2.1.5: system health (heap + lowest free stack ever seen per task). Read-only,
    // no allocation. Lets us watch the heap trend and catch a task about to overflow
    // its stack (Task WDT / random resets) without a USB cable.
    {
        const char *stackTasks[] = {"taskAPRS", "taskAPRSPoll", "taskNetwork", "async_tcp", "loopTask"};
        response->print(
            "</div>"
            "<div class='card'>"
            "<h3>SYSTEM HEALTH</h3>"
            "<small>Heap and lowest free stack ever seen (bytes) | diagnostic</small>"
            "<table>"
        );
        response->printf(
            "<tr><td>Free heap</td><td>%lu</td></tr>"
            "<tr><td>Lowest free heap</td><td>%lu</td></tr>"
            "<tr><td>Largest free block</td><td>%lu</td></tr>",
            (unsigned long)ESP.getFreeHeap(),
            (unsigned long)ESP.getMinFreeHeap(),
            (unsigned long)ESP.getMaxAllocHeap()
        );
        for (uint8_t t = 0; t < sizeof(stackTasks) / sizeof(stackTasks[0]); t++)
        {
            TaskHandle_t th = xTaskGetHandle(stackTasks[t]);
            if (th == NULL)
                continue;
            unsigned long freeStack = (unsigned long)uxTaskGetStackHighWaterMark(th);
            response->printf("<tr><td>Stack free: %s</td><td class='%s'>%lu</td></tr>",
                             stackTasks[t],
                             (freeStack < 512) ? "bad" : "ok",
                             freeStack);
        }
        response->print("</table>");
    }

    response->print(
        "</div>"
        "<div class='card'>"
        "<h3>QUEUE WAIT BY PRIORITY</h3>"
        "<small>Queue admission to RF ACCEPTED | RAM only | diagnostic</small>"
        "<table class='log'>"
        "<tr><th>Priority</th><th>Samples</th><th>Last</th>"
        "<th>Average</th><th style='text-align:right'>Max</th></tr>"
    );

    // Display highest priority first for easier operational reading.
    for (int p = (int)queueWaitCount - 1; p >= 0; p--)
    {
        const char *priorityName = "UNKNOWN";

        switch (queueWait[p].priority)
        {
            case LU6JMF_TXPRIO_VERY_LOW:
                priorityName = "VERY_LOW";
                break;

            case LU6JMF_TXPRIO_LOW:
                priorityName = "LOW";
                break;

            case LU6JMF_TXPRIO_NORMAL:
                priorityName = "NORMAL";
                break;

            case LU6JMF_TXPRIO_HIGH:
                priorityName = "HIGH";
                break;

            case LU6JMF_TXPRIO_CRITICAL:
                priorityName = "CRITICAL";
                break;
        }

        response->printf(
            "<tr><td>%s</td><td>%lu</td><td>%lu ms</td>"
            "<td>%lu ms</td><td>%lu ms</td></tr>",
            priorityName,
            (unsigned long)queueWait[p].samples,
            (unsigned long)queueWait[p].lastMs,
            (unsigned long)queueWait[p].avgMs,
            (unsigned long)queueWait[p].maxMs
        );
    }

    response->print("</table>");

    response->print("</div><div class='card'>");

    // -------------------------------------------------------------------------
    // LU6JMF - Marcelo Ferreira
    // Phase 2J: learned real RF airtime.
    // Observational only: these values never release or terminate RF TX.
    // -------------------------------------------------------------------------
    response->print(
        "<h3>LEARNED RF AIRTIME</h3>"
        "<small>PTT ON to real PTT OFF | RAM only | diagnostic</small>"
        "<table class='log'>"
        "<tr><th>Job</th><th>Samples</th><th>Last</th>"
        "<th>Average</th><th style='text-align:right'>Max</th></tr>"
    );

    for (uint8_t i = 0; i < timingCount; i++)
    {
        const char *timingJobName = "GENERIC";

        switch (timing[i].jobType)
        {
            case LU6JMF_TXJOB_ACK:
                timingJobName = "ACK";
                break;
            case LU6JMF_TXJOB_MESSAGE:
                timingJobName = "MESSAGE";
                break;
            case LU6JMF_TXJOB_MSG_RETRY:
                timingJobName = "MSG RETRY";
                break;
            case LU6JMF_TXJOB_DIGI:
                timingJobName = "DIGI";
                break;
            case LU6JMF_TXJOB_TRACKER:
                timingJobName = "TRACKER";
                break;
            case LU6JMF_TXJOB_IGATE:
                timingJobName = "IGATE";
                break;
            case LU6JMF_TXJOB_DIGI_POSITION:
                timingJobName = "DIGI POS";
                break;
            case LU6JMF_TXJOB_WX:
                timingJobName = "WX";
                break;
            case LU6JMF_TXJOB_TLM:
                timingJobName = "TLM";
                break;
            case LU6JMF_TXJOB_BULLETIN:
                timingJobName = "BULLETIN";
                break;
            case LU6JMF_TXJOB_NEWS:
                timingJobName = "NEWS";
                break;
            case LU6JMF_TXJOB_OBJECT:
                timingJobName = "OBJECT";
                break;
            case LU6JMF_TXJOB_INET2RF:
                timingJobName = "INET->RF";
                break;
            case LU6JMF_TXJOB_EXTERNAL:
                timingJobName = "EXTERNAL";
                break;
            default:
                timingJobName = "GENERIC";
                break;
        }

        response->printf(
            "<tr><td>%s</td><td>%lu</td><td>%lu ms</td>"
            "<td>%lu ms</td><td>%lu ms</td></tr>",
            timingJobName,
            (unsigned long)timing[i].samples,
            (unsigned long)timing[i].lastAirMs,
            (unsigned long)timing[i].avgAirMs,
            (unsigned long)timing[i].maxAirMs
        );
    }

    if (timingCount == 0)
    {
        response->print(
            "<tr><td colspan='5'>Waiting for first completed RF TX...</td></tr>"
        );
    }

    response->print("</table></div>");

    // -------------------------------------------------------------------------
    // LU6JMF - Marcelo Ferreira
    // Phase 2L-A: identify actual producers rejected by a full central queue.
    // Diagnostic only. No scheduler policy is changed here.
    // -------------------------------------------------------------------------
    response->print(
        "<div class='card full'>"
        "<h3>REJECTED ADMISSIONS BY PRODUCER</h3>"
        "<small>Central queue full | accumulated since boot</small>"
        "<table class='log'>"
        "<tr><th>Job</th><th style='text-align:right'>Rejected</th></tr>"
    );

    for (uint8_t i = 0; i < queueRejectCount; i++)
    {
        const char *rejectJobName = "GENERIC";

        switch (queueReject[i].jobType)
        {
            case LU6JMF_TXJOB_ACK:           rejectJobName = "ACK"; break;
            case LU6JMF_TXJOB_MESSAGE:       rejectJobName = "MESSAGE"; break;
            case LU6JMF_TXJOB_MSG_RETRY:     rejectJobName = "MSG RETRY"; break;
            case LU6JMF_TXJOB_DIGI:          rejectJobName = "DIGI"; break;
            case LU6JMF_TXJOB_TRACKER:       rejectJobName = "TRACKER"; break;
            case LU6JMF_TXJOB_IGATE:         rejectJobName = "IGATE"; break;
            case LU6JMF_TXJOB_DIGI_POSITION: rejectJobName = "DIGI POS"; break;
            case LU6JMF_TXJOB_WX:            rejectJobName = "WX"; break;
            case LU6JMF_TXJOB_TLM:           rejectJobName = "TLM"; break;
            case LU6JMF_TXJOB_BULLETIN:      rejectJobName = "BULLETIN"; break;
            case LU6JMF_TXJOB_NEWS:          rejectJobName = "NEWS"; break;
            case LU6JMF_TXJOB_OBJECT:        rejectJobName = "OBJECT"; break;
            case LU6JMF_TXJOB_INET2RF:       rejectJobName = "INET->RF"; break;
            case LU6JMF_TXJOB_EXTERNAL:      rejectJobName = "EXTERNAL"; break;
            default:                         rejectJobName = "GENERIC"; break;
        }

        response->printf(
            "<tr><td>%s</td><td>%lu</td></tr>",
            rejectJobName,
            (unsigned long)queueReject[i].rejected
        );
    }

    if (queueRejectCount == 0)
    {
        response->print(
            "<tr><td colspan='2'>No rejected admissions since boot.</td></tr>"
        );
    }

    response->print(
        "</table>"
        "</div>"
        "<div class='card full'>"
        "<h3>TX EVENT LOG</h3>"
        "<small>RAM only | newest first | cleared on reboot</small>"
        "<table class='log'>"
        "<tr><th>Uptime</th><th>Event</th><th>Job</th><th style='text-align:right'>Value</th></tr>"
    );

    // Pass 0 = current run, pass 1 = black box of the previous (abnormal) run.
    for (uint8_t pass = 0; pass < 2; pass++)
    {
    const Lu6jmfTxDebugEvent *events = (pass == 0) ? curEvents : prevEvents;
    uint8_t srcCount = (pass == 0) ? eventCount : prevCount;

    if (pass == 1)
    {
        if (prevCount == 0)
            break;

        response->printf(
            "</table>"
            "</div>"
            // 2.1.5: always visible when the previous run ended abnormally (no hidden
            // click-to-reveal any more). Only emitted for the owner (gate above).
            "<div class='card full' id='lu6bb'>"
            "<h3>BEFORE LAST RESET (%s)</h3>"
            "<small>last TX events of the previous run | newest first | "
            "uptime is from that previous boot</small>"
            "<table class='log'>"
            "<tr><th>Uptime</th><th>Event</th><th>Job</th>"
            "<th style='text-align:right'>Value</th></tr>",
            prevReason
        );
    }

    for (uint8_t i = 0; i < srcCount; i++)
    {
        const char *eventName = "UNKNOWN";
        const char *jobName = "GENERIC";
        char value[64]; // LU6JMF: 64 for the WATCHDOG STATE decode below
        value[0] = '\0';

        // LU6JMF - Marcelo Ferreira
        // Human-readable producer identity for RF Scheduler diagnostics.
        switch (events[i].jobType)
        {
            case LU6JMF_TXJOB_ACK:           jobName = "ACK"; break;
            case LU6JMF_TXJOB_MESSAGE:       jobName = "MESSAGE"; break;
            case LU6JMF_TXJOB_MSG_RETRY:     jobName = "MSG RETRY"; break;
            case LU6JMF_TXJOB_DIGI:          jobName = "DIGI"; break;
            case LU6JMF_TXJOB_TRACKER:       jobName = "TRACKER"; break;
            case LU6JMF_TXJOB_IGATE:         jobName = "IGATE"; break;
            case LU6JMF_TXJOB_DIGI_POSITION: jobName = "DIGI POS"; break;
            case LU6JMF_TXJOB_WX:            jobName = "WX"; break;
            case LU6JMF_TXJOB_TLM:           jobName = "TLM"; break;
            case LU6JMF_TXJOB_BULLETIN:      jobName = "BULLETIN"; break;
            case LU6JMF_TXJOB_NEWS:          jobName = "NEWS"; break;
            case LU6JMF_TXJOB_OBJECT:        jobName = "OBJECT"; break;
            case LU6JMF_TXJOB_INET2RF:       jobName = "INET->RF"; break;
            case LU6JMF_TXJOB_EXTERNAL:      jobName = "EXTERNAL"; break;
            default:                         jobName = "GENERIC"; break;
        }

        switch (events[i].code)
        {
            case 1:
                eventName = "RF ACCEPTED";
                snprintf(value, sizeof(value), "slot %lu",
                         (unsigned long)events[i].value);
                break;

            case 2:
                eventName = "PTT ON";
                snprintf(value, sizeof(value), "%lu ms",
                         (unsigned long)events[i].value);
                break;

            case 3:
                eventName = "PTT OFF TASK";
                break;

            case 4:
                eventName = "PTT OFF DONE";
                break;

            case 5:
                eventName = "RF RELEASE";
                snprintf(value, sizeof(value), "%lu ms",
                         (unsigned long)events[i].value);
                break;

            case 6:
                eventName = "AX25 REJECT";
                snprintf(value, sizeof(value), "count %lu",
                         (unsigned long)events[i].value);
                break;

            // LU6JMF - Marcelo Ferreira
            // Phase 2G: queue admission and queue saturation diagnostics.
            // value carries Lu6jmfTxPriority for these event types.
            case 7:
            case 8:
            {
                eventName = (events[i].code == 7)
                                ? "QUEUE PUSH"
                                : "QUEUE FULL";

                const char *priorityName = "UNKNOWN";

                switch (events[i].value)
                {
                    case LU6JMF_TXPRIO_VERY_LOW:
                        priorityName = "VERY_LOW";
                        break;

                    case LU6JMF_TXPRIO_LOW:
                        priorityName = "LOW";
                        break;

                    case LU6JMF_TXPRIO_NORMAL:
                        priorityName = "NORMAL";
                        break;

                    case LU6JMF_TXPRIO_HIGH:
                        priorityName = "HIGH";
                        break;

                    case LU6JMF_TXPRIO_CRITICAL:
                        priorityName = "CRITICAL";
                        break;
                }

                snprintf(value, sizeof(value), "%s", priorityName);
                break;
            }

            case 9:
                eventName = "ACK WINDOW START";
                snprintf(value, sizeof(value), "%lu ms",
                         (unsigned long)events[i].value);
                break;

            case 10:
                eventName = "ACK WINDOW END";
                break;

            case 11:
            {
                eventName = "QUEUE PREEMPT";

                uint8_t displacedJob =
                    (uint8_t)(events[i].value & 0xFF);

                uint8_t displacedPriority =
                    (uint8_t)((events[i].value >> 8) & 0xFF);

                const char *displacedJobName = "GENERIC";
                const char *displacedPriorityName = "UNKNOWN";

                switch (displacedJob)
                {
                    case LU6JMF_TXJOB_ACK:           displacedJobName = "ACK"; break;
                    case LU6JMF_TXJOB_MESSAGE:       displacedJobName = "MESSAGE"; break;
                    case LU6JMF_TXJOB_MSG_RETRY:     displacedJobName = "MSG RETRY"; break;
                    case LU6JMF_TXJOB_DIGI:          displacedJobName = "DIGI"; break;
                    case LU6JMF_TXJOB_TRACKER:       displacedJobName = "TRACKER"; break;
                    case LU6JMF_TXJOB_IGATE:         displacedJobName = "IGATE"; break;
                    case LU6JMF_TXJOB_DIGI_POSITION: displacedJobName = "DIGI POS"; break;
                    case LU6JMF_TXJOB_WX:            displacedJobName = "WX"; break;
                    case LU6JMF_TXJOB_TLM:           displacedJobName = "TLM"; break;
                    case LU6JMF_TXJOB_BULLETIN:      displacedJobName = "BULLETIN"; break;
                    case LU6JMF_TXJOB_NEWS:          displacedJobName = "NEWS"; break;
                    case LU6JMF_TXJOB_OBJECT:        displacedJobName = "OBJECT"; break;
                    case LU6JMF_TXJOB_INET2RF:       displacedJobName = "INET->RF"; break;
                    case LU6JMF_TXJOB_EXTERNAL:      displacedJobName = "EXTERNAL"; break;
                    default:                         displacedJobName = "GENERIC"; break;
                }

                switch (displacedPriority)
                {
                    case LU6JMF_TXPRIO_VERY_LOW:
                        displacedPriorityName = "VERY_LOW";
                        break;

                    case LU6JMF_TXPRIO_LOW:
                        displacedPriorityName = "LOW";
                        break;

                    case LU6JMF_TXPRIO_NORMAL:
                        displacedPriorityName = "NORMAL";
                        break;

                    case LU6JMF_TXPRIO_HIGH:
                        displacedPriorityName = "HIGH";
                        break;

                    case LU6JMF_TXPRIO_CRITICAL:
                        displacedPriorityName = "CRITICAL";
                        break;
                }

                snprintf(value,
                         sizeof(value),
                         "%s %s",
                         displacedJobName,
                         displacedPriorityName);
                break;
            }

            // LU6JMF - Marcelo Ferreira (Oct/2026)
            // TX Scheduler watchdog recovery: value = ms the RF lifecycle was stuck.
            case 12:
                eventName = "WATCHDOG RECOVER";
                snprintf(value, sizeof(value), "%lu ms",
                         (unsigned long)events[i].value);
                break;

            // LU6JMF - Marcelo Ferreira (Oct/2026)
            // Modem/AX.25 state snapshot taken when the watchdog fired.
            // seen=0 -> stalled BEFORE PTT ON (AX.25 never keyed);
            // seen=1 -> keyed, but the end-of-TX signal never arrived.
            case 13:
            {
                eventName = "WATCHDOG STATE";
                uint32_t v = events[i].value;
                uint8_t st = v & 0x03;
                const char *stName = (st == 0) ? "OFF"
                                     : (st == 1) ? "WAITING"
                                                 : "TRANSMITTING";
                snprintf(value, sizeof(value),
                         "init=%s full=%u seen=%u keyed=%u rx=%u h%u t%u",
                         stName,
                         (unsigned)((v >> 2) & 1),
                         (unsigned)((v >> 3) & 1),
                         (unsigned)((v >> 4) & 1),
                         (unsigned)((v >> 5) & 1),
                         (unsigned)((v >> 8) & 0xFF),
                         (unsigned)((v >> 16) & 0xFF));
                break;
            }
        }

        uint32_t seconds = events[i].timestamp / 1000;
        uint32_t milliseconds = events[i].timestamp % 1000;

        response->printf(
            "<tr><td>%lu.%03lu s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
            (unsigned long)seconds,
            (unsigned long)milliseconds,
            eventName,
            jobName,
            value
        );
    }

    if (pass == 0 && eventCount == 0)
    {
        response->print(
            "<tr><td>-</td><td>No TX events yet</td><td></td><td></td></tr>"
        );
    }
    } // end pass loop

    response->print(
        "</table>"
        "</div>"
        "</div>"
        "<p><small>"
        "Scheduler completion is released only after real PTT OFF."
        "</small></p>"
    );

    // 2.1.5: the black box card is now always shown (see above); the old
    // click-on-"OFF." reveal script was removed.
    response->print("</body></html>");

    request->send(response);
}

void handle_gnss(AsyncWebServerRequest *request)
{
	// 2.1.5: feed the task watchdog - this handler runs on the async_tcp task itself
	esp_task_wdt_reset();
	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 20000);
	if (!webString)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	webString->print("<html>\n<head>\n");
	webString->print("<script src=\"https://apps.bdimg.com/libs/jquery/2.1.4/jquery.min.js\"></script>\n");
	webString->print("<script src=\"https://code.highcharts.com/highcharts.js\"></script>\n");
	webString->print("<script src=\"https://code.highcharts.com/highcharts-more.js\"></script>\n");
	webString->print("<script language=\"JavaScript\">");

	// Add some life
	webString->print("function gnss() { \n"); // the chart may be destroyed
	webString->print("var raw=\"\";var timeStamp;\n");
	webString->print("var host='ws://'+location.hostname+':81/ws_gnss'\n");
	webString->print("const ws = new WebSocket(host);\n");
	webString->print("ws.onopen = function() { console.log('Connection opened');};\n ws.onclose = function() { console.log('Connection closed');};\n");
	webString->print("ws.onmessage = function(event) {\n  console.log(event.data);\n");
	webString->print("const jsonR=JSON.parse(event.data);\n");
	webString->print("document.getElementById(\"en\").innerHTML=parseInt(jsonR.en);\n");
	webString->print("document.getElementById(\"lat\").innerHTML=parseFloat(jsonR.lat);\n");
	webString->print("document.getElementById(\"lng\").innerHTML=parseFloat(jsonR.lng);\n");
	webString->print("document.getElementById(\"alt\").innerHTML=parseFloat(jsonR.alt);\n");
	webString->print("document.getElementById(\"spd\").innerHTML=parseFloat(jsonR.spd);\n");
	webString->print("document.getElementById(\"csd\").innerHTML=parseFloat(jsonR.csd);\n");
	webString->print("document.getElementById(\"hdop\").innerHTML=parseFloat(jsonR.hdop);\n");
	webString->print("document.getElementById(\"sat\").innerHTML=parseInt(jsonR.sat);\n");
	webString->print("document.getElementById(\"time\").innerHTML=parseInt(jsonR.time);\n");
	webString->print("raw=jsonR.RAW;\n");
	webString->print("timeStamp=Number(jsonR.timeStamp);\n");
	webString->print("var textArea=document.getElementById(\"raw_txt\");\n");
	webString->print("textArea.value+=atob(raw)+\"\\n\";\n");
	webString->print("textArea.scrollTop = textArea.scrollHeight;\n");
	webString->print("}\n");
	webString->print("};\n</script>\n");
	webString->print("</head><body onload=\"gnss()\">\n");

	webString->print("<table width=\"200\" border=\"1\">");
	webString->print("<th colspan=\"2\" style=\"background-color: #00BCD4;\"><span><b>GNSS Information</b></span></th>\n");
	// webString->print("<tr><th width=\"200\"><span><b>Name</b></span></th><th><span><b>Information</b></span></th></tr>");

	{
		char *temp_en = allocateStringMemory(512);
		if (temp_en)
		{
			snprintf(temp_en, 512, "<tr><td align=\"right\"><b>Enable: </b></td><td align=\"left\"> <label id=\"en\">%d</label></td></tr>", config.gnss_enable);
			webString->print(temp_en);
			free(temp_en);
		}
	}

	{
		char *temp_lat = allocateStringMemory(512);
		if (temp_lat)
		{
			snprintf(temp_lat, 512, "<tr><td align=\"right\"><b>Latitude: </b></td><td align=\"left\"> <label id=\"lat\">%.5f</label></td></tr>", gps.location.lat());
			webString->print(temp_lat);
			free(temp_lat);
		}
	}

	{
		char *temp_lng = allocateStringMemory(512);
		if (temp_lng)
		{
			snprintf(temp_lng, 512, "<tr><td align=\"right\"><b>Longitude: </b></td><td align=\"left\"> <label id=\"lng\">%.5f</label></td></tr>", gps.location.lng());
			webString->print(temp_lng);
			free(temp_lng);
		}
	}

	{
		char *temp_alt = allocateStringMemory(512);
		if (temp_alt)
		{
			snprintf(temp_alt, 512, "<tr><td align=\"right\"><b>Altitude: </b></td><td align=\"left\"> <label id=\"alt\">%.2f</label> m.</td></tr>", gps.altitude.meters());
			webString->print(temp_alt);
			free(temp_alt);
		}
	}

	{
		char *temp_spd = allocateStringMemory(512);
		if (temp_spd)
		{
			snprintf(temp_spd, 512, "<tr><td align=\"right\"><b>Speed: </b></td><td align=\"left\"> <label id=\"spd\">%.2f</label> km/h</td></tr>", gps.speed.kmph());
			webString->print(temp_spd);
			free(temp_spd);
		}
	}

	{
		char *temp_csd = allocateStringMemory(512);
		if (temp_csd)
		{
			snprintf(temp_csd, 512, "<tr><td align=\"right\"><b>Course: </b></td><td align=\"left\"> <label id=\"csd\">%.1f</label></td></tr>", gps.course.deg());
			webString->print(temp_csd);
			free(temp_csd);
		}
	}

	{
		char *temp_hdop = allocateStringMemory(512);
		if (temp_hdop)
		{
			snprintf(temp_hdop, 512, "<tr><td align=\"right\"><b>HDOP: </b></td><td align=\"left\"> <label id=\"hdop\">%.2f</label> </td></tr>", gps.hdop.hdop());
			webString->print(temp_hdop);
			free(temp_hdop);
		}
	}

	{
		char *temp_sat = allocateStringMemory(512);
		if (temp_sat)
		{
			snprintf(temp_sat, 512, "<tr><td align=\"right\"><b>SAT: </b></td><td align=\"left\"> <label id=\"sat\">%d</label> </td></tr>", gps.satellites.value());
			webString->print(temp_sat);
			free(temp_sat);
		}
	}

	{
		char *temp_time = allocateStringMemory(512);
		if (temp_time)
		{
			snprintf(temp_time, 512, "<tr><td align=\"right\"><b>Time: </b></td><td align=\"left\"> <label id=\"time\">%d</label> </td></tr>", gps.time.value());
			webString->print(temp_time);
			free(temp_time);
		}
	}

	webString->print("</table><table>");
	webString->print("<tr><td><b>Terminal:</b><br /><textarea id=\"raw_txt\" name=\"raw_txt\" rows=\"30\" cols=\"80\" /></textarea></td></tr>\n");
	webString->print("</table>\n");

	webString->print("</body></html>\n");

	webString->addHeader("GNSS", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
}

void handle_default()
{
	defaultSetting = true;
	defaultConfig();
	defaultSetting = false;
}

// LU6JMF - Marcelo Ferreira:
// The route's main callback (see async_server.on("/upload", ...)) used to
// unconditionally reply "File uploaded successfully!" no matter what
// happened in here - so a failed mkdir/open/write (silently swallowed,
// nothing here ever checked a return value) still showed "Upload Success"
// to the user. Track the real outcome here and let the route callback
// report it truthfully.
bool lastUploadOk = false;
String lastUploadError = "";

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    // LU6JMF - Marcelo Ferreira:
    // The generic uploader saves to LittleFS root, but the firmware reads
    // the packed symbol icons from /symbols/icons.dat (see ICON_PACK_PATH).
    // Special-case this one filename so it lands where the firmware expects
    // it - this lets the icon set be updated in place via the normal
    // single-file upload, without touching /updatefs (which reflashes the
    // whole partition and wipes the saved config).
    String targetPath = "/" + filename;
    if (filename == "icons.dat")
    {
        targetPath = "/symbols/icons.dat";
    }
    // LU6JMF - Marcelo Ferreira:
    // Fase 6 - main_shell.html originally went under /templates/, but this
    // vendored LittleFS_esp32 port doesn't reliably create/use a runtime
    // mkdir()'d subfolder on this hardware (write silently produced nothing,
    // reproduced live: "Upload Success" but file never actually landed).
    // Keep it flat at root, same as style.css - avoids the whole issue.
    else if (filename == "main_shell.html")
    {
        targetPath = "/main_shell.html";
    }
    else if (filename == "style.css")
    {
        targetPath = "/style.css";
    }
    if (!index) {
        lastUploadOk = false;
        lastUploadError = "";
        // Open the file in write mode on the first chunk
        request->_tempFile = LITTLEFS.open(targetPath, "w");
        if (!request->_tempFile) {
            lastUploadError = "Could not open " + targetPath + " for writing (missing parent dir?)";
            log_e("%s", lastUploadError.c_str());
            return;
        }
    }
    if (!request->_tempFile) {
        return; // open already failed on chunk 0 - nothing more we can do with later chunks
    }
    if (len < (LITTLEFS.totalBytes()-LITTLEFS.usedBytes())) {
        // Write the data chunk to the file
        size_t written = request->_tempFile.write(data, len);
        if (written != len) {
            lastUploadError = "Write failed at offset " + String(index) + " (" + String(written) + "/" + String(len) + " bytes)";
            log_e("%s", lastUploadError.c_str());
            request->_tempFile.close();
            LITTLEFS.remove(targetPath);
            return;
        }
    }else{
		// Not enough space to write the file
		lastUploadError = "Not enough space to upload the file";
		request->_tempFile.close();
		LITTLEFS.remove(targetPath); // Remove the incomplete file
		return;
	}
    if (final) {
        // Close the file on the last chunk, then verify it actually landed
        // with the expected content before calling it a success.
        request->_tempFile.close();
        File check = LITTLEFS.open(targetPath, "r");
        if (check && check.size() > 0) {
            lastUploadOk = true;
        } else if (lastUploadError == "") {
            lastUploadError = "File missing or empty after write: " + targetPath;
        }
        if (check) check.close();
    }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{

	if (type == WS_EVT_CONNECT)
	{

		log_d("Websocket client connection received");
	}
	else if (type == WS_EVT_DISCONNECT)
	{

		log_d("Client disconnected");
	}
}

// void handle_vpn_request(AsyncWebServerRequest *request) {
//     HTTPClient http;

//     String url = "http://vpn.nakhonthai.net:82/wg/create";

//     String mac = WiFi.macAddress();
//     mac.replace(":", "");

//     String payload = "{\"name\":\"" + mac + "\"}";

//     http.begin(url);
//     http.addHeader("Content-Type", "application/json");

//     int httpCode = http.POST(payload);

//     if (httpCode > 0) {
//         String res = http.getString();
//         request->send(200, "application/json", res);
//     } else {
//         request->send(500, "text/plain", "Error contacting VPN server");
//     }

//     http.end();
// }

bool webServiceBegin = true;
void webService()
{
	if (webServiceBegin)
	{
		webServiceBegin = false;
	}
	else
	{
		return;
	}
	ws.onEvent(onWsEvent);

	// web client handlers
	async_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
					{ setMainPage(request); });
	async_server.on("/symbol", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_symbol(request); });
	// async_server.on("/symbol2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
	// 				{ handle_symbol2(request); });
	async_server.on("/logout", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_logout(request); });
	async_server.on("/radio", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_radio(request); });
	// v2.1.3.1-audiotest: TX audio calibration tone, see handle_calibrate_tone() above.
	async_server.on("/calibrate_tone", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_calibrate_tone(request); });
	async_server.on("/vpn", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_vpn(request); });
#ifdef MQTT
	async_server.on("/mqtt", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mqtt(request); });
#endif
	async_server.on("/msg", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_msg(request); });
	async_server.on("/object", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_object(request); });
	async_server.on("/mod", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mod(request); });
	async_server.on("/mod2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mod2(request); });
	async_server.on("/default", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_default(); });
	async_server.on("/igate", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_igate(request); });
	async_server.on("/igate2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_igate2(request); });
	async_server.on("/digi", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_digi(request); });
	async_server.on("/tracker", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_tracker(request); });
	async_server.on("/wx", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_wx(request); });
	async_server.on("/wx2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_wx2(request); });
	async_server.on("/tlm", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_tlm(request); });
	async_server.on("/sensor", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_sensor(request); });
	async_server.on("/sensor2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_sensor2(request); });
	async_server.on("/system", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_system(request); });
	async_server.on("/wireless", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_wireless(request); });
	async_server.on("/tnc2", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_test(request); });
	async_server.on("/gnss", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_gnss(request); });
	// async_server.on("/realtime", HTTP_GET, [](AsyncWebServerRequest *request)
	// 				{ handle_realtime(request); });
	async_server.on("/about", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_about(request); });
	async_server.on("/prime", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_prime(request); });

	// LU6JMF - Marcelo Ferreira: PRIME authentication endpoint.
	async_server.on("/prime/login", HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_prime_login(request); });

	// LU6JMF - Marcelo Ferreira: PRIME account creation endpoint.
	async_server.on("/prime/register", HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_prime_register(request); });

	// LU6JMF - Marcelo Ferreira: PRIME password recovery endpoint.
	async_server.on("/prime/recover", HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_prime_recover(request); });
	async_server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_dashboard(request); });

	// LU6JMF - Marcelo Ferreira: temporary TX Scheduler diagnostics.
	async_server.on("/txdebug", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_txdebug(request); });
	async_server.on("/sidebarInfo", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_sidebar(request); });
	async_server.on("/sysinfo", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_sysinfo(request); });
	// async_server.on("/lastHeard", HTTP_GET, [](AsyncWebServerRequest *request)
	// 				{ handle_lastHeard(request); });
	async_server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_css(request); });
	async_server.on("/jquery-3.7.1.js", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_jquery(request); });
	async_server.on("/storage", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_storage(request); });
	async_server.on("/download", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_download(request); });
	async_server.on("/delete", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_delete(request); });
	async_server.on("/format", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_format(request); });
	// Route to handle the file upload
    async_server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *request) {
        // LU6JMF - Marcelo Ferreira: report what actually happened in
        // handleUpload() instead of always claiming success (see there).
        if (lastUploadOk) {
            request->send(200, "text/plain", "File uploaded successfully!");
        } else {
            request->send(500, "text/plain", "Upload failed: " + lastUploadError);
        }
    }, handleUpload); // Pass the handleUpload function as the upload handler

	// async_server.on("/api/vpnreq", HTTP_GET, handle_vpn_request);
	async_server.on(
		"/update", HTTP_POST, [](AsyncWebServerRequest *request)
		{
  		bool espShouldReboot = !Update.hasError();
  		AsyncWebServerResponse *response = request->beginResponse(200, "text/html", espShouldReboot ? "<h1><strong>Update DONE</strong></h1><br><a href='/'>Return Home</a>" : "<h1><strong>Update FAILED</strong></h1><br><a href='/updt'>Retry?</a>");
  		response->addHeader("Connection", "close");
  		request->send(response); },
		[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
		{
			if (!index)
			{
				log_d("Update Start: %s\n", filename.c_str());
				if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000))
				{
					Update.printError(Serial);
				}
				else
				{
					adcEn = -1;
					dacEn = -1;
					delay(500);
					// disableLoopWDT();
					// disableCore0WDT();
					// disableCore1WDT();
					//  vTaskSuspend(taskAPRSPollHandle);
					//  vTaskSuspend(taskAPRSHandle);
					//  vTaskSuspend(taskSensorHandle);
					//  vTaskSuspend(taskSerialHandle);
					//  vTaskSuspend(taskGPSHandle);
					//  vTaskSuspend(taskSensorHandle);
				}
			}
			if (!Update.hasError())
			{
				if (Update.write(data, len) != len)
				{
					Update.printError(Serial);
				}
			}
			if (final)
			{
				if (Update.end(true))
				{
					log_d("Update Success: %uByte\n", index + len);
					delay(1000);
					esp_restart();
				}
				else
				{
					Update.printError(Serial);
				}
			}
		});

	async_server.on(
		"/updatefs", HTTP_POST, [](AsyncWebServerRequest *request)
		{
  		bool espShouldReboot = !Update.hasError();
  		AsyncWebServerResponse *response = request->beginResponse(200, "text/html", espShouldReboot ? "<h1><strong>Filesystem Update DONE</strong></h1><br><a href='/'>Return Home</a>" : "<h1><strong>Filesystem Update FAILED</strong></h1><br><a href='/about'>Retry?</a>");
  		response->addHeader("Connection", "close");
  		request->send(response); },
		[](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
		{
			if (!index)
			{
				log_d("Filesystem Update Start: %s\n", filename.c_str());
				// custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
				// Same Update lib as firmware OTA, but targets the "spiffs" (LittleFS) partition
				// instead of the app partition - lets us push data/symbols/icons/* over the web
				// when there is no physical USB access to the board.
				if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS))
				{
					Update.printError(Serial);
				}
			}
			if (!Update.hasError())
			{
				if (Update.write(data, len) != len)
				{
					Update.printError(Serial);
				}
			}
			if (final)
			{
				if (Update.end(true))
				{
					log_d("Filesystem Update Success: %uByte\n", index + len);
					// custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
					// The published littlefs.bin only carries icons/fsversion.txt (no
					// personal config), so this raw overwrite of the whole partition
					// would otherwise wipe the user's WiFi/APRS config too - stranding
					// remote users with no USB access in AP mode. `config` is the live
					// in-RAM copy (loaded at boot, untouched by the flash write below),
					// so remount the fresh filesystem and write it straight back out.
					LITTLEFS.end();
					if (LITTLEFS.begin(false))
					{
						if (saveConfiguration("/default.cfg", config))
						{
							log_d("Config restored to new filesystem after update\n");
						}
						else
						{
							log_e("Failed to restore config after filesystem update\n");
						}
						LITTLEFS.end();
					}
					else
					{
						log_e("Could not remount LITTLEFS to restore config after update\n");
					}
					delay(1000);
					esp_restart();
				}
				else
				{
					Update.printError(Serial);
				}
			}
		});

async_server.on("/check_version", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_check_version(request); });		

	lastheard_events.onConnect([](AsyncEventSourceClient *client)
							   {
    if(client->lastId()){
      log_d("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
    }
    // send event with message "hello!", id current millis
    // and set reconnect delay to 1 second
    //String html = event_lastHeard(true);
    //client->send(html.c_str(), "lastHeard", time(NULL), 5000); 
});
	async_server.addHandler(&lastheard_events);

	message_events.onConnect([](AsyncEventSourceClient *client)
							 {
    if(client->lastId()){
      log_d("Client reconnected! Last message ID that it got is: %u\n", client->lastId());
    }
    // send event with message "hello!", id current millis
    // and set reconnect delay to 1 second
	String html = event_chatMessage(true);
    client->send(html.c_str(), "chatMsg", time(NULL), 5000); });
	async_server.addHandler(&message_events);

	async_server.onNotFound(notFound);
	async_server.begin();
	async_websocket.addHandler(&ws);
	async_websocket.addHandler(&ws_gnss);
	async_websocket.begin();
}
