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
#include "webservice.h"
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

void setMainPage(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

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
	if (strlen(config.host_name) > 0)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "<title>%s</title>\n", config.host_name);
		webString->print(temp_buffer);
	}
	else
	{
		webString->print("<title>ESP32APRS_Audio</title>\n");
	}

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
	webString->print("} else if (tabName == 'DIGI') {\n");
	webString->print("$(\"#contentmain\").load(\"/digi\");\n");
	webString->print("} else if (tabName == 'TRACKER') {\n");
	webString->print("$(\"#contentmain\").load(\"/tracker\");\n");
	webString->print("} else if (tabName == 'WX') {\n");
	webString->print("$(\"#contentmain\").load(\"/wx\");\n");
	webString->print("} else if (tabName == 'TLM') {\n");
	webString->print("$(\"#contentmain\").load(\"/tlm\");\n");
	webString->print("} else if (tabName == 'SENSOR') {\n");
	webString->print("$(\"#contentmain\").load(\"/sensor\");\n");
	webString->print("} else if (tabName == 'VPN') {\n");
	webString->print("$(\"#contentmain\").load(\"/vpn\");\n");
#ifdef MQTT
	webString->print("} else if (tabName == 'MQTT') {\n");
	webString->print("$(\"#contentmain\").load(\"/mqtt\");\n");
#endif
	webString->print("} else if (tabName == 'MSG') {\n");
	webString->print("$(\"#contentmain\").load(\"/msg\");\n");
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
	if (strlen(config.host_name) > 0)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "<h1>%s</h1>\n", config.host_name);
		webString->print(temp_buffer);
	}
	else
	{
		webString->print("<h1>ESP32APRS_Audio</h1>\n");
	}
	webString->print("<div style=\"font-size: 8px; text-align: right; padding-right: 8px;\"><a href=\"/logout\">[LOG OUT]</a></div>\n");
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
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'VPN')\">VPN</button>\n");
#ifdef MQTT
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MQTT')\">MQTT</button>\n");
#endif
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MSG')\">MSG</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'WiFi')\">WiFi</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'MOD')\">MOD</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'IO')\">I/O</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'System')\">System</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'File')\">File</button>\n");
	webString->print("<button class=\"nav-tabs\" onclick=\"selectTab(event, 'About')\">About</button>\n");
	webString->print("</ul>\n");
	webString->print("</div>\n");
	webString->print("</div>\n");
	webString->print("\n");

	webString->print("<div class=\"contentwide\" id=\"contentmain\"  style=\"font-size: 2pt;\">\n");
	webString->print("\n");
	webString->print("</div>\n");
	webString->print("<br />\n");
	webString->print("<div class=\"footer\">\n");
	webString->print("ESP32APRS_Audio Web Configuration<br />Copy right ©2023.\n");
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
	const char *css = ".container{width:900px;text-align:left;margin:auto;border-radius:10px 10px 10px 10px;-moz-border-radius:10px 10px 10px 10px;-webkit-border-radius:10px 10px 10px 10px;-khtml-border-radius:10px 10px 10px 10px;-ms-border-radius:10px 10px 10px 10px;box-shadow:3px 3px 3px #707070;background:#fff;border-color: #2194ec;padding: 0px;border-width: 5px;border-style:solid;}body,font{font:12px verdana,arial,sans-serif;color:#fff}.header{background:#2194ec;text-decoration:none;color:#fff;font-family:verdana,arial,sans-serif;text-align:left;padding:5px 0;border-radius:10px 10px 0 0;-moz-border-radius:10px 10px 0 0;-webkit-border-radius:10px 10px 0 0;-khtml-border-radius:10px 10px 0 0;-ms-border-radius:10px 10px 0 0}.content{margin:0 0 0 166px;padding:1px 5px 5px;color:#000;background:#fff;text-align:center;font-size: 8pt;}.contentwide{padding:50px 5px 5px;color:#000;background:#fff;text-align:center}.contentwide h2{color:#000;font:1em verdana,arial,sans-serif;text-align:center;font-weight:700;padding:0;margin:0;font-size: 12pt;}.footer{background:#2194ec;text-decoration:none;color:#fff;font-family:verdana,arial,sans-serif;font-size:9px;text-align:center;padding:10px 0;border-radius:0 0 10px 10px;-moz-border-radius:0 0 10px 10px;-webkit-border-radius:0 0 10px 10px;-khtml-border-radius:0 0 10px 10px;-ms-border-radius:0 0 10px 10px;clear:both}#tail{height:450px;width:805px;overflow-y:scroll;overflow-x:scroll;color:#0f0;background:#000}table{vertical-align:middle;text-align:center;empty-cells:show;padding-left:3;padding-right:3;padding-top:3;padding-bottom:3;border-collapse:collapse;border-color:#0f07f2;border-style:solid;border-spacing:0px;border-width:3px;text-decoration:none;color:#fff;background:#000;font-family:verdana,arial,sans-serif;font-size : 12px;width:100%;white-space:nowrap}table th{cursor: pointer;user-select: none;font-size: 10pt;font-family:lucidia console,Monaco,monospace;text-shadow:1px 1px #0e038c;text-decoration:none;background:#0525f7;border:1px solid silver}table tr:nth-child(even){background:#f7f7f7}table tr:nth-child(odd){background:#eeeeee}table td{color:#000;font-family:lucidia console,Monaco,monospace;text-decoration:none;border:1px solid #010369}body{background:#edf0f5;color:#000}a{text-decoration:none}a:link,a:visited{text-decoration:none;color:#0000e0;font-weight:400}th:last-child a.tooltip:hover span{left:auto;right:0}ul{padding:5px;margin:10px 0;list-style:none;float:left}ul li{float:left;display:inline;margin:0 10px}ul li a{text-decoration:none;float:left;color:#999;cursor:pointer;font:900 14px/22px arial,Helvetica,sans-serif}ul li a span{margin:0 10px 0 -10px;padding:1px 8px 5px 18px;position:relative;float:left}h1{text-shadow:2px 2px #303030;text-align:center}.toggle{position:absolute;margin-left:-9999px;visibility:hidden}.toggle+label{display:block;position:relative;cursor:pointer;outline:none}input.toggle-round-flat+label{padding:1px;width:33px;height:18px;background-color:#ddd;border-radius:10px;transition:background .4s}input.toggle-round-flat+label:before,input.toggle-round-flat+label:after{display:block;position:absolute;}input.toggle-round-flat+label:before{top:1px;left:1px;bottom:1px;right:1px;background-color:#fff;border-radius:10px;transition:background .4s}input.toggle-round-flat+label:after{top:2px;left:2px;bottom:2px;width:16px;background-color:#ddd;border-radius:12px;transition:margin .4s,background .4s}input.toggle-round-flat:checked+label{background-color:#dd4b39}input.toggle-round-flat:checked+label:after{margin-left:14px;background-color:#dd4b39}@-moz-document url-prefix(){select,input{margin:0;padding:0;border-width:1px;font:12px verdana,arial,sans-serif}input[type=button],button,input[type=submit]{padding:0 3px;border-radius:3px 3px 3px 3px;-moz-border-radius:3px 3px 3px 3px}}.nice-select.small,.nice-select-dropdown li.option{height:24px!important;min-height:24px!important;line-height:24px!important}.nice-select.small ul li:nth-of-type(2){clear:both}.nav{margin-bottom:0;padding-left:10;list-style:none}.nav>li{position:relative;display:block}.nav>li>a{position:relative;display:block;padding:5px 10px}.nav>li>a:hover,.nav>li>a:focus{text-decoration:none;background-color:#eee}.nav>li.disabled>a{color:#999}.nav>li.disabled>a:hover,.nav>li.disabled>a:focus{color:#999;text-decoration:none;background-color:initial;cursor:not-allowed}.nav .open>a,.nav .open>a:hover,.nav .open>a:focus{background-color:#eee;border-color:#428bca}.nav .nav-divider{height:1px;margin:9px 0;overflow:hidden;background-color:#e5e5e5}.nav>li>a>img{max-width:none}.nav-tabs{border-bottom:1px solid #ddd}.nav-tabs>li{float:left;margin-bottom:-1px}.nav-tabs>li>a{margin-right:0;line-height:1.42857143;border:1px solid #ddd;border-radius:10px 10px 0 0}.nav-tabs>li>a:hover{border-color:#eee #eee #ddd}.nav-tabs>button{margin-right:0;line-height:1.42857143;border:2px solid #ddd;border-radius:10px 10px 0 0}.nav-tabs>button:hover{background-color:#25bbfc;border-color:#428bca;color:#eaf2f9;border-bottom-color:transparent;}.nav-tabs>button.active,.nav-tabs>button.active:hover,.nav-tabs>button.active:focus{color:#f7fdfd;background-color:#1aae0d;border:1px solid #ddd;border-bottom-color:transparent;cursor:default}.nav-tabs>li.active>a,.nav-tabs>li.active>a:hover,.nav-tabs>li.active>a:focus{color:#428bca;background-color:#e5e5e5;border:1px solid #ddd;border-bottom-color:transparent;cursor:default}.nav-tabs.nav-justified{width:100%;border-bottom:0}.nav-tabs.nav-justified>li{float:none}.nav-tabs.nav-justified>li>a{text-align:center;margin-bottom:5px}.nav-tabs.nav-justified>.dropdown .dropdown-menu{top:auto;left:auto}.nav-status{float:left;margin:0;padding:3px;width:160px;font-weight:400;min-height:600}#bar,#prgbar {background-color: #f1f1f1;border-radius: 14px}#bar {background-color: #3498db;width: 0%;height: 14px}.switch{position:relative;display:inline-block;width:34px;height:16px}.switch input{opacity:0;width:0;height:0}.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#f55959;-webkit-transition:.4s;transition:.4s}.slider:before{position:absolute;content:\"\";height:12px;width:12px;left:2px;bottom:2px;background-color:#fff;-webkit-transition:.4s;transition:.4s}input:checked+.slider{background-color:#5ca30a}input:focus+.slider{box-shadow:0 0 1px #5ca30a}input:checked+.slider:before{-webkit-transform:translateX(16px);-ms-transform:translateX(16px);transform:translateX(16px)}.slider.round{border-radius:34px}.slider.round:before{border-radius:50%}.button{border:1px solid #06c;background-color:#09c;color:#fff;padding:5px 10px;border-radius: 3px}.button:hover{border:1px solid #09c;background-color:#0ac;color:#fff}.button:disabled,button[disabled]{border:1px solid #999;background-color:#ccc;color:#666}.arrow {margin-left: 5px;font-size: 12px;}\n";
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

void handle_dashboard(AsyncWebServerRequest *request)
{
	char temp_buffer[200];
	// if (!request->authenticate(config.http_username, config.http_password))
	// {
	// 	return request->requestAuthentication();
	// }
	StandByTick = millis() + (config.pwr_stanby_delay * 1000);

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 14000);
	if (!webString)
	{
		return; // Memory allocation failed
	}

	webString->print("<script type=\"text/javascript\">\n");
	webString->print("function reloadSysInfo() {\n");
	webString->print("$(\"#sysInfo\").load(\"/sysinfo\", function () { setTimeout(reloadSysInfo, 60000) });\n");
	webString->print("}\n");
	webString->print("setTimeout(reloadSysInfo(), 100);\n");
	webString->print("function reloadSidebarInfo() {\n");
	webString->print("$(\"#sidebarInfo\").load(\"/sidebarInfo\", function () { setTimeout(reloadSidebarInfo, 10000) });\n");
	webString->print("}\n");
	webString->print("setTimeout(reloadSidebarInfo, 1000);\n");
	webString->print("$(window).trigger('resize');\n");

	webString->print("</script>\n");

	//webString->print("<script>\n");
	// webString->print("let aprsData = [\n");
	// webString->print("  { time: \"21:54:23\", icon: \"91-1.png\", callsign: \"HS5TQA-7\", path: \"RF: WIDE1-1\", dx: 0.0, packet: 2, audio: -19.6 },\n");
	// webString->print("  { time: \"22:13:33\", icon: \"66-1.png\", callsign: \"HS5TQA-3\", path: \"RF: DIRECT\", dx: null, packet: 9, audio: -14.1 }\n");
	// webString->print("];\n\n");
	// webString->print("const tableBody = document.querySelector(\"#aprsTable tbody\");\n");
	// webString->print("let sortDirection = {};\n");
	// webString->print("let currentSortKey = null;\n\n");
	// webString->print("function renderTable(data) {\n");
	// webString->print("    tableBody.innerHTML = \"\";\n");
	// webString->print("    data.forEach(row => {\n");
	// webString->print("        const tr = document.createElement(\"tr\");\n");
	// webString->print("        tr.innerHTML = `\n");
	// webString->print("            <td>${row.time}</td>\n");
	// webString->print("            <td><img src=\\\"http://aprs.nakhonthai.net/symbols/icons/${row.icon}\\\"></td>\n");
	// webString->print("            <td>${row.callsign}</td>\n");
	// webString->print("            <td>${row.path}</td>\n");
	// webString->print("            <td>${row.dx !== null ? row.dx + \\\" km\\\" : \\\"-\\\"}</td>\n");
	// webString->print("            <td>${row.packet}</td>\n");
	// webString->print("            <td style=\\\"color:green;\\\">${row.audio !== null ? row.audio + \\\" dBV\\\" : \\\"-\\\"}</td>\n");
	// webString->print("        `;\n");
	// webString->print("        tableBody.appendChild(tr);\n");
	// webString->print("    });\n");
	// webString->print("}\n\n");
	// webString->print("function clearArrows() {\n");
	// webString->print("    document.querySelectorAll(\\\".arrow\\\").forEach(a => a.textContent = \\\"\\\");\n");
	// webString->print("}\n\n");
	// webString->print("document.querySelectorAll(\\\"#aprsTable th[data-sort]\\\")\n");
	// webString->print(".forEach(header => {\n\n");
	// webString->print("    header.addEventListener(\\\"click\\\", () => {\n\n");
	// webString->print("        const key = header.dataset.sort;\n\n");
	// webString->print("        sortDirection[key] = !sortDirection[key];\n");
	// webString->print("        currentSortKey = key;\n\n");
	// webString->print("        aprsData.sort((a, b) => {\n\n");
	// webString->print("            let valA = a[key];\n");
	// webString->print("            let valB = b[key];\n\n");
	// webString->print("            if (key === \\\"time\\\") {\n");
	// webString->print("                valA = new Date(\\\"1970-01-01T\\\" + valA);\n");
	// webString->print("                valB = new Date(\\\"1970-01-01T\\\" + valB);\n");
	// webString->print("            }\n\n");
	// webString->print("            if (valA === null) return 1;\n");
	// webString->print("            if (valB === null) return -1;\n\n");
	// webString->print("            if (valA < valB) return sortDirection[key] ? -1 : 1;\n");
	// webString->print("            if (valA > valB) return sortDirection[key] ? 1 : -1;\n");
	// webString->print("            return 0;\n");
	// webString->print("        });\n\n");
	// webString->print("        clearArrows();\n\n");
	// webString->print("        const arrowSpan = header.querySelector(\\\".arrow\\\");\n");
	// webString->print("        arrowSpan.textContent = sortDirection[key] ? \\\"▲\\\" : \\\"▼\\\";\n\n");
	// webString->print("        renderTable(aprsData);\n");
	// webString->print("    });\n\n");
	// webString->print("});\n\n");
	// webString->print("renderTable(aprsData);\n");
	// webString->print("</script>\n");


	webString->print("<div id=\"sysInfo\">\n");
	webString->print("</div>\n");

	webString->print("<br />\n");
	webString->print("<div class=\"nav-status\">\n");
	webString->print("<div id=\"sidebarInfo\">\n");
	webString->print("</div>\n");
	webString->print("<br />\n");

	webString->print("<table>\n");
	webString->print("<tr>\n");
	webString->print("<th colspan=\"2\">Radio Info</th>\n");
	webString->print("</tr>\n");
	if (config.rf_en)
	{
		webString->print("<tr>\n");
		webString->print("<td>Freq.TX</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%.4f MHz</td>\n", config.freq_tx);
		webString->print(temp_buffer);

		webString->print("</tr>\n");
		webString->print("<tr>\n");
		webString->print("<td>Freq.RX</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%.4f MHz</td>\n", config.freq_rx);
		webString->print(temp_buffer);

		webString->print("</tr>\n");
		webString->print("<tr>\n");
		webString->print("<td>TX PWR</td>\n");
		if (config.rf_power)
			webString->print("<td>HIGH</td>\n");
		else
			webString->print("<td>LOW</td>\n");
		webString->print("</tr>\n");
	}
	webString->print("<tr>\n");
	webString->print("<td>MODEM</td>\n");

	
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", MODEM_TYPE[config.modem_type]);
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>FX.25</td>\n");

	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", FX25_MODE[config.fx25_mode]);
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("</table>\n");
	webString->print("\n");
	if (config.igate_en)
	{
		webString->print("<br />\n");
		webString->print("<table>\n");
		webString->print("<tr>\n");
		webString->print("<th colspan=\"2\">APRS-IS SERVER</th>\n");
		webString->print("</tr>\n");
		webString->print("<tr>\n");
		webString->print("<td>HOST</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", config.aprs_host);
		webString->print(temp_buffer);

		webString->print("</tr>\n");
		webString->print("<tr>\n");
		webString->print("<td>PORT</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%d</td>\n", config.aprs_port);
		webString->print(temp_buffer);

		webString->print("</tr>\n");
		webString->print("</table>\n");
	}
	webString->print("<br />\n");
	webString->print("<table>\n");
	webString->print("<tr>\n");
	webString->print("<th colspan=\"2\">WiFi</th>\n");
	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>MODE</td>\n");
	const char *strWiFiMode = "OFF";
	if (config.wifi_mode == WIFI_STA_FIX)
	{
		strWiFiMode = "STA";
	}
	else if (config.wifi_mode == WIFI_AP_FIX)
	{
		strWiFiMode = "AP";
	}
	else if (config.wifi_mode == WIFI_AP_STA_FIX)
	{
		strWiFiMode = "AP+STA";
	}

	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", strWiFiMode);
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>SSID</td>\n");

	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", WiFi.SSID().c_str());
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>RSSI</td>\n");
	if (WiFi.isConnected())
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%d dBm</td>\n", WiFi.RSSI());
		webString->print(temp_buffer);
	}
	else
		webString->print("<td style=\"background:#606060; color:#b0b0b0;\" aria-disabled=\"true\">Disconnect</td>\n");
	webString->print("</tr>\n");
	webString->print("</table>\n");
	webString->print("<br />\n");
#ifdef BLUETOOTH
	webString->print("<table>\n");
	webString->print("<tr>\n");

	webString->print("<th colspan=\"2\">Bluetooth</th>\n");
	webString->print("</tr>\n");
	webString->print("<td>Master</td>\n");
	if (config.bt_master)
		webString->print("<td style=\"background:#0b0; color:#030; width:50%;\">Enabled</td>\n");
	else
		webString->print("<td style=\"background:#606060; color:#b0b0b0;\" aria-disabled=\"true\">Disabled</td>\n");
	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>NAME</td>\n");

	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", config.bt_name);
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("<tr>\n");
	webString->print("<td>MODE</td>\n");
	const char *btMode = "";
	if (config.bt_mode == 1)
	{
		btMode = "TNC2";
	}
	else if (config.bt_mode == 2)
	{
		btMode = "KISS";
	}
	else
	{
		btMode = "NONE";
	}

	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%s</td>\n", btMode);
	webString->print(temp_buffer);

	webString->print("</tr>\n");
	webString->print("<tr>\n");
	webString->print("</table>\n");
#endif
	webString->print("</div>\n");

	webString->print("</div>\n");
	webString->print("\n");
	webString->print("<div class=\"content\">\n");
	webString->print("<div id=\"lastHeard\">\n");
	//String lastHeardString = event_lastHeard(true);
	//webString->print(lastHeardString.c_str());
	//lastHeardString.clear();
	webString->print("<table id=\"aprsTable\">\n<thread>\n");
	webString->print("<th colspan=\"7\" style=\"background-color: #070ac2;\">LAST HEARD <a href=\"/tnc2\" target=\"_tnc2\" style=\"color: yellow;font-size:8pt\">[RAW]</a></th>\n");
	webString->print("<tr>\n");
	webString->print("<th data-sort=\"time\" style=\"min-width:10ch\"><span><b>Time (");
	if (config.timeZone >= 0)
		webString->print("+");
	// else
	//	webString->print("-");

	if (config.timeZone == (int)config.timeZone)
	{
		sprintf(temp_buffer, "%d", (int)config.timeZone);
		webString->print(temp_buffer);
		webString->print(")</b></span><span class=\"arrow\"></span></th>\n");
	}
	else
	{
		sprintf(temp_buffer, "%.1f", config.timeZone);
		webString->print(temp_buffer);
		webString->print(")</b></span><span class=\"arrow\"></span></th>\n");
	}
	webString->print("<th style=\"min-width:16px\">ICON</th>\n");
	webString->print("<th data-sort=\"callsign\" style=\"min-width:10ch\">Callsign<span class=\"arrow\"></span></th>\n");
	webString->print("<th>VIA LAST PATH</th>\n");
	webString->print("<th data-sort=\"dx\" style=\"min-width:5ch\">DX<span class=\"arrow\"></span></th>\n");
	webString->print("<th data-sort=\"packet\" style=\"min-width:5ch\">PACKET<span class=\"arrow\"></span></th>\n");
	webString->print("<th data-sort=\"audio\" style=\"min-width:5ch\">AUDIO<span class=\"arrow\"></span></th>\n");
	webString->print("</tr></thread>\n<tbody id=\"aprsTableBody\"></tbody>\n");
	webString->print("</table>\n");
	webString->print("</div>\n");

	webString->addHeader("dashboard", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
	lastHeardTimeout = millis() + 500;
	lastHeard_Flag = true;
}

void handle_sidebar(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *html = request->beginResponseStream("text/html", 10000);
	if (!html)
	{
		return; // Memory allocation failed
	}

	html->print("<table style=\"background:white;border-collapse: unset;\">\n");
	html->print("<tr>\n");
	html->print("<th colspan=\"2\">Modes Enabled</th>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
	if (config.igate_en)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">IGATE</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">IGATE</th>\n");

	if (config.digi_en)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">DIGI</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">DIGI</th>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
	if (config.wx_en)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">WX</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">WX</th>\n");
	if (config.trk_en)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">TRACKER</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\">TRACKER</th>\n");
	html->print("</tr>\n");
	html->print("</table>\n");
	html->print("<br />\n");
	html->print("<table style=\"background:white;border-collapse: unset;\">\n");
	html->print("<tr>\n");
	html->print("<th colspan=\"2\">Network Status</th>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
	if (aprsClient.connected() == true)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">APRS-IS</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">APRS-IS</th>\n");
	if (wireguard_active() == true)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">VPN</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">VPN</th>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
// html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">4G LTE</th>\n");
#ifdef PPPOS
	if (PPP.connected())
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">PPPoS</th>\n");
	else
#endif
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">PPPoS</th>\n");
#ifdef MQTT
	if (clientMQTT.connected())
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">MQTT</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">MQTT</th>\n";);
#endif
	if (config.fx25_mode > 0)
		html->print("<th style=\"background:#0b0; color:#030; width:50%;border-radius: 10px;border: 2px solid white;\">FX.25</th>\n");
	else
		html->print("<th style=\"background:#606060; color:#b0b0b0;border-radius: 10px;border: 2px solid white;\" aria-disabled=\"true\">FX.25</th>\n");
	html->print("</tr>\n");
	html->print("</table>\n");
	html->print("<br />\n");
	html->print("<table>\n");
	html->print("<tr>\n");
	html->print("<th colspan=\"2\">STATISTICS</th>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">RADIO RX:</td>\n");

	// Convert numeric values to strings using temporary buffers
	char temp_buffer[64];
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.rxCount);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">PACKET RX:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.allCount);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">PACKET TX:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.txCount);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">RF2INET:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.rf2inet);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">INET2RF:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.inet2rf);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">DIGI:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu</td>\n", status.digiCount);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td style=\"width: 60px;text-align: right;\">DROP/ERR:</td>\n");
	snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;\">%lu/", status.dropCount);
	html->print(temp_buffer);
	snprintf(temp_buffer, sizeof(temp_buffer), "%lu</td>\n", status.errorCount);
	html->print(temp_buffer);

	html->print("</tr>\n");
	html->print("</table>\n");
	html->print("<br />\n");
	if (config.gnss_enable)
	{
		html->print("<table>\n");
		html->print("<tr>\n");
		html->print("<th colspan=\"2\">GPS Info <a href=\"/gnss\" target=\"_gnss\" style=\"color: yellow;font-size:8pt\">[View]</a></th>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td>LAT:</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;text-align: left;\">%f</td>\n", gps.location.lat());
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td>LON:</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;text-align: left;\">%f</td>\n", gps.location.lng());
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td>ALT:</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;text-align: left;\">%f</td>\n", gps.altitude.meters());
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td>SAT:</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"background: #ffffff;text-align: left;\">%d</td>\n", gps.satellites.value());
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("</table>\n");
	}
	html->print("<script>\n");
	html->print("$(window).trigger('resize');\n");
	html->print("</script>\n");

	html->addHeader("Sidebar", "content");
	html->addHeader("Cache-Control", "no-cache");
	request->send(html);
}

void handle_symbol(AsyncWebServerRequest *request)
{
	int i;
	int sel = -1;
	for (i = 0; i < request->args(); i++)
	{
		if (request->argName(i) == "sel")
		{
			if (request->arg(i) != "")
			{
				if (isValidNumber(request->arg(i)))
				{
					sel = request->arg(i).toInt();
				}
			}
		}
	}

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *web = request->beginResponseStream("text/html", 26000);
	if (web)
	{
		web->print("<table border=\"1\" align=\"center\">\n");
		web->print("<tr><th colspan=\"16\">Table '/'</th></tr>\n");
		web->print("<tr>\n");
		char lnk[200];
		for (i = 33; i < 129; i++)
		{
			memset(lnk, 0, sizeof(lnk));
			//<td><img onclick="window.opener.setValue(113,2);" src="http://aprs.nakhonthai.net/symbols/icons/113-2.png"></td>
			if (sel == -1)
				sprintf(lnk, "<td><img onclick=\"window.opener.setValue(%d,1);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-1.png\"></td>", i, i);
			else
				sprintf(lnk, "<td><img onclick=\"window.opener.setValue(%d,%d,1);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-1.png\"></td>", sel, i, i);
			web->print(lnk);

			if (((i % 16) == 0) && (i < 126))
				web->print("</tr>\n<tr>\n");
		}
		web->print("</tr>");
		web->print("</table>\n<br />");
		web->print("<table border=\"1\" align=\"center\">\n");
		web->print("<tr><th colspan=\"16\">Table '\\'</th></tr>\n");
		web->print("<tr>\n");
		for (i = 33; i < 129; i++)
		{
			memset(lnk, 0, sizeof(lnk));
			if (sel == -1)
				sprintf(lnk, "<td><img onclick=\"window.opener.setValue(%d,2);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-2.png\"></td>", i, i);
			else
				sprintf(lnk, "<td><img onclick=\"window.opener.setValue(%d,%d,2);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-2.png\"></td>", sel, i, i);
			web->print(lnk);
			if (((i % 16) == 0) && (i < 126))
				web->print("</tr>\n<tr>\n");
		}
		web->print("</tr>");
		web->print("</table>\n");
		web->addHeader("Symbol", "content");
		web->addHeader("Cache-Control", "no-cache");
		request->send(web);
	}
}

void handle_sysinfo(AsyncWebServerRequest *request)
{
	// Using dynamic memory allocation instead of String
	char *html = allocateStringMemory(1024); // Initial buffer size, adjust as needed
	if (!html)
	{
		return; // Memory allocation failed
	}

	strcpy(html, "<table style=\"table-layout: fixed;border-collapse: unset;border-radius: 10px;border-color: #ee800a;border-style: ridge;border-spacing: 1px;border-width: 4px;background: #ee800a;\">\n");
	strcat(html, "<tr>\n");
	strcat(html, "<th><span><b>Up Time</b></span></th>\n");
	strcat(html, "<th><span>RAM(KByte)</span></th>\n");
#ifdef BOARD_HAS_PSRAM
	strcat(html, "<th><span>PSRAM(KByte)</span></th>\n");
#endif
	strcat(html, "<th><span>SPIFFS(KByte)</span></th>\n");
	if (VBat_Flag)
		strcat(html, "<th><span>VBat(V)</span></th>\n");
	strcat(html, "<th><span>CPU(Mhz)</span></th>\n");
	strcat(html, "<th><span>CPU.Temp(°C)</span></th>\n");

	strcat(html, "</tr>\n");
	strcat(html, "<tr>\n");
	// time_t tn = time(NULL) - systemUptime;
	//  char* uptime = String(day(tn) - 1, DEC) + "D " + String(hour(tn), DEC) + ":" + String(minute(tn), DEC) + ":" + String(second(tn), DEC);
	// String uptime = String(day(tn) - 1, DEC) + "D " + String(hour(tn), DEC) + ":" + String(minute(tn), DEC);
	char strTime[20];
	convertSecondsToDHMS(strTime, (millis() / 1000) - upTimeStamp);

	char temp_buffer[512];
	snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%s</b></td>\n", strTime);
	strcat(html, temp_buffer);

	snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%.1f/%.1f</b></td>\n", (float)ESP.getFreeHeap() / 1000, (float)ESP.getHeapSize() / 1000);
	strcat(html, temp_buffer);

#ifdef BOARD_HAS_PSRAM
	snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%.1f/%.1f</b></td>\n", (float)ESP.getFreePsram() / 1000, (float)ESP.getPsramSize() / 1000);
	strcat(html, temp_buffer);
#endif

	unsigned long cardTotal = LITTLEFS.totalBytes();
	unsigned long cardUsed = LITTLEFS.usedBytes();
	snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%.1f/%.1f</b></td>\n", (double)cardUsed / 1024, (double)cardTotal / 1024);
	strcat(html, temp_buffer);

	if (VBat_Flag)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%.2f</b></td>\n", VBat);
		strcat(html, temp_buffer);
	}

	snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%d</b></td>\n", ESP.getCpuFreqMHz());
	strcat(html, temp_buffer);

	// CPU temp dashboard fix - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
	// The ESPCPUTemp lib refuses classic ESP32 (D0WD/D0WD-V3) outright ("not found" - unsupported
	// by the official IDF driver on this chip), so we read the raw Arduino core sensor directly
	// instead, same as the earlier v1.6H build did. Not officially calibrated on classic ESP32,
	// but gives a usable relative reading instead of a permanent N/A.
	float cpuTempC = temperatureRead();
	if (!isnan(cpuTempC) && cpuTempC > -40.0f && cpuTempC < 125.0f)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "<td><b>%.1f</b></td>\n", cpuTempC);
		strcat(html, temp_buffer);
	}
	else
	{
		strcat(html, "<td><b>N/A</b></td>\n");
	}
	// html += "<td style=\"background: #f00\"><b>" + String(ESP.getCycleCount()) + "</b></td>\n";
	strcat(html, "</tr>\n");
	strcat(html, "</table>\n");

	// request->send(200, "text/html", html); // send to someones browser when asked
	AsyncWebServerResponse *response = request->beginResponse(200, "text/html", (const char *)html);
	response->addHeader("Sysinfo", "content");
	response->addHeader("Cache-Control", "no-cache");
	request->send(response);
	free(html); // Free the allocated memory
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

	// Using dynamic memory allocation instead of String
	char *html = allocateStringMemory(4096); // Initial buffer size, adjust as needed
	if (!html)
	{
		return String(""); // Memory allocation failed
	}

	time_t timen = time(NULL);
	localtime_r(&timen, &tmNow);

	strcpy(html, "<tr>\n");
	strcat(html, "<th style=\"width:60pt\"><span><b>Time (");
	if (config.timeZone >= 0)
		strcat(html, "+");

	// Convert timezone to string
	char temp_buffer[64];
	if (config.timeZone == (int)config.timeZone)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%d", (int)config.timeZone);
		strcat(html, temp_buffer);
		strcat(html, ")</b></span></th>\n");
	}
	else
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.1f", config.timeZone);
		strcat(html, temp_buffer);
		strcat(html, ")</b></span></th>\n");
	}
	// strcat(html, "<th style=\"min-width:16px\">ICON</th>\n");

	strcat(html, "<th style=\"width:70pt\">Callsign</th>\n");
	strcat(html, "<th>Message</th>\n");
	strcat(html, "<th style=\"width:10pt\">ACK</th>\n");
	strcat(html, "<th style=\"width:20pt\">msgID</th>\n");
	strcat(html, "</tr>\n");

	pkgMsgSort(msgQueue);
	for (int i = 0; i < PKGLISTSIZE; i++)
	{
		if (i >= PKGLISTSIZE)
			break;
		msgType pkg = getMsgList(i);
		if (pkg.time > 0)
		{
			// String line = String(pkg.text); // Not needed anymore

			pkg.callsign[10] = 0;
			// time_t tm = pkg.time;
			localtime_r(&pkg.time, &tmstruct);
			char strTime[10];
			// sprintf(strTime, "%02d:%02d:%02d", tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
			if (tmNow.tm_mday == tmstruct.tm_mday)
				sprintf(strTime, "%02d:%02d:%02d", tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
			else
				sprintf(strTime, "%dd %02d:%02d", tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min);
			// String str = String(tmstruct.tm_hour, DEC) + ":" + String(tmstruct.tm_min, DEC) + ":" + String(tmstruct.tm_sec, DEC);

			if (pkg.ack > 0)
			{
				strcat(html, "<tr style=\"background-color: #f1697dff;\">\n");
			}
			else if (pkg.ack == -1)
			{
				strcat(html, "<tr style=\"background-color: #7ff1c5ff;\">\n");
			}
			else if (pkg.ack == -2)
			{
				strcat(html, "<tr style=\"background-color: #73caf0ff;\">\n");
			}
			else
			{
				strcat(html, "<tr style=\"background-color: #f55353ff;\">\n");
			}

			strcat(html, "<td>");
			strcat(html, strTime);
			strcat(html, "</td>");

			strcat(html, "<td>");
			strcat(html, pkg.callsign);
			strcat(html, "</td>");

			strcat(html, "<td style=\"text-align: left;\">");
			strcat(html, pkg.text);
			strcat(html, "</td>");

			if (pkg.ack > 0)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<td>%d/%d</td>", pkg.ack, config.msg_retry);
				strcat(html, temp_buffer);
			}
			else if (pkg.ack == -1)
			{
				strcat(html, "<td>RX</td>");
			}
			else if (pkg.ack == -2)
			{
				strcat(html, "<td>TX</td>");
			}
			else
			{
				strcat(html, "<td>TF</td>");
			}

			snprintf(temp_buffer, sizeof(temp_buffer), "<td>%d</td></tr>\n", pkg.msgID);
			strcat(html, temp_buffer);
		}
	}

	size_t html_len = strlen(html);
	log_d("HTML Length=%d Byte gethtml:%d event_cnt:%d", html_len, gethtml, message_events.count());

	if (gethtml)
	{
		String result = String(html); // Convert back to String for return
		free(html);					  // Free the allocated memory
		return result;
	}

	if (message_events.count() > 0)
	{
		char *info = (char *)calloc(html_len + 1, sizeof(char)); // +1 for null terminator
		if (info)
		{
			strcpy(info, html);
			message_events.send(info, "chatMsg", time(NULL), 5000);
			free(info);
		}
	}

	free(html); // Free the allocated memory
	return String("");
}

void handle_storage(AsyncWebServerRequest *request)
{
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

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 20000);
	if (!webString)
	{
		return; // Memory allocation failed
	}

		webString->print("<script type=\"text/javascript\">\n"
					  "function sub(obj){"
					  "var fileName = obj.value.split('\\\\');"
					  "document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
					  "}\n"
					  //"var form = document.getElementById('upload_form');"
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
					  //"document.getElementById('upload_sumbit').disabled = true;"
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
					  //"});"
					  "}else{"					  
					  "$.ajax({"
					  "url: '/storage',"
					  "type: 'POST',"
					  "data: data,"
					  "contentType: false,"
					  "processData:false,"					  
					  "success:function(d, s) {"
					  //"alert('Upload Success');"
					  "if(e.currentTarget.id===\"formDelete\") $(\"#contentmain\").load(\"/storage\");\n"
					  "},"
					  "error: function (a, b, c) {"
					  "}"
					  "});"
					  //"});"
					  "}"
					  "});"
					  "</script>");

	webString->print("<div style=\"font-size: 8pt;text-align:left;\">");
	webString->print("<b>Total space: </b>");

	char temp_buffer[512];
	if (cardTotal > 1000000)
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.2f MByte ,", (double)cardTotal / 1048576);
		webString->print(temp_buffer);
	}
	else
	{
		snprintf(temp_buffer, sizeof(temp_buffer), "%.2f KByte ,", (double)cardTotal / 1024);
		webString->print(temp_buffer);
	}

	webString->print("<b>Used space: </b>");
	snprintf(temp_buffer, sizeof(temp_buffer), "%.2f KByte", (double)cardUsed / 1024);
	webString->print(temp_buffer);

	snprintf(temp_buffer, sizeof(temp_buffer), "</br>Listing directory: </b>%s</div>\n", dirname);
	webString->print(temp_buffer);

	File root = LITTLEFS.open(dirname);
	if (!root)
	{
		webString->print("Failed to open directory\n");
		// return;
	}
	if (!root.isDirectory())
	{
		webString->print("Not a directory");
		// return;
	}

	File file = root.openNextFile();
	webString->print("<table border=\"1\"><tr align=\"center\" bgcolor=\"#03DDFC\"><th width=\"100\"><b>DIRECTORY</b></th><th><b>FILE NAME</b></th><th width=\"100\"><b>SIZE(Byte)</b></th><th width=\"170\"><b>DATE TIME</b></th><th width=\"50\"><b>DELETE</b></th><th width=\"100\"><b>DOWNLOAD</b></th></tr>");
	while (file)
	{
		if (file.isDirectory())
		{
			// webString += "<tr><td>DIR : ");
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td>%s</td>", file.name());
			webString->print(temp_buffer);
			time_t t = file.getLastWrite();
			struct tm *tmstruct = localtime(&t);
			sprintf(strTime, "<td></td><td></td><td align=\"right\">%d-%02d-%02d %02d:%02d:%02d</td>", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
			webString->print(strTime);
			// if (levels) {
			//	listDir(fs, file.name(), levels - 1);
			// }
			webString->print("<td></td></tr>\n");
		}
		else
		{
			/*Serial.print("  FILE: ");
			Serial.print(file.name());*/
			// char *fName = String(file.name()).substring(1).c_str(); // Not needed for full path
			const char *fName = file.name();
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td>/</td><td align=\"left\"><a href=\"/download?FILE=%s\" target=\"_blank\">%s</a></td>", fName, fName);
			webString->print(temp_buffer);
			// Serial.print("  SIZE: ");
			snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"right\">%d</td>", file.size());
			webString->print(temp_buffer);
			time_t t = file.getLastWrite();
			struct tm *tmstruct = localtime(&t);
			sprintf(strTime, "<td align=\"center\">%d-%02d-%02d %02d:%02d:%02d</td>", (tmstruct->tm_year) + 1900, (tmstruct->tm_mon) + 1, tmstruct->tm_mday, tmstruct->tm_hour, tmstruct->tm_min, tmstruct->tm_sec);
			webString->print(strTime);
			if (strcmp(fName, "fsversion.txt") == 0)
			{
				webString->print("<td></td>");
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formDelete\" method=\"post\"><input name=\"delete\" type=\"hidden\" /><input name=\"FILE\" type=\"hidden\" value=\"%s\" /><button name=\"commit\" id=\"btnDelete\" type=\"submit\" style=\"background-color:red;color:white\">X</button></form></td>\n", fName);
				webString->print(temp_buffer);
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formDownload\" method=\"post\"><input name=\"download\" type=\"hidden\" /><input name=\"FILE\" type=\"hidden\" value=\"%s\" /><button name=\"commit\" id=\"btnDownload\" type=\"submit\" style=\"background-color:green;color:white\">DOWNLOAD</button></form></td></tr>\n", fName);
			webString->print(temp_buffer);
		}
		file = root.openNextFile();
	}
	webString->print("</table>\n");
	// webString->print("<form accept-charset=\"UTF-8\" action=\"/format\" class=\"form-horizontal\" id=\"format_form\" method=\"post\">\n");
	// webString->print("<br><div><button class=\"button\" type='submit' id='format_form_sumbit'  name=\"commit\"> FORMAT </button></div>\n");
	// webString->print("</form><br/>\n");

	// UPLOAD CONFIGURATION FILE
	webString->print("<br><form accept-charset=\"UTF-8\" action=\"#\" "
					  "class=\"form-horizontal\" id=\"upload_form\" method=\"post\" "
					  "enctype=\"multipart/form-data\">\n");

	webString->print("<table border=\"1\" style=\"margin-top:10px;width:100%;\">\n");
	webString->print("<tr align=\"center\" bgcolor=\"#03DDFC\">"
					  "<th colspan=\"3\"><b>UPLOAD FILE</b></th>"
					  "</tr>\n");

	webString->print("<tr align=\"center\">"
					  "<td width=\"60\" align=\"right\"><b>File:</b></td>"
					  "<td align=\"left\"><input type=\"file\" name=\"data\" required></td>"
					  "<td width=\"120\"><button class=\"button\" type='submit' id='upload_sumbit'>UPLOAD</button></td>"
					  "</tr>\n");

	webString->print("</table>\n");
	webString->print("</form><br/>\n");

	webString->print("</body>\n</html>\n");

	webString->addHeader("Sensor", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
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
	}
	else
	{
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 20000);
		if (!html)
		{
			return; // Memory allocation failed
		}

		html->print("<script type=\"text/javascript\">\n");
		html->print("var sliderVol = document.getElementById(\"sliderVolume\");\n");
		html->print("var outputVol = document.getElementById(\"volShow\");\n");
		html->print("var sliderSql = document.getElementById(\"sliderSql\");\n");
		html->print("var outputSql = document.getElementById(\"sqlShow\");\n");
		html->print("outputVol.innerHTML = sliderVol.value;\n");
		html->print("outputSql.innerHTML = sliderSql.value;\n");
		html->print("\n");
		html->print("sliderVol.oninput = function () {\n");
		html->print("outputVol.innerHTML = this.value;\n");
		html->print("}\n");
		html->print("sliderSql.oninput = function () {\n");
		html->print("outputSql.innerHTML = this.value;\n");
		html->print("}\n");
		html->print("\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formRadio\") document.getElementById(\"submitRadio\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formTNC\") document.getElementById(\"submitTNC\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/radio',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("function rfType(){\n");
		html->print("var type = document.getElementById(\"rf_type\").value;\n");
		html->print("if(type==1||type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"max\",174);document.getElementById(\"rx_freq\").setAttribute(\"max\",174);};\n");
		html->print("if(type==1){document.getElementById(\"tx_freq\").setAttribute(\"min\",134);document.getElementById(\"rx_freq\").setAttribute(\"min\",134);};\n");
		html->print("if(type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"min\",136);document.getElementById(\"rx_freq\").setAttribute(\"min\",136);};\n");
		html->print("if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"max\",470);document.getElementById(\"rx_freq\").setAttribute(\"max\",470);};\n");
		html->print("if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"min\",400);document.getElementById(\"rx_freq\").setAttribute(\"min\",400);};\n");
		html->print("if(type==3){document.getElementById(\"tx_freq\").setAttribute(\"min\",320);document.getElementById(\"rx_freq\").setAttribute(\"min\",320);};\n");
		html->print("if(type==3){document.getElementById(\"tx_freq\").setAttribute(\"max\",400);document.getElementById(\"rx_freq\").setAttribute(\"max\",400);};\n");
		html->print("if(type==6){document.getElementById(\"tx_freq\").setAttribute(\"min\",350);document.getElementById(\"rx_freq\").setAttribute(\"min\",350);};\n");
		html->print("if(type==6){document.getElementById(\"tx_freq\").setAttribute(\"max\",390);document.getElementById(\"rx_freq\").setAttribute(\"max\",390);};\n");
		html->print("if(type==1||type==4||type==7){document.getElementById(\"tx_freq\").setAttribute(\"value\",144.390);document.getElementById(\"rx_freq\").setAttribute(\"value\",144.390);};\n");
		html->print("if(type==2||type==5||type==8){document.getElementById(\"tx_freq\").setAttribute(\"value\",432.5);document.getElementById(\"rx_freq\").setAttribute(\"value\",432.5);};\n");
		html->print("\n");
		html->print("}\n");
		html->print("</script>\n");
		html->print("<form id='formRadio' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>RF Analog Module</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");

		// Handle radio enable flag
		char temp_buffer[256];
		if (config.rf_en)
		{
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"radioEnable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n");
		}
		else
		{
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"radioEnable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n");
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Module Type:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"rf_type\" id=\"rf_type\" onchange=\"rfType()\">\n");
		for (int i = 0; i < 9; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.rf_type == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%s</option>\n", RF_TYPE[i]);
			html->print(temp_buffer);
		}
		html->print("</select>\n");
		html->print("</td>\n");
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
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX Frequency:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input type=\"number\" id=\"tx_freq\" name=\"tx_freq\" min=\"%.4f\" max=\"%.4f\"\n", freqMin, freqMax);
		html->print(temp_buffer);
		snprintf(temp_buffer, sizeof(temp_buffer), "step=\"0.0001\" value=\"%.4f\" /> MHz</td>\n", config.freq_tx);
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RX Frequency:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input type=\"number\" id=\"rx_freq\" name=\"rx_freq\" min=\"%.4f\" max=\"%.4f\"\n", freqMin, freqMax);
		html->print(temp_buffer);
		snprintf(temp_buffer, sizeof(temp_buffer), "step=\"0.0001\" value=\"%.4f\" /> Mhz</td>\n", config.freq_rx);
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX CTCSS:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"tx_ctcss\" id=\"tx_ctcss\">\n");
		for (int i = 0; i < 39; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.tone_tx == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%.1f</option>\n", ctcss[i]);
			html->print(temp_buffer);
		}
		html->print("</select> Hz\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RX CTCSS:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"rx_ctcss\" id=\"rx_ctcss\">\n");
		html->print("<option value=\"0\" selected>0.0</option>\n");
		for (int i = 0; i < 39; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.tone_rx == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%.1f</option>\n", ctcss[i]);
			html->print(temp_buffer);
		}
		html->print("</select> Hz\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Narrow/Wide:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"nw_band\" id=\"nw_band\">\n");

		if (config.band)
		{
			html->print("<option value=\"0\" >12.5KHz</option>\n");
			html->print("<option value=\"1\" selected>25.0KHz</option>\n");
		}
		else
		{
			html->print("<option value=\"0\" selected>12.5KHz</option>\n");
			html->print("<option value=\"1\" >25.0KHz</option>\n");
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX Power:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"rf_power\" id=\"rf_power\">\n");

		if (config.rf_power)
		{
			html->print("<option value=\"1\" selected>HIGH</option>\n");
			html->print("<option value=\"0\" >LOW</option>\n");
		}
		else
		{
			html->print("<option value=\"1\" >HIGH</option>\n");
			html->print("<option value=\"0\" selected>LOW</option>\n");
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>VOLUME:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"sliderVolume\" name=\"volume\" type=\"range\"\nmin=\"1\" max=\"8\" value=\"%d\" /><b><span style=\"font-size: 14pt;\" id=\"volShow\">%d</span></b></td>\n", config.volume, config.volume);
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SQL Level:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"sliderSql\" name=\"sql_level\" type=\"range\"\nmin=\"0\" max=\"8\" value=\"%d\" /><b><span style=\"font-size: 14pt;\" id=\"sqlShow\">%d</span></b></td>\n", config.sql_level, config.sql_level);
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitRadio'  name=\"commitRadio\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitRadio\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form>");

		// AFSK,TNC Configuration
		html->print("<form id='formTNC' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>AFSK/TNC Configuration</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Modem Type:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"modem_type\" id=\"modem_type\" \">\n");
		#ifdef CONFIG_IDF_TARGET_ESP32S3
		for (int i = 0; i < 4; i++)
		#else 
		for (int i = 0; i < 3; i++)
		#endif
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.modem_type == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%s</option>\n", MODEM_TYPE[i]);
			html->print(temp_buffer);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>FX.25 Mode:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"fx25_mode\" id=\"fx25_mode\" \">\n");
		for (int i = 0; i < 3; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.fx25_mode == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%s</option>\n", FX25_MODE[i]);
			html->print(temp_buffer);
		}
		html->print("</select>  (FX.25 = AX.25 + FEC)\n");
		html->print("</td>\n");
		html->print("<tr>\n");
		// html->print("<td align=\"right\"><b>Audio HPF:</b></td>\n");
		// char strFlag[32] = "";
		// if (config.audio_hpf)
		// 	strcpy(strFlag, "checked");
		// snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"HPF\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Audio high pass filter >1KHz cutoff 10Khz</i></label></td>\n", strFlag);
		// html->print(temp_buffer);
		// html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Deemphasis Audio:</b></td>\n");
		if (config.audio_lpf)
		{
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"LPF\" value=\"OK\" checked><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Audio low pass filter 1hz-2.5KHz</i></label></td>\n");
		}
		else
		{
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"LPF\" value=\"OK\" ><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Audio low pass filter 1hz-2.5KHz</i></label></td>\n");
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX Time Slot:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input type=\"number\" name=\"timeSlot\" min=\"0\" max=\"99999\"\nstep=\"100\" value=\"%d\" /> mSec.</td>\n", config.tx_timeslot);
		html->print(temp_buffer);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Preamble:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"preamble\">\n");
		for (int i = 1; i < 11; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", i);
			html->print(temp_buffer);
			if (config.preamble == i)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%d</option>\n", i * 100);
			html->print(temp_buffer);
		}
		html->print("</select> mSec.\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitTNC'  name=\"commitTNC\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitTNC\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form>");

		html->addHeader("Sysinfo", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

void handle_vpn(AsyncWebServerRequest *request)
{
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
					strcpy(config.wg_public_key, request->arg(i).c_str());
					config.wg_public_key[44] = 0;
				}
			}

			if (request->argName(i) == "wg_private_key")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wg_private_key, request->arg(i).c_str());
					config.wg_private_key[44] = 0;
				}
			}

			if (request->argName(i) == "wg_peer_address")
			{
				if (request->arg(i) != "")
				{
					strcpy(config.wg_peer_address, request->arg(i).c_str());
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

		config.vpn = vpnEn;
		saveConfig(request);
	}
	else
	{
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 16000);
		if (!html)
		{
			return; // Memory allocation failed
		}

		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formVPN\") document.getElementById(\"submitVPN\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/vpn',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");

		// Get MAC address and remove colons
		String ESP32_ID = WiFi.macAddress();
		ESP32_ID.replace(":", "");
		char temp_buffer[512];
		snprintf(temp_buffer, sizeof(temp_buffer), "function loadVPNConfig() {\nconst url = \"http://hs1.hs5tqa.ampr.org:81/wg/create\";\nconst espID = {'name': '%s'};\n", ESP32_ID.c_str());
		html->print(temp_buffer);
		html->print("fetch(url,{\n");
		html->print("method: 'POST',\n");
		html->print("body: JSON.stringify(espID),\n");
		html->print("headers: { 'Content-Type': 'application/json', 'Access-Control-Allow-Headers': 'Content-Type', 'Access-Control-Allow-Origin': '*','Access-Control-Allow-Methods': 'POST,GET,OPTIONS'}\n");
		html->print("})\n");
		html->print(".then(response => response.json())\n");
		html->print(".then(data => {\n");
		html->print("console.log(\"VPN Data:\", data);\n");
		html->print("document.getElementById(\"wg_enable\").checked = true;\n");
		html->print("document.getElementById(\"wg_peer_address\").value = data.Enpoint.split(\":\")[0];\n");
		html->print("document.getElementById(\"wg_port\").value = data.Enpoint.split(\":\")[1];\n");
		html->print("document.getElementById(\"wg_local_address\").value = data.Address;\n");
		html->print("document.getElementById(\"wg_netmask_address\").value = \"255.255.255.0\";\n");
		html->print("document.getElementById(\"wg_gw_address\").value = data.Gateway;\n");
		html->print("document.getElementById(\"wg_public_key\").value = data.PublicKey;\n");
		html->print("document.getElementById(\"wg_private_key\").value = data.PrivateKey;\n");
		html->print("})\n");
		html->print(".catch(err => console.error(\"VPN API Error:\", err));\n}\n");
		html->print("</script>\n");
		// ===== JavaScript AJAX =====
		// html->print("<script>\n");
		// html->print("function loadVPNConfig() {\n");
		// html->print("  $.ajax({\n");
		// html->print("    url: '/api/vpnreq',\n");
		// html->print("    method: 'GET',\n");
		// html->print("    dataType: 'json',\n");
		// html->print("    success: function(data) {\n");
		// html->print("       console.log(data);\n");
		// html->print("       let ep = data.Enpoint.split(':');\n");
		// html->print("       $('#wg_peer_address').val(ep[0]);\n");
		// html->print("       $('#wg_port').val(ep[1]);\n");
		// html->print("       $('#wg_local_address').val(data.Address);\n");
		// html->print("       $('#wg_public_key').val(data.PublicKey);\n");
		// html->print("       $('#wg_private_key').val(data.PrivateKey);\n");
		// html->print("    },\n");
		// html->print("    error: function(e) {\n");
		// html->print("       alert('โหลดข้อมูล VPN ไม่สำเร็จ');\n");
		// html->print("       console.log(e);\n");
		// html->print("    }\n");
		// html->print("  });\n");
		// html->print("}\n");
		// html->print("</script>");

		// html->print("<h2>System Setting</h2>\n");
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromVPN\" method=\"post\">\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Wireguard Configuration</b></span></th>\n");
		html->print("<tr>");

		// Handle sync flag
		if (config.vpn)
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"wg_enable\" name=\"vpnEnable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n");
		}
		else
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"wg_enable\" name=\"vpnEnable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n");
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Address</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  size=\"20\" maxlength=\"32\" id=\"wg_peer_address\" name=\"wg_peer_address\" type=\"text\" value=\"%s\" /></td>\n", config.wg_peer_address);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Port</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"wg_port\" size=\"5\" name=\"wg_port\" type=\"number\" value=\"%d\" /></td>\n", config.wg_port);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Local Address</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"wg_local_address\" name=\"wg_local_address\" type=\"text\" value=\"%s\" /></td>\n", config.wg_local_address);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Netmask</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"wg_netmask_address\" name=\"wg_netmask_address\" type=\"text\" value=\"%s\" /></td>\n", config.wg_netmask_address);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Gateway</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input id=\"wg_gw_address\" name=\"wg_gw_address\" type=\"text\" value=\"%s\" /></td>\n", config.wg_gw_address);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Public Server Key</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"44\" id=\"wg_public_key\" name=\"wg_public_key\" type=\"text\" value=\"%s\" /></td>\n", config.wg_public_key);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Private Client Key</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"44\" id=\"wg_private_key\" name=\"wg_private_key\" type=\"text\" value=\"%s\" /></td>\n", config.wg_private_key);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitVPN'  name=\"commitVPN\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitVPN\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br /><br />");

		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromGetVPN\" method=\"post\">\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Helper: Free VPN Wireguard for Web Service</b></span></th>\n");
		html->print("<tr><td align=\"left\">1. Click New Register button to get VPN config from web service.</td></tr>\n");
		html->print("<tr><td align=\"left\">2. The VPN config will fill in the form automatically.</td></tr>\n");
		html->print("<tr><td align=\"left\">3. Click Apply Change and reboot again.</td></tr>\n");
		html->print("<tr><td align=\"left\">4. Enjoy your free VPN service!</td></tr>\n");

		// Check if local address starts with "10.44."
		String wg_local_addr = String(config.wg_local_address);
		if (wg_local_addr.startsWith("10.44."))
		{
			int lastoct = wg_local_addr.substring(wg_local_addr.lastIndexOf('.') + 1).toInt();
			// String url="http://"+String(config.wg_peer_address)+":"+String(8000+lastoct);
			// html->print("<tr><td>Your External Host IP: <a href=\"");
			// snprintf(temp_buffer, sizeof(temp_buffer), "%s\">%s</a></td></tr>\n", url.c_str(), url.c_str());
			// html->print(temp_buffer);
			int thirdoct = wg_local_addr.substring(wg_local_addr.indexOf('.', wg_local_addr.indexOf('.') + 1) + 1, wg_local_addr.lastIndexOf('.')).toInt();
			String url_base = "http://hs" + String(thirdoct) + ".hs5tqa.ampr.org:" + String((thirdoct * 10000) + 8000 + lastoct);
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td>Your External by AMPR URL: <a href=\"%s\" target=\"_blank\">%s</a></td></tr>\n", url_base.c_str(), url_base.c_str());
			html->print(temp_buffer);
			url_base = "http://vpn.nakhonthai.net:" + String((thirdoct * 10000) + 8000 + lastoct);
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td>Fast Direct URL: <a href=\"%s\" target=\"_blank\">%s</a></td></tr>\n", url_base.c_str(), url_base.c_str());
			html->print(temp_buffer);
		}
		html->print("<tr><td><button type=\"button\" onclick=\"loadVPNConfig()\">New Register</button></td></tr>\n");
		html->print("</table><br />\n");
		html->print("</form>");

		html->addHeader("VPN", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

#ifdef MQTT
void handle_mqtt(AsyncWebServerRequest *request)
{
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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 17000);
		if (!html)
		{
			return; // Memory allocation failed
		}

		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formVPN\") document.getElementById(\"submitMQTT\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/mqtt',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		// html->print("<h2>System Setting</h2>\n");
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromMQTT\" method=\"post\">\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>MQTT Configuration</b></span></th>\n");
		html->print("<tr>");

		// Handle sync flag
		if (config.en_mqtt)
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n");
		}
		else
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n");
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Address:</b></td>\n");
		char temp_buffer[512];
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  size=\"30\" maxlength=\"32\" name=\"host\" type=\"text\" value=\"%s\" /></td>\n", config.mqtt_host);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Port:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"5\"  maxlength=\"5\"  name=\"port\" type=\"number\" value=\"%d\" /></td>\n", config.mqtt_port);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>User:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input maxlength=\"32\" name=\"user\" type=\"text\" value=\"%s\" /></td>\n", config.mqtt_user);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Password:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"40\" maxlength=\"63\" name=\"pass\" type=\"password\" value=\"%s\" /></td>\n", config.mqtt_pass);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Topic:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"32\" id=\"topic\" name=\"topic\" type=\"text\" value=\"%s\" /></td>\n", config.mqtt_topic);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Topic Flag:</b></td>\n");

		html->print("<td align=\"center\">\n");
		html->print("<fieldset id=\"TopicGrp\">\n");
		html->print("<legend>Topic Flags Send out MQTT</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">");
		html->print("<tr style=\"background:unset;\">");

		// Handle topic flags
		if (config.mqtt_topic_flag & MQTT_TOPIC_TNC)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTNC\" type=\"checkbox\" value=\"OK\" checked/>TNC</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTNC\" type=\"checkbox\" value=\"OK\" />TNC</td>\n");
		}

		if (config.mqtt_topic_flag & MQTT_TOPIC_STATUS)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSts\" type=\"checkbox\" value=\"OK\" checked/>Status</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSts\" type=\"checkbox\" value=\"OK\" />Status</td>\n");
		}

		if (config.mqtt_topic_flag & MQTT_TOPIC_TELEMETRY)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTlm\" type=\"checkbox\" value=\"OK\" checked/>Telemetry</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicTlm\" type=\"checkbox\" value=\"OK\" />Telemetry</td>\n");
		}

		if (config.mqtt_topic_flag & MQTT_TOPIC_WX)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicWX\" type=\"checkbox\" value=\"OK\" checked/>Weather</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicWX\" type=\"checkbox\" value=\"OK\" />Weather</td>\n");
		}

		if (config.mqtt_topic_flag & MQTT_TOPIC_SENSOR)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSensor\" type=\"checkbox\" value=\"OK\" checked/>Sensor</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"TopicSensor\" type=\"checkbox\" value=\"OK\" />Sensor</td>\n");
		}

		html->print("<td style=\"border:unset;\"></td>");
		html->print("</tr></table></fieldset>\n");
		html->print("</td></tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Subscription:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"50\" maxlength=\"32\" name=\"subscribe\" type=\"text\" value=\"%s\" /></td>\n", config.mqtt_subscribe);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Subscription Flag:</b></td>\n");

		html->print("<td align=\"center\">\n");
		html->print("<fieldset id=\"SubGrp\">\n");
		html->print("<legend>Subscription Flags Receive</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">");
		html->print("<tr style=\"background:unset;\">");

		// Handle subscription flags
		if (config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_CMD)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subCMD\" type=\"checkbox\" value=\"OK\" checked/>AT-Command</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subCMD\" type=\"checkbox\" value=\"OK\" />AT-Command</td>\n");
		}

		if (config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_TNC)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subTNC\" type=\"checkbox\" value=\"OK\" checked/>TNC</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subTNC\" type=\"checkbox\" value=\"OK\" />TNC</td>\n");
		}

		if (config.mqtt_subscribe_flag & MQTT_SUBSCRIBE_MESSAGE)
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subMsg\" type=\"checkbox\" value=\"OK\" checked/>Message</td>\n");
		}
		else
		{
			html->print("<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"subMsg\" type=\"checkbox\" value=\"OK\" />Message</td>\n");
		}

		html->print("<td style=\"border:unset;\"></td>");
		html->print("</tr></table></fieldset>\n");
		html->print("</td></tr>\n");

		html->print("</table><br />\n");
		html->print("<td><input class=\"button\" id=\"submitMQTT\" name=\"commitMQTT\" type=\"submit\" value=\"Save Config\" maxlength=\"80\"/></td>\n");
		html->print("<input type=\"hidden\" name=\"commitMQTT\"/>\n");
		html->print("</form>\n");

		html->addHeader("MQTT", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
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
		sendAPRSMessage(String(toCall), String(msg), config.msg_encrypt);
		// Using dynamic memory allocation instead of String
		char *html = allocateStringMemory(64); // Small buffer for "Send completed"
		if (html)
		{
			strcpy(html, "Send completed");
			request->send(200, "text/html", html); // send to someones browser when asked
			free(html);							   // Free the allocated memory
		}
	}
	else if (request->hasArg("commitObj"))
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
	else if (request->hasArg("commitBLN"))
	{
		bool blnEn[9] = {false};
		char blnText[9][STATUS_SIZE];
		uint16_t blnInterval[9];
		uint16_t blnLimit[9];
		uint8_t blnActiveFor[9]; // v2.1-lu6jmf: 0=default (72/24h), else 24/36/48/72
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
			blnActiveFor[bi] = config.bln_activefor[bi];
		}

		for (uint8_t i = 0; i < request->args(); i++)
		{
			String argName = request->argName(i);
			for (uint8_t bi = 0; bi < 9; bi++)
			{
				char fieldEn[10], fieldText[10], fieldInt[12], fieldLimit[12], fieldActFor[14];
				snprintf(fieldEn, sizeof(fieldEn), "blnEn%d", bi + 1);
				snprintf(fieldText, sizeof(fieldText), "blnText%d", bi + 1);
				snprintf(fieldInt, sizeof(fieldInt), "blnInv%d", bi + 1);
				snprintf(fieldLimit, sizeof(fieldLimit), "blnLim%d", bi + 1);
				snprintf(fieldActFor, sizeof(fieldActFor), "blnActFor%d", bi + 1);

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
					// v2.1.4-lu6jmf: Alerts and News share the same preset dropdown now (seconds),
					// so both branches read the same way - no more *60 for News.
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
				if (argName == fieldActFor)
				{
					// Blank/0 = default (72h Alerts / 24h News); else 24/36/48/72
					if (isValidNumber(request->arg(i)))
						blnActiveFor[bi] = request->arg(i).toInt();
					else
						blnActiveFor[bi] = 0;
				}
			}
		}

		for (uint8_t bi = 0; bi < 9; bi++)
		{
			// Enable was OFF and is now being turned ON: start a fresh send count
			if (!config.bln_en[bi] && blnEn[bi])
			{
				blnSentCount[bi] = 0;
			}
			config.bln_en[bi] = blnEn[bi];
			strlcpy(config.bln_text[bi], blnText[bi], sizeof(config.bln_text[bi]));
			config.bln_interval[bi] = blnInterval[bi];
			config.bln_limit[bi] = blnLimit[bi];
			config.bln_activefor[bi] = blnActiveFor[bi];
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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks).
		// v2.1.3-lu6jmf: bumped 28000->36000 - BLN Active-for/schedule-preview rows and the
		// Objects section pushed real content past 28000, forcing a mid-stream realloc that
		// could fail under heap fragmentation and truncate the page (seen live: cut off
		// partway through Object4, Apply button + <script> with setValue() never sent).
		AsyncResponseStream *html = request->beginResponseStream("text/html", 36000);
		if (!html)
		{
			return; // Memory allocation failed
		}

		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formMSG\") document.getElementById(\"submitMSG\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formBLN\") document.getElementById(\"submitBLN\").disabled=true;\n");
		// v2.1.4-lu6jmf: formObj was missing from every branch below, so its Apply Change
		// button never showed the confirmation popup even though the save worked fine.
		html->print("if(e.currentTarget.id===\"formObj\") document.getElementById(\"submitObj\").disabled=true;\n");
		// html->print("if(e.currentTarget.id===\"formChat\") document.getElementById(\"submitI2C0\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/msg',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("if(e.currentTarget.id===\"formMSG\") alert(\"Submited Successfully\");\n");
		html->print("if(e.currentTarget.id===\"formBLN\") alert(\"Submited Successfully\");\n");
		html->print("if(e.currentTarget.id===\"formObj\") { alert(\"Submited Successfully\"); document.getElementById(\"submitObj\").disabled=false; }\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("if(e.currentTarget.id===\"formMSG\") alert(\"An error occurred.\");\n");
		html->print("if(e.currentTarget.id===\"formBLN\") alert(\"An error occurred.\");\n");
		html->print("if(e.currentTarget.id===\"formObj\") { alert(\"An error occurred.\"); document.getElementById(\"submitObj\").disabled=false; }\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");

		// html->print("if (!!window.EventSource) {";
		// strcat(html, "var source = new EventSource('/eventMsg');");

		// html->print("source.addEventListener('open', function(e) {";
		// strcat(html, "console.log(\"Events MSG Connected\");";
		// html->print("}, false);";
		// html->print("source.addEventListener('error', function(e) {";
		// strcat(html, "if (e.target.readyState != EventSource.OPEN) {";
		// strcat(html, "console.log(\"Events MSG Disconnected\");";
		// html->print("}\n}, false);";
		// html->print("source.addEventListener('chatMsg', function(e) {";
		// // strcat(html, "console.log(\"lastHeard\", e.data);";
		// html->print("var lh=document.getElementById(\"chatMsg\");";
		// html->print("if(lh != null) {lh.innerHTML = e.data;}";
		// strcat(html, "}, false);\n}";
		html->print("</script>\n");

		// html->print("<h2>System Setting</h2>\n");
		// v2.1.2-lu6jmf: discreet build marker so it is unmistakable in the browser whether a
		// given reflash actually took. Also carries the last reset reason here (not on the
		// main DashBoard) so it doesn't alarm non-technical users with terms like PANIC/WDT/
		// BROWNOUT - stays available for troubleshooting without being front-and-center.
		char buildInfo[160];
		snprintf(buildInfo, sizeof(buildInfo), "<div style=\"text-align:right;color:#999;font-size:7pt;padding:2px 4px;\">build " __DATE__ " " __TIME__ " | reset: %s</div>\n", lastResetReasonStr);
		html->print(buildInfo);
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formMSG\" method=\"post\">\n");
		html->print("<table width=\"90%\">\n");
		html->print("<th colspan=\"2\"><span><b>Message Configuration</b></span></th>\n");
		html->print("<tr>");

		// Handle sync flag
		if (config.msg_enable)
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n");
		}
		else
		{
			html->print("<td align=\"right\"><b>Enable</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"enable\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n");
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>My Callsign:</b></td>\n");
		char temp_buffer[1200]; // v2.1.1-lu6jmf: bumped from 512 - the Objects Interval/Symbol rows are longer than that
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  size=\"20\" maxlength=\"9\" name=\"mycall\" type=\"text\" value=\"%s\" /> *<i>Callsign with SSID (Ex. HS5TQA-12)</i></td>\n", config.msg_mycall);
		html->print(temp_buffer);
		html->print("</tr>\n");

		// Handle RF and Internet flags
		if (config.msg_rf && config.msg_inet)
		{
			html->print("<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"msgRf\" value=\"OK\" checked/>RF <input type=\"checkbox\" name=\"msgInet\" value=\"OK\" checked/>Internet </td></tr>\n");
		}
		else if (config.msg_rf)
		{
			html->print("<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"msgRf\" value=\"OK\" checked/>RF <input type=\"checkbox\" name=\"msgInet\" value=\"OK\" />Internet </td></tr>\n");
		}
		else if (config.msg_inet)
		{
			html->print("<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"msgRf\" value=\"OK\" />RF <input type=\"checkbox\" name=\"msgInet\" value=\"OK\" checked/>Internet </td></tr>\n");
		}
		else
		{
			html->print("<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"msgRf\" value=\"OK\" />RF <input type=\"checkbox\" name=\"msgInet\" value=\"OK\" />Internet </td></tr>\n");
		}

		html->print("<tr>");
		if (config.msg_encrypt)
		{
			html->print("<td align=\"right\"><b>Encryption</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"encrypt\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n");
		}
		else
		{
			html->print("<td align=\"right\"><b>Encryption</b></td>\n");
			html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"encrypt\" value=\"OK\" ><span class=\"slider round\"></span></label></td>\n");
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>AES Key:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  size=\"40\" maxlength=\"33\" name=\"key\" type=\"text\" value=\"%s\" /> *<i>ASCII HEX 16Byte</i></td>\n", config.msg_key);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Send Retry:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  min=\"0\" max=\"99\"   name=\"retry\" type=\"number\" value=\"%d\" /></td>\n", config.msg_retry);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Send Timeout:</b></td>\n");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input  min=\"1\" max=\"9999\"   name=\"timeout\" type=\"number\" value=\"%d\" /> Sec.</td>\n", config.msg_interval);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PATH:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"path\" id=\"path\">\n");
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" ", pthIdx);
			html->print(temp_buffer);
			if (config.msg_path == pthIdx)
			{
				html->print("selected>");
			}
			else
			{
				html->print(">");
			}
			snprintf(temp_buffer, sizeof(temp_buffer), "%s</option>\n", PATH_NAME[pthIdx]);
			html->print(temp_buffer);
		}
		html->print("</select></td>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitMSG'  name=\"commitMSG\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitMSG\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br /><br />");

		html->print("<table width=\"90%\">\n");
		html->print("<th style=\"background-color: #070ac2;\">CHAT MESSAGE</th>\n");

		html->print("<tr><td>\n");
		html->print("<table id=\"chatMsg\">\n");
		html->print(event_chatMessage(true).c_str());
		html->print("</table>\n");

		html->print("</td></tr><tr><td colspan=\"5\">");

		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formChat\" method=\"post\">\n");
		html->print("<table>\n");

		html->print("<tr>\n");
		html->print("<td align=\"left\"><b>TO:</b><input size=\"10\" name=\"toCall\" id=\"toCall\" type=\"text\" value=\"\" oninput=\"this.value=this.value.toUpperCase();\" /> <b>MSG:</b><input size=\"80\" name=\"msg\" id=\"msg\" type=\"text\" value=\"\" /></td>\n");
		html->print("<td align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitChat\" name=\"commitChat\" type=\"submit\" value=\"Send\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitChat\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form><br />\n");

		html->print("</td></tr></table><br />\n");

		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formBLN\" method=\"post\">\n");
		html->print("<table width=\"90%\" style=\"table-layout:fixed;border-collapse:collapse;\">\n");
		// Bulletins BLN1-BLN9 UI - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
		// v2.1.4-lu6jmf: BLN1-BLN4 = Alerts, NEWS5-NEWS9 = News. Numbers now match the real
		// on-air BLN digit (BLN5NEWS..BLN9NEWS) so the UI label and the raw packet correlate 1:1.
	html->print("<th colspan=\"7\" style=\"background-color: #070ac2;\"><span><b>Bulletins: BLN1-BLN4 Alerts, NEWS5-NEWS9</b></span></th>\n");
		html->print("<tr><td colspan=\"7\"><i>Uses the same TX Channel/PATH as Message Configuration above.</i></td></tr>\n");
		html->print("<tr>");
		html->print("<td align=\"center\" style=\"width:6%;\"><b>#</b></td>");
		html->print("<td align=\"center\" style=\"width:8%;\"><b>Enable</b></td>");
		html->print("<td align=\"center\" style=\"width:38%;\"><b>Text</b></td>");
		html->print("<td align=\"center\" style=\"width:14%;\"><b>Interval</b></td>");
		html->print("<td align=\"center\" style=\"width:12%;\"><b>Limit</b><br /><i style=\"font-weight:normal;font-size:8pt;\">0 = unlimited</i></td>");
		html->print("<td align=\"center\" style=\"width:14%;\"><b>Active for</b></td>");
		html->print("<td align=\"center\" style=\"width:8%;\"><b>Sent</b></td>");
		html->print("</tr>\n");
		for (uint8_t bi = 0; bi < 9; bi++)
		{
			char schedPreview[220];
			schedPreview[0] = 0; // v2.1.3-lu6jmf: filled for News rows, printed as its own full-width row below
			html->print("<tr>\n");
			if (bi < 4)
				snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><b>BLN%d</b></td>\n", bi + 1);
			else
				snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><b>NEWS%d</b></td>\n", bi + 1); // v2.1.4-lu6jmf: matches the on-air BLN digit (BLN5NEWS..BLN9NEWS)
			html->print(temp_buffer);

			if (config.bln_en[bi])
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><label class=\"switch\"><input type=\"checkbox\" name=\"blnEn%d\" value=\"OK\" checked><span class=\"slider round\"></span></label></td>\n", bi + 1);
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\"><label class=\"switch\"><input type=\"checkbox\" name=\"blnEn%d\" value=\"OK\"><span class=\"slider round\"></span></label></td>\n", bi + 1);
			}
			html->print(temp_buffer);

			{
				// v2.1-lu6jmf: live 'characters left' counter (maxlength already enforced server-side too)
				int blnCharsLeft = (int)(STATUS_SIZE - 1) - (int)strlen(config.bln_text[bi]);
				snprintf(temp_buffer, sizeof(temp_buffer),
					"<td style=\"text-align: left;\"><input id=\"blnTxt%d\" style=\"width:96%%;box-sizing:border-box;\" maxlength=\"%d\" name=\"blnText%d\" type=\"text\" value=\"%s\" "
					"oninput=\"var n=this.maxLength-this.value.length;var c=document.getElementById('blnCnt%d');c.textContent=n+' left';c.style.color=(n<0)?'red':'#2e7d32';\" /><br />"
					"<span id=\"blnCnt%d\" style=\"font-size:8pt;color:%s;\">%d left</span></td>\n",
					bi + 1, STATUS_SIZE - 1, bi + 1, config.bln_text[bi], bi + 1, bi + 1,
					(blnCharsLeft < 0) ? "red" : "#2e7d32", blnCharsLeft);
				html->print(temp_buffer);
			}

			{
				// v2.1.4-lu6jmf: Alerts and News now share the exact same closed dropdown -
				// 5/10/15/30/60 min (300/600/900/1800/3600s). For News this is just the base T;
				// the growing-gap math (below) still makes the real gaps grow from there.
				static const uint16_t intervalOpts[5] = {300, 600, 900, 1800, 3600};
				static const char *intervalLabels[5] = {"5 min", "10 min", "15 min", "30 min", "60 min"};
				char sel[600];
				snprintf(sel, sizeof(sel), "<td style=\"text-align: left;\"><select style=\"width:96%%;box-sizing:border-box;\" name=\"blnInv%d\">", bi + 1);
				html->print(sel);
				for (uint8_t k = 0; k < 5; k++)
				{
					snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\"%s>%s</option>",
						intervalOpts[k], (config.bln_interval[bi] == intervalOpts[k]) ? " selected" : "", intervalLabels[k]);
					html->print(temp_buffer);
				}
				html->print("</select></td>\n");

				if (bi >= 4)
				{
					// NEWS1-NEWS5: base T is now always one of the same 5 preset values, in seconds
					uint16_t baseTMin = config.bln_interval[bi] / 60;
					if (baseTMin < 5)
						baseTMin = 5; // floor: 5 min minimum, in case of an old saved value below that
					buildNewsSchedulePreview(schedPreview, sizeof(schedPreview), baseTMin);
				}
			}

			snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input style=\"width:90%%;box-sizing:border-box;\" min=\"0\" max=\"65535\" name=\"blnLim%d\" type=\"number\" value=\"%d\" /></td>\n", bi + 1, config.bln_limit[bi]);
			html->print(temp_buffer);

			{
				// v2.1-lu6jmf: Active for - 0 = default (72h Alerts / 24h News), else 24/36/48/72
				static const uint8_t afOpts[5] = {0, 24, 36, 48, 72};
				char afSel[500];
				snprintf(afSel, sizeof(afSel), "<td style=\"text-align: left;\"><select style=\"width:96%%;box-sizing:border-box;\" name=\"blnActFor%d\">", bi + 1);
				html->print(afSel);
				for (uint8_t k = 0; k < 5; k++)
				{
					if (afOpts[k] == 0)
						snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"0\"%s>Default (%dh)</option>",
							(config.bln_activefor[bi] == 0) ? " selected" : "", 24);
					else
						snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\"%s>%dh</option>",
							afOpts[k], (config.bln_activefor[bi] == afOpts[k]) ? " selected" : "", afOpts[k]);
					html->print(temp_buffer);
				}
				html->print("</select></td>\n");
			}

			snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"center\">%u</td>\n", blnSentCount[bi]);
			html->print(temp_buffer);

			html->print("</tr>\n");

			if (bi >= 4)
			{
				// v2.1.3-lu6jmf: schedule preview gets its own full-width row - the Interval
				// column is too narrow (14%) to show it inline without getting cut off
				snprintf(temp_buffer, sizeof(temp_buffer),
					"<tr><td colspan=\"2\"></td><td colspan=\"5\" style=\"text-align:left;font-size:7pt;color:#555;padding-top:0;padding-bottom:4px;\">sends: %s</td></tr>\n",
					schedPreview);
				html->print(temp_buffer);
			}
		}
		html->print("<tr><td colspan=\"7\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitBLN' name=\"commitBLN\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitBLN\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />\n");

		// --- v2.1-lu6jmf: Objects (Object1-Object4) ---
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formObj\" method=\"post\">\n");
		html->print("<table width=\"90%\" style=\"border-collapse:collapse;\">\n");
		html->print("<th colspan=\"2\" style=\"background-color: #070ac2;\"><span><b>Objects: Object1-Object4</b></span></th>\n");
		html->print("<tr><td colspan=\"2\"><i>Uses the same TX Channel/PATH as Message Configuration above.</i></td></tr>\n");
		for (uint8_t oi = 0; oi < 4; oi++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td colspan=\"2\" style=\"background-color:#eef0f4;\"><b>Object%d</b></td></tr>\n", oi + 1);
			html->print(temp_buffer);

			snprintf(temp_buffer, sizeof(temp_buffer),
				"<tr><td align=\"right\"><b>Enable:</b></td><td align=\"left\"><label class=\"switch\"><input type=\"checkbox\" name=\"objEn%d\" value=\"OK\"%s><span class=\"slider round\"></span></label></td></tr>\n",
				oi + 1, config.obj_en[oi] ? " checked" : "");
			html->print(temp_buffer);

			snprintf(temp_buffer, sizeof(temp_buffer),
				"<tr><td align=\"right\"><b>Item/Obj Name:</b></td><td align=\"left\"><input maxlength=\"9\" name=\"objName%d\" type=\"text\" value=\"%s\" />%s</td></tr>\n",
				oi + 1, config.obj_name[oi], (oi == 0) ? " <i>3-9 character</i>" : "");
			html->print(temp_buffer);

			snprintf(temp_buffer, sizeof(temp_buffer),
				"<tr><td align=\"right\"><b>Latitude / Longitude:</b></td><td align=\"left\">"
				// v2.1.4-lu6jmf: step="any" - "0.00001" rejected 6-decimal GPS coords pasted straight
				// from a phone/map (e.g. -32.474722), since that isn't an exact multiple of 0.00001
				"<input style=\"width:120px;\" step=\"any\" name=\"objLat%d\" type=\"number\" value=\"%.6f\" /> "
				"<input style=\"width:120px;\" step=\"any\" name=\"objLon%d\" type=\"number\" value=\"%.6f\" />%s</td></tr>\n",
				oi + 1, config.obj_lat[oi], oi + 1, config.obj_lon[oi], (oi == 0) ? " <i>independent of the digi's own position</i>" : "");
			html->print(temp_buffer);

			{
				// Symbol picker: same widget/pattern as Station Symbol (IGATE/DIGI/Tracker), generalized
				// via /symbol?sel=N + setValue(sel,symbol,table) - Object1-4 use sel 0-3 in this page's own script.
				char objTableCh = config.obj_symbol[oi][0] ? config.obj_symbol[oi][0] : '/';
				char objSymCh = config.obj_symbol[oi][1] ? config.obj_symbol[oi][1] : 'r';
				int objTableNum = (objTableCh == '\\') ? 2 : 1;
				snprintf(temp_buffer, sizeof(temp_buffer),
					"<tr><td align=\"right\"><b>Symbol:</b></td><td align=\"left\">Table:"
					"<input maxlength=\"1\" size=\"1\" id=\"obj%dTable\" name=\"obj%dTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" oninput=\"objSymRefresh(%d);\" /> Symbol:"
					"<input maxlength=\"1\" size=\"1\" id=\"obj%dSymbol\" name=\"obj%dSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" oninput=\"objSymRefresh(%d);\" /> "
					"<img border=\"1\" style=\"vertical-align: middle;\" id=\"obj%dImgSymbol\" onclick=\"openWindowSymbolObj(%d);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%d.png\">%s</td></tr>\n",
					oi + 1, oi + 1, objTableCh, oi, oi + 1, oi + 1, objSymCh, oi, oi + 1, oi, (int)objSymCh, objTableNum,
					(oi == 0) ? " <i>*Click icon or type Table/Symbol directly</i>" : "");
				html->print(temp_buffer);
			}

			{
				int objCharsLeft = (int)(STATUS_SIZE - 1) - (int)strlen(config.obj_text[oi]);
				snprintf(temp_buffer, sizeof(temp_buffer),
					"<tr><td align=\"right\"><b>Text/Comment:</b></td><td align=\"left\"><input style=\"width:96%%;box-sizing:border-box;\" maxlength=\"%d\" name=\"objText%d\" type=\"text\" value=\"%s\" "
					"oninput=\"var n=this.maxLength-this.value.length;var c=document.getElementById('objCnt%d');c.textContent=n+' left';c.style.color=(n<0)?'red':'#2e7d32';\" /><br />"
					"<span id=\"objCnt%d\" style=\"font-size:8pt;color:%s;\">%d left</span>%s</td></tr>\n",
					STATUS_SIZE - 1, oi + 1, config.obj_text[oi], oi + 1, oi + 1, (objCharsLeft < 0) ? "red" : "#2e7d32", objCharsLeft,
					(oi == 0) ? " <i style=\"font-size:8pt;\">(own buffer, not shared)</i>" : "");
				html->print(temp_buffer);
			}

			{
				bool fixedMode = (config.obj_mode[oi] == 0);
				static const uint16_t objIvlOpts[3] = {900, 1800, 3600};
				static const char *objIvlLabels[3] = {"15 min", "30 min", "60 min"};
				static const uint8_t objAfOpts[4] = {24, 36, 48, 72};
				snprintf(temp_buffer, sizeof(temp_buffer),
					"<tr><td align=\"right\"><b>Interval:</b></td><td align=\"left\">"
					"<input type=\"radio\" name=\"objMode%d\" value=\"0\"%s> Fixed: <select name=\"objInv%d\">",
					oi + 1, fixedMode ? " checked" : "", oi + 1);
				html->print(temp_buffer);
				for (uint8_t k = 0; k < 3; k++)
				{
					snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\"%s>%s</option>",
						objIvlOpts[k], (config.obj_interval[oi] == objIvlOpts[k]) ? " selected" : "", objIvlLabels[k]);
					html->print(temp_buffer);
				}
				snprintf(temp_buffer, sizeof(temp_buffer),
					"</select> + max sends <input style=\"width:60px;\" min=\"0\" max=\"65535\" name=\"objLim%d\" type=\"number\" value=\"%d\" /><br />"
					"<input type=\"radio\" name=\"objMode%d\" value=\"1\"%s> Active for: <select name=\"objActFor%d\">",
					oi + 1, config.obj_limit[oi], oi + 1, fixedMode ? "" : " checked", oi + 1);
				html->print(temp_buffer);
				for (uint8_t k = 0; k < 4; k++)
				{
					snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\"%s>%dh</option>",
						objAfOpts[k], (config.obj_activefor[oi] == objAfOpts[k]) ? " selected" : "", objAfOpts[k]);
					html->print(temp_buffer);
				}
				if (oi == 0)
					html->print("</select><br /><i>either mode (Permanent off): never exceeds 72h total send time</i></td></tr>\n");
				else
					html->print("</select></td></tr>\n");
			}

			snprintf(temp_buffer, sizeof(temp_buffer),
				"<tr><td align=\"right\"><b>Permanent / Never disable:</b></td><td align=\"left\"><label class=\"switch\"><input type=\"checkbox\" name=\"objPerm%d\" value=\"OK\"%s><span class=\"slider round\"></span></label>%s</td></tr>\n",
				oi + 1, config.obj_permanent[oi] ? " checked" : "", (oi == 0) ? " <i>if checked, ignores Interval and the 72h cap entirely</i>" : "");
			html->print(temp_buffer);

			html->print((oi == 0)
				? "<tr><td align=\"right\"><b>Timestamp:</b></td><td align=\"left\"><i>always UTC (zulu), automatic - not user configurable</i></td></tr>\n"
				: "<tr><td align=\"right\"><b>Timestamp:</b></td><td align=\"left\"><i>always UTC, automatic</i></td></tr>\n");
		}
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitObj' name=\"commitObj\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitObj\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />\n");

		html->print("<script type=\"text/javascript\">\n");
		html->print("function openWindowSymbolObj(sel) {\n");
		html->print("window.open(\"/symbol?sel=\"+sel.toString(), null, \"height=400,width=400,status=no,toolbar=no,menubar=no,location=no\");\n");
		html->print("}\n");
		html->print("function setValue(sel,symbol,table) {\n");
		html->print("var txtsymbol=document.getElementById('obj'+(sel+1)+'Symbol');\n");
		html->print("var txttable=document.getElementById('obj'+(sel+1)+'Table');\n");
		html->print("var imgicon=document.getElementById('obj'+(sel+1)+'ImgSymbol');\n");
		html->print("txtsymbol.value = String.fromCharCode(symbol);\n");
		html->print("if(table==1){\n txttable.value='/';\n");
		html->print("}else if(table==2){\n txttable.value='\\\\';\n}\n");
		html->print("imgicon.src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n");
		html->print("}\n");
		html->print("function objSymRefresh(sel) {\n");
		html->print("var txtsymbol=document.getElementById('obj'+(sel+1)+'Symbol');\n");
		html->print("var txttable=document.getElementById('obj'+(sel+1)+'Table');\n");
		html->print("var imgicon=document.getElementById('obj'+(sel+1)+'ImgSymbol');\n");
		html->print("var sym=txtsymbol.value.charCodeAt(0);\n");
		html->print("var tbl=(txttable.value=='\\\\')?2:1;\n");
		html->print("if(!isNaN(sym)&&sym>0){\n imgicon.src = \"http://aprs.nakhonthai.net/symbols/icons/\"+sym.toString()+'-'+tbl.toString()+'.png';\n}\n");
		html->print("}\n");
		html->print("</script>\n");

		html->addHeader("MSG", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
		// Streamed response: avoids a single large calloc() for the whole page
		// (same fix already applied to handle_igate/wx/sensor/etc after we
		// confirmed via live heap-integrity diagnostics that the single big
		// contiguous allocation was the reproducible trigger for a heap
		// corruption assert under real traffic).
		AsyncResponseStream *html = request->beginResponseStream("text/html", 55000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}

		// Initialize the HTML string with the JavaScript code
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formUART0\") document.getElementById(\"submitURAT0\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formUART1\") document.getElementById(\"submitURAT1\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formUART1\") document.getElementById(\"submitURAT1\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formGNSS\") document.getElementById(\"submitGNSS\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formMODBUS\") document.getElementById(\"submitMODBUS\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formTNC\") document.getElementById(\"submitTNC\").disabled=true;\n");
		// html->print("if(e.currentTarget.id===\"formONEWIRE\") document.getElementById(\"submitONEWIRE\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formRF\") document.getElementById(\"submitRF\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formI2C0\") document.getElementById(\"submitI2C0\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formI2C1\") document.getElementById(\"submitI2C1\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formCOUNT0\") document.getElementById(\"submitCOUNT0\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formCOUNT1\") document.getElementById(\"submitCOUNT1\").disabled=true;\n");
#ifdef PPPOS
		html->print("if(e.currentTarget.id===\"formPPPoS\") document.getElementById(\"submitPPPoS\").disabled=true;\n");
#endif
		html->print("$.ajax({\n");
		html->print("url: '/mod',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\\nRequire hardware REBOOT!\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"32%\" style=\"border:unset;\">\n");
		// html->print("<h2>System Setting</h2>\n");
		/**************UART0(USB) Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromUART0\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>UART0 Modify</b></span></th>\n");
		html->print("<tr>\n");

		char enFlage[20] = "";
		if (config.uart0_enable)
			strcpy(enFlage, "checked");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(enFlage) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", enFlage);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rx_gpio_str = StringToCharPtr(String(config.uart0_rx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"rx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"rx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rx_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *tx_gpio_str = StringToCharPtr(String(config.uart0_tx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"tx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(tx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"tx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, tx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(tx_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RTS/DE GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rts_gpio_str = StringToCharPtr(String(config.uart0_rts_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"rts\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rts_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"rts\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rts_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rts_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Baudrate:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"baudrate\" id=\"baudrate\">\n");
		for (int i = 0; i < 13; i++)
		{
			char *baudrate_str = StringToCharPtr(String(baudrate[i]));
			if (config.uart0_baudrate == baudrate[i])
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" selected>  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" selected>%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			else
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" >  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" >%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			free(baudrate_str);
		}
		html->print("</select> bps\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitUART0\" name=\"commitUART0\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitUART0\"/>\n");
		html->print("</td></tr></table>\n");

		html->print("</form><br />\n");
		html->print("</td><td width=\"32%\" style=\"border:unset;\">");

		/**************UART1 Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromUART1\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>UART1 Modify</b></span></th>\n");
		html->print("<tr>");

		if (config.uart1_enable)
			strcpy(enFlage, "checked");
		else
			strcpy(enFlage, "");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(enFlage) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", enFlage);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rx_gpio_str = StringToCharPtr(String(config.uart1_rx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"rx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"rx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rx_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *tx_gpio_str = StringToCharPtr(String(config.uart1_tx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"tx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(tx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"tx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, tx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(tx_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RTS/DE GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rts_gpio_str = StringToCharPtr(String(config.uart1_rts_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"rts\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rts_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"rts\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rts_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rts_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Baudrate:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"baudrate\" id=\"baudrate\">\n");
		for (int i = 0; i < 13; i++)
		{
			char *baudrate_str = StringToCharPtr(String(baudrate[i]));
			if (config.uart1_baudrate == baudrate[i])
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" selected>  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" selected>%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			else
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" >  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" >%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			free(baudrate_str);
		}
		html->print("</select> bps\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitUART1\" name=\"commitUART1\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitUART1\"/>\n");
		html->print("</td></tr></table>\n");

		html->print("</form><br />\n");
		// html->print("</td><td width=\"32%\" style=\"border:unset;\">");

		/**************UART2 Modify******************/
		// html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromUART2\" method=\"post\">\n");
		// html += "<table>\n";
		// html += "<th colspan=\"2\"><span><b>UART2 Modify</b></span></th>\n";
		// html += "<tr>";

		// enFlage = "";
		// if (config.uart2_enable)
		// 	enFlage = "checked";
		// html += "<td align=\"right\"><b>Enable</b></td>\n";
		// html += "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" " + enFlage + "><span class=\"slider round\"></span></label></td>\n";
		// html += "</tr>\n";

		// html += "<tr>\n";
		// html += "<td align=\"right\"><b>RX GPIO:</b></td>\n";
		// html += "<td style=\"text-align: left;\"><input min=\"-1\" max=\""+String(GPIO_NUM_MAX)+"\" name=\"rx\" type=\"number\" value=\"" + String(config.uart2_rx_gpio) + "\" /></td>\n";
		// html += "</tr>\n";

		// html += "<tr>\n";
		// html += "<td align=\"right\"><b>TX GPIO:</b></td>\n";
		// html += "<td style=\"text-align: left;\"><input min=\"-1\" max=\""+String(GPIO_NUM_MAX)+"\" name=\"tx\" type=\"number\" value=\"" + String(config.uart2_tx_gpio) + "\" /></td>\n";
		// html += "</tr>\n";

		// html += "<tr>\n";
		// html += "<td align=\"right\"><b>RTS/DE GPIO:</b></td>\n";
		// html += "<td style=\"text-align: left;\"><input min=\"-1\" max=\""+String(GPIO_NUM_MAX)+"\"  name=\"rts\" type=\"number\" value=\"" + String(config.uart2_rts_gpio) + "\" /></td>\n";
		// html += "</tr>\n";

		// html += "<tr>\n";
		// html += "<td align=\"right\"><b>Baudrate:</b></td>\n";
		// html += "<td style=\"text-align: left;\">\n";
		// html += "<select name=\"baudrate\" id=\"baudrate\">\n";
		// for (int i = 0; i < 13; i++)
		// {
		// 	if (config.uart2_baudrate == baudrate[i])
		// 		html += "<option value=\"" + String(baudrate[i]) + "\" selected>" + String(baudrate[i]) + " </option>\n";
		// 	else
		// 		html += "<option value=\"" + String(baudrate[i]) + "\" >" + String(baudrate[i]) + " </option>\n";
		// }
		// html += "</select> bps\n";
		// html += "</td>\n";
		// html += "</tr>\n";
		// html += "<tr><td colspan=\"2\" align=\"right\">\n";
		// html += "<input class=\"btn btn-primary\" id=\"submitUART2\" name=\"commitUART2\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n";
		// html += "<input type=\"hidden\" name=\"commitUART2\"/>\n";
		// html += "</td></tr></table>\n";

		// html += "</form><br />\n";
		// html += "</td></tr></table>\n";

		html->print("</td></tr></table>\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">");
		/**************RF GPIO******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromRF\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>RF GPIO Modify</b></span></th>\n");
		html->print("<tr>");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>ADC Attenuation:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"atten\" id=\"atten\">\n");
		for (int i = 0; i < 5; i++)
		{
			char *i_str = StringToCharPtr(String(i));
			char *atten_str = StringToCharPtr(String(ADC_ATTEN[i]));
			if (config.adc_atten == i)
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" selected>  </option>\n") + strlen(i_str) + strlen(atten_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" selected>%s </option>\n", i_str, atten_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			else
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" >  </option>\n") + strlen(i_str) + strlen(atten_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" >%s </option>\n", i_str, atten_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			free(i_str);
			free(atten_str);
		}
		{
			char *offset_str = StringToCharPtr(String(offset));
			char *temp_html = allocateStringMemory(strlen("</select> DC-Offset:  mV\n") + strlen(offset_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "</select> DC-Offset: %s mV\n", offset_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(offset_str);
		}
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UART Baudrate:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"baudrate\" id=\"baudrate\">\n");
		for (int i = 0; i < 13; i++)
		{
			char *baudrate_str = StringToCharPtr(String(baudrate[i]));
			if (config.rf_baudrate == baudrate[i])
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" selected>  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" selected>%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			else
			{
				char *temp_html = allocateStringMemory(strlen("<option value=\"\" >  </option>\n") + 2 * strlen(baudrate_str) + 1);
				if (temp_html)
				{
					sprintf(temp_html, "<option value=\"%s\" >%s </option>\n", baudrate_str, baudrate_str);
					html->print(temp_html);
					free(temp_html);
				}
				else
				{
					log_e("MOD page: temp_html allocation failed, skipping field");
				}
			}
			free(baudrate_str);
		}
		html->print("</select> bps\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		// html += "<tr>\n";
		// html += "<td align=\"right\"><b>ADC DC OFFSET:</b></td>\n";
		// html += "<td style=\"text-align: left;\"><input min=\"100\" max=\"2500\" name=\"offset\" type=\"number\" value=\"" + String(config.adc_dc_offset) + "\" /> mV     (Current: " + String(offset) + " mV)</td>\n";
		// html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UART RX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_rx_gpio_str = StringToCharPtr(String(config.rf_rx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"rx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rf_rx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"rx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rf_rx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_rx_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UART TX GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_tx_gpio_str = StringToCharPtr(String(config.rf_tx_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"tx\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(rf_tx_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"tx\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, rf_tx_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_tx_gpio_str);
		}
		html->print("</tr>\n");

		char LowFlag[20] = "", HighFlag[20] = "";
		strcpy(LowFlag, "");
		strcpy(HighFlag, "");
		if (config.rf_pd_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PD GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_pd_gpio_str = StringToCharPtr(String(config.rf_pd_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"pd\" type=\"number\" value=\"\" /> Active:<input type=\"radio\" name=\"pd_active\" value=\"0\"  />LOW <input type=\"radio\" name=\"pd_active\" value=\"1\"  />HIGH </td>\n") + strlen(gpio_max_str) + strlen(rf_pd_gpio_str) + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"pd\" type=\"number\" value=\"%s\" /> Active:<input type=\"radio\" name=\"pd_active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"pd_active\" value=\"1\" %s/>HIGH </td>\n", gpio_max_str, rf_pd_gpio_str, LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_pd_gpio_str);
		}
		html->print("</tr>\n");

		sprintf(LowFlag, "");
		sprintf(HighFlag, "");
		if (config.rf_pwr_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>H/L GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_pwr_gpio_str = StringToCharPtr(String(config.rf_pwr_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"pwr\" type=\"number\" value=\"\" /> Active:<input type=\"radio\" name=\"pwr_active\" value=\"0\"  />LOW <input type=\"radio\" name=\"pwr_active\" value=\"1\"  />HIGH </td>\n") + strlen(gpio_max_str) + strlen(rf_pwr_gpio_str) + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"pwr\" type=\"number\" value=\"%s\" /> Active:<input type=\"radio\" name=\"pwr_active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"pwr_active\" value=\"1\" %s/>HIGH </td>\n", gpio_max_str, rf_pwr_gpio_str, LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_pwr_gpio_str);
		}
		html->print("</tr>\n");

		sprintf(LowFlag, "");
		sprintf(HighFlag, "");
		if (config.rf_sql_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SQL GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_sql_gpio_str = StringToCharPtr(String(config.rf_sql_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"sql\" type=\"number\" value=\"\" /> Active:<input type=\"radio\" name=\"sql_active\" value=\"0\"  />LOW <input type=\"radio\" name=\"sql_active\" value=\"1\"  />HIGH </td>\n") + strlen(gpio_max_str) + strlen(rf_sql_gpio_str) + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"sql\" type=\"number\" value=\"%s\" /> Active:<input type=\"radio\" name=\"sql_active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"sql_active\" value=\"1\" %s/>HIGH </td>\n", gpio_max_str, rf_sql_gpio_str, LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_sql_gpio_str);
		}
		html->print("</tr>\n");

		sprintf(LowFlag, "");
		sprintf(HighFlag, "");
		if (config.rf_ptt_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PTT GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *rf_ptt_gpio_str = StringToCharPtr(String(config.rf_ptt_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\"  name=\"ptt\" type=\"number\" value=\"\" /> Active:<input type=\"radio\" name=\"ptt_active\" value=\"0\"  />LOW <input type=\"radio\" name=\"ptt_active\" value=\"1\"  />HIGH </td>\n") + strlen(gpio_max_str) + strlen(rf_ptt_gpio_str) + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\"  name=\"ptt\" type=\"number\" value=\"%s\" /> Active:<input type=\"radio\" name=\"ptt_active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"ptt_active\" value=\"1\" %s/>HIGH </td>\n", gpio_max_str, rf_ptt_gpio_str, LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(rf_ptt_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitRF\" name=\"commitRF\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitRF\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");

		html->print("</td></tr></table>\n");
		html->print("<br />\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">\n");

		/**************External TNC Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromTNC\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>External TNC Modify</b></span></th>\n");
		html->print("<tr>\n");

		strcpy(enFlage, "");
		if (config.ext_tnc_enable)
			strcpy(enFlage, "checked");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ");
		html->print(enFlage);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"channel\" id=\"channel\">\n");
		for (int i = 0; i < 5; i++)
		{
			html->print("<option value=\"");
			char *iStr4 = intToString(i);
			html->print(iStr4);
			html->print("\" ");
			if (config.ext_tnc_channel == i)
			{
				html->print("selected>");
				html->print(TNC_PORT[i]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(TNC_PORT[i]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr4);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>MODE:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"mode\" id=\"mode\">\n");
		for (int i = 0; i < 4; i++)
		{
			html->print("<option value=\"");
			char *iStr5 = intToString(i);
			html->print(iStr5);
			html->print("\" ");
			if (config.ext_tnc_mode == i)
			{
				html->print("selected>");
				html->print(TNC_MODE[i]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(TNC_MODE[i]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr5);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitTNC\" name=\"commitTNC\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitTNC\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");

		/**************TCP KISS Server Modify******************/
		// custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromTCPKISS\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>TCP KISS Server Modify</b></span></th>\n");
		html->print("<tr>\n");

		strcpy(enFlage, "");
		if (config.tcp_kiss_enable)
			strcpy(enFlage, "checked");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ");
		html->print(enFlage);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT 1:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input type=\"text\" name=\"port1\" value=\"");
		html->print(String(config.tcp_kiss_port1).c_str());
		html->print("\" maxlength=\"5\" size=\"6\"/></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT 2:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input type=\"text\" name=\"port2\" value=\"");
		html->print(String(config.tcp_kiss_port2).c_str());
		html->print("\" maxlength=\"5\" size=\"6\"/></td>\n");
		html->print("</tr>\n");

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
			html->print("<tr><td colspan=\"2\" align=\"center\"><b>Status:</b></td></tr>\n");
			html->print("<tr><td colspan=\"2\" align=\"center\">");
			html->print(tcpKissStatus1.c_str());
			html->print("</td></tr>\n");
			html->print("<tr><td colspan=\"2\" align=\"center\">");
			html->print(tcpKissStatus2.c_str());
			html->print("</td></tr>\n");
		}

		html->print("<tr><td colspan=\"2\" style=\"word-wrap:break-word;white-space:normal;\"><p style=\"font-size:9pt;margin:4px 0;\">Lets PC software (Xastir, APRSIS32, etc) use this device as a KISS TNC over the local network - no cable needed. No password - anyone on this WiFi network can transmit through the radio via these ports while enabled. Port number changes need a reboot to take effect; the Enable switch does not.</p></td></tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitTCPKISS\" name=\"commitTCPKISS\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitTCPKISS\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		html->print("</td></tr></table>\n");
		html->print("<br />\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">\n");

		/************************ AT-COMMAND **************************/
		html->print("<form id='formATCommand' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>AT-COMMAND CHANNEL</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td width=\"150\" align=\"right\"><b>MQTT:</b></td>\n");
		char cmdFlag[20] = "";
		if (config.at_cmd_mqtt)
			strcpy(cmdFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"mqtt\" value=\"OK\" ");
		html->print(cmdFlag);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>MESSAGE:</b></td>\n");
		strcpy(cmdFlag, "");
		if (config.at_cmd_msg)
			strcpy(cmdFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"msg\" value=\"OK\" ");
		html->print(cmdFlag);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>BLUETOOTH:</b></td>\n");
		strcpy(cmdFlag, "");
		if (config.at_cmd_bluetooth)
			strcpy(cmdFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"bluetooth\" value=\"OK\" ");
		html->print(cmdFlag);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UART PORT:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"uart\" id=\"cmdUart\">\n");
		for (int i = 0; i < 5; i++)
		{
			html->print("<option value=\"");
			char *iStr6 = intToString(i);
			html->print(iStr6);
			html->print("\" ");
			if (config.at_cmd_uart == i)
			{
				html->print("selected>");
				html->print(TNC_PORT[i]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(TNC_PORT[i]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr6);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitCMD'  name=\"commit\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitCMD\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />\n");

#ifdef PPPOS
		html->print("<br />\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">\n");

		/************************ PPPoS **************************/

		html->print("<form id='formPPPoS' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>PPP Over Serial (GSM/4G-LTE)</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");
		char pppEnFlag[20] = "";
		if (config.ppp_enable)
			strcpy(pppEnFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"pppEn\" value=\"OK\" ");
		html->print(pppEnFlag);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");
		html->print("<td align=\"right\"><b>GNSS:</b></td>\n");
		strcpy(pppEnFlag, "");
		if (config.ppp_gnss)
			strcpy(pppEnFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"pppGnss\" value=\"OK\" ");
		html->print(pppEnFlag);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>NAPT:</b></td>\n");
		strcpy(pppEnFlag, "");
		if (config.ppp_napt)
			strcpy(pppEnFlag, "checked");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"pppNapt\" value=\"OK\" ");
		html->print(pppEnFlag);
		html->print("><span class=\"slider round\"></span></label> *WiFi NAT</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>APN:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input maxlength=\"20\" name=\"pppAPN\" type=\"text\" value=\"");
		html->print(config.ppp_apn);
		html->print("\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");

		html->print("<td align=\"right\"><b>PIN:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"0\" max=\"999999\" name=\"pppPin\" type=\"number\" value=\"");
		char *pppPinStr = intToString(atoi(config.ppp_pin));
		html->print(pppPinStr);
		free(pppPinStr);
		html->print("\" /> <i>*PIN of SIM</i></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");

		html->print("<td align=\"right\"><b>RX GPIO:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"-1\" max=\"50\" name=\"rx\" type=\"number\" value=\"");
		char *pppRxStr = intToString(config.ppp_rx_gpio);
		html->print(pppRxStr);
		free(pppRxStr);
		html->print("\" /></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TX GPIO:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"-1\" max=\"50\" name=\"tx\" type=\"number\" value=\"");
		char *pppTxStr = intToString(config.ppp_tx_gpio);
		html->print(pppTxStr);
		free(pppTxStr);
		html->print("\" /></td>\n");
		html->print("</tr>\n");

		strcpy(LowFlag, "");
		strcpy(HighFlag, "");
		if (config.ppp_rst_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RESET GPIO:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"-1\" max=\"50\"  name=\"rst\" type=\"number\" value=\"");
		char *pppRstStr = intToString(config.ppp_rst_gpio);
		html->print(pppRstStr);
		free(pppRstStr);
		html->print("\" /> Active:<input type=\"radio\" name=\"rst_active\" value=\"0\" ");
		html->print(LowFlag);
		html->print("/>LOW <input type=\"radio\" name=\"rst_active\" value=\"1\" ");
		html->print(HighFlag);
		html->print("/>HIGH </td>\n");
		html->print("</tr>\n");

		html->print("<td align=\"right\"><b>RESET DELAY:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"0\" max=\"999999\" name=\"rstDly\" type=\"number\" value=\"");
		char *pppRstDelayStr = intToString(config.ppp_rst_delay);
		html->print(pppRstDelayStr);
		free(pppRstDelayStr);
		html->print("\" /> mSec.</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"port\" id=\"port\">\n");
		for (int i = 0; i < 2; i++)
		{
			html->print("<option value=\"");
			char *iStr7 = intToString(i);
			html->print(iStr7);
			html->print("\" ");
			if (config.ppp_serial == i)
			{
				html->print("selected>");
				html->print(GNSS_PORT[i + 1]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(GNSS_PORT[i + 1]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr7);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Baudrate:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"baudrate\" id=\"baudrate\">\n");
		for (int i = 0; i < 13; i++)
		{
			html->print("<option value=\"");
			char *baudrateStr3 = intToString(baudrate[i]);
			html->print(baudrateStr3);
			html->print("\" ");
			if (config.ppp_serial_baudrate == baudrate[i])
			{
				html->print("selected>");
				html->print(baudrateStr3);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(baudrateStr3);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(baudrateStr3);
		}
		html->print("</select> bps\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitPPPoS'  name=\"commitPPPoS\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitPPPoS\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form>\n");

		html->print("</td></tr></table>\n");
#endif

		html->addHeader("MOD", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 46000);
		if (html == NULL)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}

		// Locals reused across several sections below (declared once here since
		// the sections that originally declared them stayed in handle_mod when
		// this function was split out).
		char enFlage[20] = "";
		char LowFlag[20] = "", HighFlag[20] = "";

		// Initialize the HTML string with the JavaScript code
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formGNSS\") document.getElementById(\"submitGNSS\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formMODBUS\") document.getElementById(\"submitMODBUS\").disabled=true;\n");
		// html->print("if(e.currentTarget.id===\"formONEWIRE\") document.getElementById(\"submitONEWIRE\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formI2C0\") document.getElementById(\"submitI2C0\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formI2C1\") document.getElementById(\"submitI2C1\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formCOUNT0\") document.getElementById(\"submitCOUNT0\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formCOUNT1\") document.getElementById(\"submitCOUNT1\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/mod2',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\\nRequire hardware REBOOT!\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">");
		/**************1-Wire Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromONEWIRE\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>1-Wire Bus Modify</b></span></th>\n");
		html->print("<tr>");

		String syncFlage = "";
		if (config.onewire_enable)
			syncFlage = "checked";
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(syncFlage.c_str()) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", syncFlage.c_str());
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *onewire_gpio_str = StringToCharPtr(String(config.onewire_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"data\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(onewire_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"data\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, onewire_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(onewire_gpio_str);
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitONEWIRE\" name=\"commitONEWIRE\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitONEWIRE\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form><br />\n");
		html->print("</td></tr></table>\n");
		html->print("<br />\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">");
		/**************I2C_0 Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromI2C0\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>I2C_0(OLED) Modify</b></span></th>\n");
		html->print("<tr>");

		syncFlage = "";
		if (config.i2c_enable)
			syncFlage = "checked";
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(syncFlage.c_str()) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", syncFlage.c_str());
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SDA GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *i2c_sda_pin_str = StringToCharPtr(String(config.i2c_sda_pin));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"sda\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(i2c_sda_pin_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"sda\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, i2c_sda_pin_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(i2c_sda_pin_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SCK GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *i2c_sck_pin_str = StringToCharPtr(String(config.i2c_sck_pin));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"sck\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(i2c_sck_pin_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"sck\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, i2c_sck_pin_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(i2c_sck_pin_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Frequency:</b></td>\n");
		{
			char *freq_str = StringToCharPtr(String(config.i2c_freq));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"1000\" max=\"800000\" name=\"freq\" type=\"number\" value=\"\" /></td>\n") + strlen(freq_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"1000\" max=\"800000\" name=\"freq\" type=\"number\" value=\"%s\" /></td>\n", freq_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(freq_str);
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitI2C0\" name=\"commitI2C0\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitI2C0\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		/**************Counter_0 Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromCOUNTER0\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>Counter_0 Modify</b></span></th>\n");
		html->print("<tr>");

		syncFlage = "";
		if (config.counter0_enable)
			syncFlage = "checked";
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(syncFlage.c_str()) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", syncFlage.c_str());
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>INPUT GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *counter0_gpio_str = StringToCharPtr(String(config.counter0_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"gpio\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(counter0_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"gpio\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, counter0_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(counter0_gpio_str);
		}
		html->print("</tr>\n");

		sprintf(LowFlag, "");
		sprintf(HighFlag, "");
		if (config.counter0_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Active</td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input type=\"radio\" name=\"active\" value=\"0\"  />LOW <input type=\"radio\" name=\"active\" value=\"1\"  />HIGH </td>\n") + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input type=\"radio\" name=\"active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"active\" value=\"1\" %s/>HIGH </td>\n", LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitCOUNTER0\" name=\"commitCOUNTER0\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitCOUNTER0\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		html->print("</td><td width=\"50%\" style=\"border:unset;\">");
		/**************I2C_1 Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromI2C1\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>I2C_1 Modify</b></span></th>\n");
		html->print("<tr>");

		syncFlage = "";
		if (config.i2c1_enable)
			syncFlage = "checked";
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(syncFlage.c_str()) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", syncFlage.c_str());
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SDA GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *i2c1_sda_pin_str = StringToCharPtr(String(config.i2c1_sda_pin));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"sda\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(i2c1_sda_pin_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"sda\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, i2c1_sda_pin_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(i2c1_sda_pin_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>SCK GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *i2c1_sck_pin_str = StringToCharPtr(String(config.i2c1_sck_pin));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"sck\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(i2c1_sck_pin_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"sck\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, i2c1_sck_pin_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(i2c1_sck_pin_str);
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Frequency:</b></td>\n");
		{
			char *freq_str = StringToCharPtr(String(config.i2c1_freq));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"1000\" max=\"800000\" name=\"freq\" type=\"number\" value=\"\" /></td>\n") + strlen(freq_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"1000\" max=\"800000\" name=\"freq\" type=\"number\" value=\"%s\" /></td>\n", freq_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(freq_str);
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitI2C1\" name=\"commitI2C1\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitI2C1\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		/**************Counter_1 Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromCOUNTER1\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>Counter_1 Modify</b></span></th>\n");
		html->print("<tr>");

		syncFlage = "";
		if (config.counter1_enable)
			syncFlage = "checked";
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ") + strlen(syncFlage.c_str()) + strlen("\"><span class=\"slider round\"></span></label></td>\n") + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", syncFlage.c_str());
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>INPUT GPIO:</b></td>\n");
		{
			char *gpio_max_str = StringToCharPtr(String(GPIO_NUM_MAX));
			char *counter1_gpio_str = StringToCharPtr(String(config.counter1_gpio));
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input min=\"-1\" max=\"\" name=\"gpio\" type=\"number\" value=\"\" /></td>\n") + strlen(gpio_max_str) + strlen(counter1_gpio_str) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input min=\"-1\" max=\"%s\" name=\"gpio\" type=\"number\" value=\"%s\" /></td>\n", gpio_max_str, counter1_gpio_str);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
			free(gpio_max_str);
			free(counter1_gpio_str);
		}
		html->print("</tr>\n");

		sprintf(LowFlag, "");
		sprintf(HighFlag, "");
		if (config.counter1_active)
			strcpy(HighFlag, "checked=\"checked\"");
		else
			strcpy(LowFlag, "checked=\"checked\"");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Active</td>\n");
		{
			char *temp_html = allocateStringMemory(strlen("<td style=\"text-align: left;\"><input type=\"radio\" name=\"active\" value=\"0\"  />LOW <input type=\"radio\" name=\"active\" value=\"1\"  />HIGH </td>\n") + strlen(LowFlag) + strlen(HighFlag) + 1);
			if (temp_html)
			{
				sprintf(temp_html, "<td style=\"text-align: left;\"><input type=\"radio\" name=\"active\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"active\" value=\"1\" %s/>HIGH </td>\n", LowFlag, HighFlag);
				html->print(temp_html);
				free(temp_html);
			}
			else
			{
				log_e("MOD page: temp_html allocation failed, skipping field");
			}
		}
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitCOUNTER1\" name=\"commitCOUNTER1\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitCOUNTER1\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		html->print("</td></tr></table>\n");
		html->print("<br />\n");

		html->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;vertical-align:top\"><td width=\"50%\" style=\"border:unset;vertical-align:top\">");
		/**************GNSS Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromGNSS\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>GNSS Modify</b></span></th>\n");
		html->print("<tr>\n");

		strcpy(enFlage, "");
		if (config.gnss_enable)
			strcpy(enFlage, "checked");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ");
		html->print(enFlage);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"channel\" id=\"channel\">\n");
		for (int i = 0; i < 5; i++)
		{
			html->print("<option value=\"");
			char *iStr2 = intToString(i);
			html->print(iStr2);
			html->print("\" ");
			if (config.gnss_channel == i)
			{
				html->print("selected>");
				html->print(GNSS_PORT[i]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(GNSS_PORT[i]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr2);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<td align=\"right\"><b>AT Command:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input maxlength=\"30\" size=\"20\" id=\"atc\" name=\"atc\" type=\"text\" value=\"");
		html->print(config.gnss_at_command);
		html->print("\" /></td>\n");
		html->print("</tr>\n");
		html->print("<td align=\"right\"><b>TCP Host:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input maxlength=\"20\" size=\"15\" id=\"Host\" name=\"Host\" type=\"text\" value=\"");
		html->print(config.gnss_tcp_host);
		html->print("\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>TCP Port:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"1024\" max=\"65535\"  id=\"Port\" name=\"Port\" type=\"number\" value=\"");
		char *gnssTcpPortStr = intToString(config.gnss_tcp_port);
		html->print(gnssTcpPortStr);
		free(gnssTcpPortStr);
		html->print("\" /></td>\n");
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitGNSS\" name=\"commitGNSS\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitGNSS\"/>\n");
		html->print("</td></tr></table>\n");

		html->print("</form><br />\n");
		html->print("</td><td width=\"50%\" style=\"border:unset;\">");
		/**************MODBUS Modify******************/
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"fromMODBUS\" method=\"post\">\n");
		html->print("<table>\n");
		esp_task_wdt_reset(); // feed watchdog: handle_mod runs on the async_tcp task itself,
		// and this monolithic page can take long enough under heap pressure to trip the TWDT.
		html->print("<th colspan=\"2\"><span><b>MODBUS Modify</b></span></th>\n");
		html->print("<tr>\n");

		strcpy(enFlage, "");
		if (config.modbus_enable)
			strcpy(enFlage, "checked");
		html->print("<td align=\"right\"><b>Enable</b></td>\n");
		html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" ");
		html->print(enFlage);
		html->print("><span class=\"slider round\"></span></label></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PORT:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"channel\" id=\"channel\">\n");
		for (int i = 0; i < 5; i++)
		{
			html->print("<option value=\"");
			char *iStr3 = intToString(i);
			html->print(iStr3);
			html->print("\" ");
			if (config.modbus_channel == i)
			{
				html->print("selected>");
				html->print(GNSS_PORT[i]);
				html->print(" </option>\n");
			}
			else
			{
				html->print(">");
				html->print(GNSS_PORT[i]);
				html->print(" </option>\n");
			}
			// Free temporary string
			free(iStr3);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Address:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"-1\" max=\"");
		char *gpioMaxStr20 = intToString(GPIO_NUM_MAX);
		html->print(gpioMaxStr20);
		free(gpioMaxStr20);
		html->print("\" name=\"address\" type=\"number\" value=\"");
		char *modbusAddrStr = intToString(config.modbus_address);
		html->print(modbusAddrStr);
		free(modbusAddrStr);
		html->print("\" /></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>DE:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input min=\"-1\" max=\"");
		char *gpioMaxStr21 = intToString(GPIO_NUM_MAX);
		html->print(gpioMaxStr21);
		free(gpioMaxStr21);
		html->print("\" name=\"de\" type=\"number\" value=\"");
		char *modbusDeStr = intToString(config.modbus_de_gpio);
		html->print(modbusDeStr);
		free(modbusDeStr);
		html->print("\" /></td>\n");
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<input class=\"button\" id=\"submitMODBUS\" name=\"commitMODBUS\" type=\"submit\" value=\"Apply\" maxlength=\"80\"/>\n");
		html->print("<input type=\"hidden\" name=\"commitMODBUS\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form>\n");
		html->print("</td></tr></table>\n");

		html->addHeader("MOD2", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

void handle_system(AsyncWebServerRequest *request)
{
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

		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 48000);
		if (!html)
		{
			return; // Memory allocation failed
		}

		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formHostName\") document.getElementById(\"updateHostName\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formTime\") document.getElementById(\"updateTime\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formNTP\") document.getElementById(\"updateTimeNtp\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formTimeZone\") document.getElementById(\"updateTimeZone\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formReboot\") document.getElementById(\"REBOOT\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formDisp\") document.getElementById(\"submitDISP\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formWebAuth\") document.getElementById(\"submitWebAuth\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formPWR\") document.getElementById(\"submitPWR\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formPath\") document.getElementById(\"submitPath\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/system',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		// html += "<h2>System Setting</h2>\n";
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>System Setting</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\">Host Name:</td>\n");

		// Building form with snprintf to avoid string concatenation
		char temp_buffer[300];
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formHostName\" method=\"post\"><input name=\"SetHostName\" type=\"text\" value=\"%s\" />\n", config.host_name);
		html->print(temp_buffer);
		html->print("<button type='submit' id='updateHostName'  name=\"updateHostName\"> Apply </button>\n");
		html->print("<input type=\"hidden\" name=\"updateHostName\"/></form>\n</td>\n");
		html->print("</tr>\n");
		html->print("<tr>");
		// html += "<form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formTime\" method=\"post\">\n";
		html->print("<td style=\"text-align: right;\">LOCAL DATE/TIME </td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formTime\" method=\"post\">\n<input name=\"SetTime\" type=\"text\" value=\"%s\" />\n", strTime);
		html->print(temp_buffer);
		html->print("<span class=\"input-group-addon\">\n<span class=\"glyphicon glyphicon-calendar\">\n</span></span>\n");
		// html += "<div class=\"col-sm-3 col-xs-6\"><button class=\"btn btn-primary\" data-args=\"[true]\" data-method=\"getDate\" type=\"button\" data-related-target=\"#SetTime\" />Get Date</button></div>\n");
		html->print("<button type='submit' id='updateTime'  name=\"commit\"> Time Update </button>\n");
		html->print("<input type=\"hidden\" name=\"updateTime\"/></form>\n</td>\n");
		// html += "<input class=\"btn btn-primary\" id=\"updateTime\" name=\"updateTime\" type=\"submit\" value=\"Time Update\" maxlength=\"80\"/></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\">NTP Host </td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formNTP\" method=\"post\"><input name=\"SetTimeNtp\" type=\"text\" value=\"%s\" />\n", config.ntp_host);
		html->print(temp_buffer);
		html->print("<button type='submit' id='updateTimeNtp'  name=\"commit\"> NTP Update </button>\n");
		html->print("<input type=\"hidden\" name=\"updateTimeNtp\"/></form>\n</td>\n");
		// html += "<input class=\"btn btn-primary\" id=\"updateTimeNtp\" name=\"updateTimeNtp\" type=\"submit\" value=\"NTP Update\" maxlength=\"80\"/></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\">Auto REBOOT:</td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formAutoReset\" method=\"post\"><input  min=\"0\" max=\"65535\"  name=\"SetAutoReset\" type=\"number\" value=\"%d\" /> Minutes\n", config.reset_timeout);
		html->print(temp_buffer);
		html->print("<button type='submit' id='updateAutoReset'  name=\"commit\"> Update </button> *<i>0=No reset</i>\n");
		html->print("<input type=\"hidden\" name=\"updateAutoReset\"/></form>\n</td>\n");
		// html += "<input class=\"button\" id=\"updateTimeNtp\" name=\"updateTimeNtp\" type=\"submit\" value=\"NTP Update\" maxlength=\"80\"/></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\">Time Zone </td>\n");
		html->print("<td style=\"text-align: left;\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formTimeZone\" method=\"post\">\n");
		html->print("<select name=\"SetTimeZone\" id=\"SetTimeZone\">\n");
		for (int i = 0; i < 40; i++)
		{
			if (config.timeZone == tzList[i].tz)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%.1f\" selected>%s Sec</option>\n", tzList[i].tz, tzList[i].name);
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%.1f\" >%s Sec</option>\n", tzList[i].tz, tzList[i].name);
			}
			html->print(temp_buffer);
		}
		html->print("</select>");
		html->print("<button type='submit' id='updateTimeZone'  name=\"commit\"> TZ Update </button>\n");
		html->print("<input type=\"hidden\" name=\"updateTimeZone\"/></form>\n</td>\n");
		// html += "<input class=\"btn btn-primary\" id=\"updateTimeZone\" name=\"updateTimeZone\" type=\"submit\" value=\"TZ Update\" maxlength=\"80\"/></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\">SYSTEM CONTROL </td>\n");
		html->print("<td style=\"text-align: left;\"><table><tr><td width=\"100\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formReboot\" method=\"post\"> <button type='submit' id='REBOOT'  name=\"commit\" style=\"background-color:red;color:white\"> REBOOT </button>\n");
		html->print(" <input type=\"hidden\" name=\"REBOOT\"/></form></td><td width=\"100\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formFactory\" method=\"post\"> <button type='submit' id='Factory'  name=\"commit\" style=\"background-color:orange;color:white\"> Factory Reset </button>\n");
		html->print(" <input type=\"hidden\" name=\"Factory\"/></form></td><td width=\"100\"><form accept-charset=\"UTF-8\" action=\"#\" enctype='multipart/form-data' id=\"formLoad\" method=\"post\"> <button type='submit' id='LoadCFG'  name=\"commit\" style=\"background-color:green;color:white\"> Load Default </button>\n");
		html->print(" <input type=\"hidden\" name=\"LoadCFG\"/></form></td></tr></table></td>\n");
		// html += "<td style=\"text-align: left;\"><input type='submit' class=\"btn btn-danger\" id=\"REBOOT\" name=\"REBOOT\" value='REBOOT'></td>\n");
		html->print("</tr></table><br /><br />\n");

		/************************ WEB AUTH **************************/
		html->print("<form id='formWebAuth' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Web Authentication</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Web USER:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"32\" maxlength=\"32\" class=\"form-control\" name=\"webauth_user\" type=\"text\" value=\"%s\" /></td>\n", config.http_username);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Web PASSWORD:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input size=\"63\" maxlength=\"63\" class=\"form-control\" name=\"webauth_pass\" type=\"password\" value=\"%s\" /></td>\n", config.http_password);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitWebAuth'  name=\"commit\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitWebAuth\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br /><br />");

		/**************Power Mode******************/
		// LU6JMF cleanup (Set/2026): "Power Save Mode" card stripped down to
		// just StandBy Delay (OLED timeout), the only field that was ever
		// really implemented. Enable/PWR GPIO/Sleep Interval/Power Mode/
		// Events were a dead UI stub - removed to save space.
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formPWR\" method=\"post\">\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Display StandBy</b></span></th>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>StandBy Delay:</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><input min=\"0\" max=\"9999\" name=\"stb\" type=\"number\" value=\"%d\" /></td>\n", config.pwr_stanby_delay);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitPWR'  name=\"commitPWR\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitPWR\"/>\n");
		html->print("</td></tr></table>\n");

		html->print("</form><br /><br />\n");

		#ifdef LOG_FILE
		/**************Log File******************/
		// LU6JMF fix (Set/2026): filterFlageEn used to be declared by the
		// (now removed) Power Save Mode "Events" section above. Re-declared
		// here since this LOG_FILE block still needs it.
		char filterFlageEn[10] = "";
		html->print("<form accept-charset=\"UTF-8\" action=\"#\" class=\"form-horizontal\" id=\"formLOG\" method=\"post\">\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Log File</b></span></th>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Activate:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<fieldset id=\"FilterGrp\">\n");
		html->print("<legend>Events</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">\n");
		html->print("<tr style=\"background:unset;\">");

		if (config.log & LOG_TRACKER)
			strcpy(filterFlageEn, "checked");
		else
			strcpy(filterFlageEn, "");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"logTracker\" type=\"checkbox\" value=\"OK\" %s/>Tracker</td>\n", filterFlageEn);
		html->print(temp_buffer);

		if (config.log & LOG_IGATE)
			strcpy(filterFlageEn, "checked");
		else
			strcpy(filterFlageEn, "");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"logIgate\" type=\"checkbox\" value=\"OK\" %s/>IGate</td>\n", filterFlageEn);
		html->print(temp_buffer);

		if (config.log & LOG_DIGI)
			strcpy(filterFlageEn, "checked");
		else
			strcpy(filterFlageEn, "");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"logDigi\" type=\"checkbox\" value=\"OK\" %s/>DIGI</td>\n", filterFlageEn);
		html->print(temp_buffer);

		if (config.log & LOG_WX)
			strcpy(filterFlageEn, "checked");
		else
			strcpy(filterFlageEn, "");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"logWeather\" type=\"checkbox\" value=\"OK\" %s/>Weather</td>\n", filterFlageEn);
		html->print(temp_buffer);

		html->print("<td style=\"border:unset;\"></td>\n");
		html->print("</tr></table></fieldset>\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitLOG'  name=\"commitLOG\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitLOG\"/>\n");
		html->print("</td></tr></table>\n");

		html->print("</form><br /><br />\n");
		#endif
		/************************ PATH USER define **************************/
		html->print("<form id='formPath' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>PATH USER Define</b></span></th>\n");
		html->print("<tr>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"right\"><b>PATH_1:</b></td>\n<td style=\"text-align: left;\"><input size=\"72\" maxlength=\"72\" class=\"form-control\" name=\"path1\" type=\"text\" value=\"%s\" /></td>\n", config.path[0]);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"right\"><b>PATH_2:</b></td>\n<td style=\"text-align: left;\"><input size=\"72\" maxlength=\"72\" class=\"form-control\" name=\"path2\" type=\"text\" value=\"%s\" /></td>\n", config.path[1]);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"right\"><b>PATH_3:</b></td>\n<td style=\"text-align: left;\"><input size=\"72\" maxlength=\"72\" class=\"form-control\" name=\"path3\" type=\"text\" value=\"%s\" /></td>\n", config.path[2]);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td align=\"right\"><b>PATH_4:</b></td>\n<td style=\"text-align: left;\"><input size=\"72\" maxlength=\"72\" class=\"form-control\" name=\"path4\" type=\"text\" value=\"%s\" /></td>\n", config.path[3]);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitPath'  name=\"commitPath\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitPath\"/>\n");
		html->print("</td></tr></table>\n");
		html->print("</form><br /><br />");

#if defined OLED || defined ST7735_160x80 || defined GUI_LCD
		html->print("<form id='formDisp' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		// html += "<h2>Display Setting</h2>\n";
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Display Setting</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>OLED/TFT Enable</b></td>\n");
		char oledFlageEn[10] = "";
		if (config.oled_enable == true)
			strcpy(oledFlageEn, "checked");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"oledEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", oledFlageEn);
		html->print(temp_buffer);
		html->print("</tr>\n");
		if (config.disp_flip == true)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>Flip Rotate</b></td>\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"dispFlip\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", oledFlageEn);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>TX Display</b></td>\n");
		if (config.tx_display == true)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"txdispEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"> <i>*All TX Packet for display affter filter.</i></label></td>\n", oledFlageEn);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>RX Display</b></td>\n");
		if (config.rx_display == true)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"rxdispEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"> <i>*All RX Packet for display affter filter.</i></label></td>\n", oledFlageEn);
		html->print(temp_buffer);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>Head Up</b></td>\n");
		if (config.h_up == true)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"hupEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"> <i>*The compass will rotate in the direction of movement.</i></label></td>\n", oledFlageEn);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>TFT Brightness</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"dispBright\" id=\"dispBright\">\n");
		for (int i = 0; i < 255; i += 25)
		{
			if (config.disp_brightness == i)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" selected>%d</option>\n", i, i);
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" >%d</option>\n", i, i);
			}
			html->print(temp_buffer);
		}
		html->print("</select>\n");
		html->print("</td></tr>\n");

		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>Popup Delay</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"dispDelay\" id=\"dispDelay\">\n");
		for (int i = 0; i < 16; i += 1)
		{
			if (config.dispDelay == i)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" selected>%d Sec</option>\n", i, i);
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" >%d Sec</option>\n", i, i);
			}
			html->print(temp_buffer);
		}
		html->print("</select>\n");
		html->print("</td></tr>\n");
		html->print("<tr>\n");
		html->print("<td style=\"text-align: right;\"><b>OLED/TFT Sleep</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"oled_timeout\" id=\"oled_timeout\">\n");
		for (int i = 0; i <= 600; i += 30)
		{
			if (config.oled_timeout == i)
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" selected>%d Sec</option>\n", i, i);
			}
			else
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<option value=\"%d\" >%d Sec</option>\n", i, i);
			}
			html->print(temp_buffer);
		}
		html->print("</select>\n");
		html->print("</td></tr>\n");
		char rfFlageEn[20] = "";
		if (config.dispRF == true)
			strcpy(rfFlageEn, "checked");
		char inetFlageEn[20] = "";
		if (config.dispINET == true)
			strcpy(inetFlageEn, "checked");
		snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td style=\"text-align: right;\"><b>RX Channel</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"dispRF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"dispINET\" value=\"OK\" %s/>Internet </td></tr>\n", rfFlageEn, inetFlageEn);
		html->print(temp_buffer);
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Filter DX:</b></td>\n");
		html->print("<td style=\"text-align: left;\"><input type=\"number\" name=\"filterDX\" min=\"0\" max=\"9999\"\n");

		snprintf(temp_buffer, sizeof(temp_buffer), "step=\"1\" value=\"%d\" /> Km.  <label style=\"vertical-align: bottom;font-size: 8pt;\"> <i>*Value 0 is all distant allow.</i></label></td>\n", config.filterDistant);
		html->print(temp_buffer);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Filter:</b></td>\n");

		html->print("<td align=\"center\">\n");
		html->print("<fieldset id=\"filterDispGrp\">\n");
		html->print("<legend>Filter popup display</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">\n");
		html->print("<tr style=\"background:unset;\">");

		// html += "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"dispTNC\" name=\"dispTNC\" type=\"checkbox\" value=\"OK\" " + rfFlageEn + "/>From RF</td>\n";

		// html += "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"dispINET\" name=\"dispINET\" type=\"checkbox\" value=\"OK\" " + inetFlageEn + "/>From INET</td>\n";

		if (config.dispFilter & FILTER_MESSAGE)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterMessage\" name=\"filterMessage\" type=\"checkbox\" value=\"OK\" %s/>Message</td>\n", oledFlageEn);
		html->print(temp_buffer);
		if (config.dispFilter & FILTER_STATUS)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterStatus\" name=\"filterStatus\" type=\"checkbox\" value=\"OK\" %s/>Status</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_TELEMETRY)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterTelemetry\" name=\"filterTelemetry\" type=\"checkbox\" value=\"OK\" %s/>Telemetry</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_WX)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterWeather\" name=\"filterWeather\" type=\"checkbox\" value=\"OK\" %s/>Weather</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_OBJECT)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterObject\" name=\"filterObject\" type=\"checkbox\" value=\"OK\" %s/>Object</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_ITEM)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		html->print("</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterItem\" name=\"filterItem\" type=\"checkbox\" value=\"OK\" ");
		html->print(oledFlageEn);
		html->print("/>Item</td>\n");

		if (config.dispFilter & FILTER_QUERY)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterQuery\" name=\"filterQuery\" type=\"checkbox\" value=\"OK\" %s/>Query</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_BUOY)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterBuoy\" name=\"filterBuoy\" type=\"checkbox\" value=\"OK\" %s/>Buoy</td>\n", oledFlageEn);
		html->print(temp_buffer);

		if (config.dispFilter & FILTER_POSITION)
			strcpy(oledFlageEn, "checked");
		else
			strcpy(oledFlageEn, "");
		snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"border:unset;\"><input class=\"field_checkbox\" id=\"filterPosition\" name=\"filterPosition\" type=\"checkbox\" value=\"OK\" %s/>Position</td>\n", oledFlageEn);
		html->print(temp_buffer);

		html->print("<td style=\"border:unset;\"></td>\n");
		html->print("</tr></table></fieldset>\n");

		html->print("</td></tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitDISP'  name=\"commitDISP\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitDISP\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");
#endif

		html->addHeader("System", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
		// Streamed response: avoids a single large calloc() for the whole page.
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		// instead of requiring one big contiguous heap block up front - this was
		// the reproducible trigger for a heap corruption assert (block_trim_free)
		// under real traffic, confirmed via heap-integrity diagnostics.
		char tempHtml[512];
		AsyncResponseStream *html = request->beginResponseStream("text/html", 44000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		// html->print("document.getElementById(\"submitIGATE\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formIgate\") document.getElementById(\"submitIGATE\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formIgateFilter\") document.getElementById(\"submitIGATEfilter\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/igate',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n<script type=\"text/javascript\">\n");

		html->print("function openWindowSymbol() {\n");
		html->print("var i, l, options = [{\n");
		html->print("value: 'first',\n");
		html->print("text: 'First'\n");
		html->print("}, {\n");
		html->print("value: 'second',\n");
		html->print("text: 'Second'\n");
		html->print("}],\n");
		html->print("newWindow = window.open(\"/symbol\", null, \"height=400,width=400,status=no,toolbar=no,menubar=no,location=no\");\n");
		html->print("}\n");

		html->print("function setValue(symbol,table) {\n");
		html->print("document.getElementById('igateSymbol').value = String.fromCharCode(symbol);\n");
		html->print("if(table==1){\n document.getElementById('igateTable').value='/';\n");
		html->print("}else if(table==2){\n document.getElementById('igateTable').value='\\\\';\n}\n");
		html->print("document.getElementById('igateImgSymbol').src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n");
		html->print("\n}\n");
		html->print("function calculatePHGR(){document.forms.formIgate.texttouse.value=\"PHG\"+calcPower(document.forms.formIgate.power.value)+calcHeight(document.forms.formIgate.haat.value)+calcGain(document.forms.formIgate.gain.value)+calcDirection(document.forms.formIgate.direction.selectedIndex)}function Log2(e){return Math.log(e)/Math.log(2)}function calcPerHour(e){return e<10?e:String.fromCharCode(65+(e-10))}function calcHeight(e){return String.fromCharCode(48+Math.round(Log2(e/10),0))}function calcPower(e){if(e<1)return 0;if(e>=1&&e<4)return 1;if(e>=4&&e<9)return 2;if(e>=9&&e<16)return 3;if(e>=16&&e<25)return 4;if(e>=25&&e<36)return 5;if(e>=36&&e<49)return 6;if(e>=49&&e<64)return 7;if(e>=64&&e<81)return 8;if(e>=81)return 9}function calcDirection(e){if(e==\"0\")return\"0\";if(e==\"1\")return\"1\";if(e==\"2\")return\"2\";if(e==\"3\")return\"3\";if(e==\"4\")return\"4\";if(e==\"5\")return\"5\";if(e==\"6\")return\"6\";if(e==\"7\")return\"7\";if(e==\"8\")return\"8\"}function calcGain(e){return e>9?\"9\":e<0?\"0\":Math.round(e,0)}\n");
		html->print("function onRF2INETCheck() {\n");
		html->print("if (document.querySelector('#rf2inetEnable').checked) {\n");
		// Checkbox has been checked
		html->print("document.getElementById(\"rf2inetFilterGrp\").disabled=false;\n");
		html->print("} else {\n");
		// Checkbox has been unchecked
		html->print("document.getElementById(\"rf2inetFilterGrp\").disabled=true;\n");
		html->print("}\n}\n");
		html->print("function onINET2RFCheck() {\n");
		html->print("if (document.querySelector('#inet2rfEnable').checked) {\n");
		// Checkbox has been checked
		html->print("document.getElementById(\"inet2rfFilterGrp\").disabled=false;\n");
		html->print("} else {\n");
		// Checkbox has been unchecked
		html->print("document.getElementById(\"inet2rfFilterGrp\").disabled=true;\n");
		html->print("}\n}\n");

		html->print("function selPrecision(idx) {\n");
		html->print("var x=0;\n");
		html->print("x = document.getElementsByName(\"precision\"+idx)[0].value;\n");
		html->print("document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n");
		html->print("}\n");
		html->print("function selOffset(idx) {\n");
		html->print("var x=0;\n");
		html->print("x = document.getElementsByName(\"offset\"+idx)[0].value;\n");
		html->print("document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n");
		html->print("}\n");
		html->print("</script>\n");
		delay(1);
		/************************ IGATE Mode **************************/
		html->print("<form id='formIgate' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		// html->print("<h2>[IGATE] Internet Gateway Mode</h2>\n");
		html->print("<table>\n");
		// html->print("<tr>\n");
		// html->print("<th width=\"200\"><span><b>Setting</b></span></th>\n");
		// html->print("<th><span><b>Value</b></span></th>\n");
		// html->print("</tr>\n");
		html->print("<th colspan=\"2\"><span><b>[IGATE] Internet Gateway Mode</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");
		char igateEnFlag[10] = "";
		if (config.igate_en)
			strcpy(igateEnFlag, "checked");
		else
			strcpy(igateEnFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", igateEnFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station Callsign:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"%s\" /></td>\n", config.aprs_mycall);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station SSID:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"mySSID\" id=\"mySSID\">\n");
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			if (config.aprs_ssid == ssid)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%d</option>\n", ssid, ssid);
				}
				else
				{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%d</option>\n", ssid, ssid);
				}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station Symbol:</b></td>\n");
		const char *table = "1";
		if (config.igate_symbol[0] == 47)
			table = "1";
		if (config.igate_symbol[0] == 92)
			table = "2";
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"igateTable\" name=\"igateTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"igateSymbol\" name=\"igateSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"igateImgSymbol\" onclick=\"openWindowSymbol();\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%s.png\"> <i>*Click icon for select symbol</i></td>\n",
						 config.igate_symbol[0], config.igate_symbol[1], (int)config.igate_symbol[1], table);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Item/Obj Name:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" id=\"igateObject\" name=\"igateObject\" type=\"text\" value=\"%s\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n", config.igate_object);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PATH:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"igatePath\" id=\"igatePath\">\n");
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
			{
				if (config.igate_path == pthIdx)
				{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
				}
				else
				{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
				}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		// html->print("<td style=\"text-align: left;\"><input maxlength=\"72\" size=\"72\" id=\"igatePath\" name=\"igatePath\" type=\"text\" value=\"" + String(config.igate_path) + "\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Host:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"20\" size=\"20\" id=\"aprsHost\" name=\"aprsHost\" type=\"text\" value=\"%s\" /> *APRS-IS by T2THAI at <a href=\"http://aprs.nakhonthai.net:14501\" target=\"_t2thai\">aprs.nakhonthai.net:14580</a>,CBAPRS at <a href=\"http://aprs.nakhonthai.net:24501\" target=\"_t2thai\">aprs.nakhonthai.net:24580</a></td>\n", config.aprs_host);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Port:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input min=\"1\" max=\"65535\" step=\"1\" id=\"aprsPort\" name=\"aprsPort\" type=\"number\" value=\"%d\" /> *AMPR Host at <a href=\"http://aprs.hs5tqa.ampr.org:14501\" target=\"_t2thai\">aprs.hs5tqa.ampr.org:14580</a></td>\n", config.aprs_port);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Server Filter:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"30\" size=\"30\" id=\"aprsFilter\" name=\"aprsFilter\" type=\"text\" value=\"%s\" /> *Filter: <a target=\"_blank\" href=\"http://www.aprs-is.net/javAPRSFilter.aspx\">http://www.aprs-is.net/javAPRSFilter.aspx</a></td>\n", config.aprs_filter);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Text Comment:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"igateComment\" name=\"igateComment\" type=\"text\" value=\"%s\" /></td>\n", config.igate_comment);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Text Status:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"igateStatus\" name=\"igateStatus\" type=\"text\" value=\"%s\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"igateSTSInv\" type=\"number\" value=\"%d\" />Sec.</td>\n", config.igate_status, config.igate_sts_interval);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");

		char rf2inetFlag[10];
		if (config.rf2inet)
			strcpy(rf2inetFlag, "checked");
		else
			strcpy(rf2inetFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td align=\"right\"><b>RF2INET:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"rf2inetEnable\" name=\"rf2inetEnable\" onclick=\"onRF2INETCheck()\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch RF to Internet gateway</i></label></td>\n", rf2inetFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		char inet2rfEnFlag[10];
		if (config.inet2rf)
			strcpy(inet2rfEnFlag, "checked");
		else
			strcpy(inet2rfEnFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td align=\"right\"><b>INET2RF:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"inet2rfEnable\" name=\"inet2rfEnable\" onclick=\"onINET2RFCheck()\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch Internet to RF gateway</i></label></td>\n", inet2rfEnFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
			const char *timeStampFlag = config.igate_timestamp ? "checked" : "";
		snprintf(tempHtml, sizeof(tempHtml), "<td align=\"right\"><b>Time Stamp:</b></td>\n<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateTimeStamp\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", timeStampFlag);
		html->print(tempHtml);
		html->print("</tr>\n<tr>");

		html->print("<td align=\"right\"><b>POSITION:</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");
			const char *igateBcnEnFlag = config.igate_bcn ? "checked" : "";
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Beacon:</td><td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"igateBcnEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\">  Interval:<input min=\"0\" max=\"3600\" step=\"1\" id=\"igatePosInv\" name=\"igatePosInv\" type=\"number\" value=\"%d\" />Sec.</label></td></tr>", igateBcnEnFlag, config.igate_interval);
		html->print(tempHtml);
		const char *igatePosFixFlag = config.igate_gps ? "" : "checked=\"checked\"";
		const char *igatePosGPSFlag = config.igate_gps ? "checked=\"checked\"" : "";
		const char *igatePos2RFFlag = config.igate_loc2rf ? "checked" : "";
		const char *igatePos2INETFlag = config.igate_loc2inet ? "checked" : "";

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"igatePosSel\" value=\"0\" %s/>Fix <input type=\"radio\" name=\"igatePosSel\" value=\"1\" %s/>GPS </td></tr>\n", igatePosFixFlag, igatePosGPSFlag);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"igatePos2RF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"igatePos2INET\" value=\"OK\" %s/>Internet </td></tr>\n", igatePos2RFFlag, igatePos2INETFlag);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"igatePosLat\" name=\"igatePosLat\" type=\"number\" value=\"%.5f\" />degrees (positive for North, negative for South)</td></tr>\n", config.igate_lat);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"igatePosLon\" name=\"igatePosLon\" type=\"number\" value=\"%.5f\" />degrees (positive for East, negative for West)</td></tr>\n", config.igate_lon);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"igatePosAlt\" name=\"igatePosAlt\" type=\"number\" value=\"%.2f\" /> meter. *Value 0 is not send height</td></tr>\n", config.igate_alt);
		html->print(tempHtml);

		html->print("</table></td>");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PHG:</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Radio TX Power</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"power\" id=\"power\">\n");
		html->print("<option value=\"1\" selected>1</option>\n");
		html->print("<option value=\"5\">5</option>\n");
		html->print("<option value=\"10\">10</option>\n");
		html->print("<option value=\"15\">15</option>\n");
		html->print("<option value=\"25\">25</option>\n");
		html->print("<option value=\"35\">35</option>\n");
		html->print("<option value=\"50\">50</option>\n");
		html->print("<option value=\"65\">65</option>\n");
		html->print("<option value=\"80\">80</option>\n");
		html->print("</select> Watts</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td style=\"text-align: right;\">Antenna Gain</td><td style=\"text-align: left;\"><input size=\"3\" min=\"0\" max=\"100\" step=\"0.1\" id=\"gain\" name=\"gain\" type=\"number\" value=\"6\" /> dBi</td></tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Height</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"haat\" id=\"haat\">\n");
		int k = 10;
		for (uint8_t w = 0; w < 10; w++)
		{
			char *temp_opt = allocateStringMemory(256);
			if (temp_opt)
			{
				if (w == 0)
				{
					snprintf(temp_opt, 256, "<option value=\"%d\" selected>%d</option>\n", k, k);
				}
				else
				{
					snprintf(temp_opt, 256, "<option value=\"%d\">%d</option>\n", k, k);
				}
				html->print(temp_opt);
				free(temp_opt);
			}
			k += k;
		}
		html->print("</select> Feet</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Antenna/Direction</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"direction\" id=\"direction\">\n");
		html->print("<option>Omni</option><option>NE</option><option>E</option><option>SE</option><option>S</option><option>SW</option><option>W</option><option>NW</option><option>N</option>\n");
		html->print("</select></td>\n");
		html->print("</tr>\n");

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>PHG Text</b></td><td align=\"left\"><input name=\"texttouse\" type=\"text\" size=\"6\" style=\"background-color: rgb(97, 239, 170);\" value=\"%s\"/> <input type=\"button\" value=\"Calculate PHG\" onclick=\"javascript:calculatePHGR()\" /></td></tr>\n", config.igate_phg);
		html->print(tempHtml);
		html->print("</table></td>");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n");
		html->print("<td align=\"center\"><table>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"igateTlmInv\" name=\"igateTlmInv\" type=\"number\" value=\"%d\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>", config.igate_tlm_interval);
		html->print(tempHtml);
		for (int ax = 0; ax < 5; ax++)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>CH A%d:</b></td>\n", ax + 1);
			html->print(tempHtml);

			html->print("<td align=\"center\">\n");
			html->print("<table>");

			html->print("<tr><td style=\"text-align: right;\">Sensor:</td>\n");
			html->print("<td style=\"text-align: left;\">CH: ");

			snprintf(tempHtml, sizeof(tempHtml), "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax, ax);
			html->print(tempHtml);

			for (uint8_t idx = 0; idx < 11; idx++)
				{
					if (idx == 0)
					{
						if (config.igate_tlm_sensor[ax] == idx)
						{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>NONE</option>\n", idx);
						}
						else
						{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">NONE</option>\n", idx);
						}
					}
					else
					{
						if (config.igate_tlm_sensor[ax] == idx)
						{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>SENSOR#%d</option>\n", idx, idx);
						}
						else
						{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">SENSOR#%d</option>\n", idx, idx);
						}
					}
				html->print(tempHtml);
			}
			html->print("</select></td>\n");

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.igate_tlm_PARM[ax]);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.igate_tlm_UNIT[ax]);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision%d\" type=\"text\" value=\"%d\" onchange=\"selPrecision(%d)\"/></td></tr>\n", ax, config.igate_tlm_precision[ax], ax);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%da\" type=\"number\" value=\"%.5f\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%db\" type=\"number\" value=\"%.5f\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%dc\" type=\"number\" value=\"%.5f\" /> (av<sup>2</sup>+bv+c) </td>\n",
							 ax, config.igate_tlm_EQNS[ax][0], ax, config.igate_tlm_EQNS[ax][1], ax, config.igate_tlm_EQNS[ax][2]);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset%d\" type=\"text\" value=\"%.5f\" onchange=\"selOffset(%d)\"/></td></tr>\n", ax, config.igate_tlm_offset[ax], ax);
			html->print(tempHtml);

			html->print("</table></td>");
			html->print("</tr>\n");
		}
		html->print("</table></td></tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitIGATE'  name=\"commitIGATE\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitIGATE\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br /><br />");

		html->print("<form id='formIgateFilter' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>[IGATE] Filter</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>RF2INET Filter:</b></td>\n");

		html->print("<td align=\"center\">\n");
		if (config.rf2inet)
			html->print("<fieldset id=\"rf2inetFilterGrp\">\n");
		else
			html->print("<fieldset id=\"rf2inetFilterGrp\" disabled>\n");
		html->print("<legend>Filter RF to Internet</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">");
		html->print("<tr style=\"background:unset;\">");

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterMessage\" type=\"checkbox\" value=\"OK\" %s/>Message</td>\n",
						 (config.rf2inetFilter & FILTER_MESSAGE) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterStatus\" type=\"checkbox\" value=\"OK\" %s/>Status</td>\n",
						 (config.rf2inetFilter & FILTER_STATUS) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterTelemetry\" type=\"checkbox\" value=\"OK\" %s/>Telemetry</td>\n",
						 (config.rf2inetFilter & FILTER_TELEMETRY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterWeather\" type=\"checkbox\" value=\"OK\" %s/>Weather</td>\n",
						 (config.rf2inetFilter & FILTER_WX) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterObject\" type=\"checkbox\" value=\"OK\" %s/>Object</td>\n",
						 (config.rf2inetFilter & FILTER_OBJECT) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterItem\" type=\"checkbox\" value=\"OK\" %s/>Item</td>\n",
						 (config.rf2inetFilter & FILTER_ITEM) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterQuery\" type=\"checkbox\" value=\"OK\" %s/>Query</td>\n",
						 (config.rf2inetFilter & FILTER_QUERY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterBuoy\" type=\"checkbox\" value=\"OK\" %s/>Buoy</td>\n",
						 (config.rf2inetFilter & FILTER_BUOY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterPosition\" type=\"checkbox\" value=\"OK\" %s/>Position</td>\n",
						 (config.rf2inetFilter & FILTER_POSITION) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"rf2inetFilterAll\" type=\"checkbox\" value=\"OK\" %s/>ALL</td>\n",
				 (config.rf2inetFilter & FILTER_ENABLE_ALL) ? "checked" : "");
		html->print(tempHtml);

		html->print("<td style=\"border:unset;\"></td>");
		html->print("</tr></table></fieldset>\n");
		html->print("</td></tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>INET2RF Filter:</b></td>\n");

		html->print("<td align=\"center\">\n");
		if (config.inet2rf)
			html->print("<fieldset id=\"inet2rfFilterGrp\">\n");
		else
			html->print("<fieldset id=\"inet2rfFilterGrp\" disabled>\n");
		html->print("<legend>Filter Internet to RF</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">");
		html->print("<tr style=\"background:unset;\">");

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterMessage\" type=\"checkbox\" value=\"OK\" %s/>Message</td>\n",
						 (config.inet2rfFilter & FILTER_MESSAGE) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterStatus\" type=\"checkbox\" value=\"OK\" %s/>Status</td>\n",
						 (config.inet2rfFilter & FILTER_STATUS) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterTelemetry\" type=\"checkbox\" value=\"OK\" %s/>Telemetry</td>\n",
						 (config.inet2rfFilter & FILTER_TELEMETRY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterWeather\" type=\"checkbox\" value=\"OK\" %s/>Weather</td>\n",
						 (config.inet2rfFilter & FILTER_WX) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterObject\" type=\"checkbox\" value=\"OK\" %s/>Object</td>\n",
						 (config.inet2rfFilter & FILTER_OBJECT) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterItem\" type=\"checkbox\" value=\"OK\" %s/>Item</td>\n",
						 (config.inet2rfFilter & FILTER_ITEM) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterQuery\" type=\"checkbox\" value=\"OK\" %s/>Query</td>\n",
						 (config.inet2rfFilter & FILTER_QUERY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterBuoy\" type=\"checkbox\" value=\"OK\" %s/>Buoy</td>\n",
						 (config.inet2rfFilter & FILTER_BUOY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"inet2rfFilterPosition\" type=\"checkbox\" value=\"OK\" %s/>Position</td>\n",
						 (config.inet2rfFilter & FILTER_POSITION) ? "checked" : "");
		html->print(tempHtml);

		html->print("<td style=\"border:unset;\"></td>");
		html->print("</tr></table></fieldset>\n");
		html->print("</td></tr>\n");

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitIGATEfilter'  name=\"commitIGATEfilter\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitIGATEfilter\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");

		html->addHeader("IGATE", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 30000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		char tempHtml[512];
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("document.getElementById(\"submitDIGI\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/digi',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n<script type=\"text/javascript\">\n");
		html->print("function openWindowSymbol() {\n");
		html->print("var i, l, options = [{\n");
		html->print("value: 'first',\n");
		html->print("text: 'First'\n");
		html->print("}, {\n");
		html->print("value: 'second',\n");
		html->print("text: 'Second'\n");
		html->print("}],\n");
		html->print("newWindow = window.open(\"/symbol\", null, \"height=400,width=400,status=no,toolbar=no,menubar=no,titlebar=no,location=no\");\n");
		html->print("}\n");

		html->print("function setValue(symbol,table) {\n");
		html->print("document.getElementById('digiSymbol').value = String.fromCharCode(symbol);\n");
		html->print("if(table==1){\n document.getElementById('digiTable').value='/';\n");
		html->print("}else if(table==2){\n document.getElementById('digiTable').value='\\\\';\n}\n");
		html->print("document.getElementById('digiImgSymbol').src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n");
		html->print("\n}\n");
		html->print("function calculatePHGR(){document.forms.formDIGI.texttouse.value=\"PHG\"+calcPower(document.forms.formDIGI.power.value)+calcHeight(document.forms.formDIGI.haat.value)+calcGain(document.forms.formDIGI.gain.value)+calcDirection(document.forms.formDIGI.direction.selectedIndex)}function Log2(e){return Math.log(e)/Math.log(2)}function calcPerHour(e){return e<10?e:String.fromCharCode(65+(e-10))}function calcHeight(e){return String.fromCharCode(48+Math.round(Log2(e/10),0))}function calcPower(e){if(e<1)return 0;if(e>=1&&e<4)return 1;if(e>=4&&e<9)return 2;if(e>=9&&e<16)return 3;if(e>=16&&e<25)return 4;if(e>=25&&e<36)return 5;if(e>=36&&e<49)return 6;if(e>=49&&e<64)return 7;if(e>=64&&e<81)return 8;if(e>=81)return 9}function calcDirection(e){if(e==\"0\")return\"0\";if(e==\"1\")return\"1\";if(e==\"2\")return\"2\";if(e==\"3\")return\"3\";if(e==\"4\")return\"4\";if(e==\"5\")return\"5\";if(e==\"6\")return\"6\";if(e==\"7\")return\"7\";if(e==\"8\")return\"8\"}function calcGain(e){return e>9?\"9\":e<0?\"0\":Math.round(e,0)}\n");
		html->print("function selPrecision(idx) {\n");
		html->print("var x=0;\n");
		html->print("x = document.getElementsByName(\"precision\"+idx)[0].value;\n");
		html->print("document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n");
		html->print("}\n");
		html->print("function selOffset(idx) {\n");
		html->print("var x=0;\n");
		html->print("x = document.getElementsByName(\"offset\"+idx)[0].value;\n");
		html->print("document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n");
		html->print("}\n");
		html->print("</script>\n");

		/************************ DIGI Mode **************************/
		html->print("<form id='formDIGI' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		// html->print("<h2>[DIGI] Digital Repeater Mode</h2>\n");
		html->print("<table>\n");
		// html->print("<tr>\n");
		// html->print("<th width=\"200\"><span><b>Setting</b></span></th>\n");
		// html->print("<th><span><b>Value</b></span></th>\n");
		// html->print("</tr>\n");
		html->print("<th colspan=\"2\"><span><b>[DIGI] Digital Repeater Mode</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");
		char digiFlag[10] = "";
		if (config.digi_en)
			strcpy(digiFlag, "checked");
		else
			strcpy(digiFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", digiFlag);
		html->print(tempHtml);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Auto Enable:</b></td>\n");

		if (config.digi_auto)
			strcpy(digiFlag, "checked");
		else
			strcpy(digiFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiAuto\" value=\"OK\" %s><span class=\"slider round\"></span></label> <i>*Automatic enable when APRS-IS disconnected</i></td>\n", digiFlag);
		html->print(tempHtml);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station Callsign:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"%s\" /></td>\n", config.digi_mycall);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station SSID:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"mySSID\" id=\"mySSID\">\n");
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			if (config.digi_ssid == ssid)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%d</option>\n", ssid, ssid);
			}
			else
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%d</option>\n", ssid, ssid);
			}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station Symbol:</b></td>\n");
		char table[3] = "1";
		if (config.digi_symbol[0] == 47)
			strcpy(table, "1");
		if (config.digi_symbol[0] == 92)
			strcpy(table, "2");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"digiTable\" name=\"digiTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"digiSymbol\" name=\"digiSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"digiImgSymbol\" onclick=\"openWindowSymbol();\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%s.png\"> <i>*Click icon for select symbol</i></td>\n",
				 config.digi_symbol[0], config.digi_symbol[1], (int)config.digi_symbol[1], table);
		html->print(tempHtml);

		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PATH:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"digiPath\" id=\"digiPath\">\n");
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			if (config.digi_path == pthIdx)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
			}
			else
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
			}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		// html->print("<td style=\"text-align: left;\"><input maxlength=\"72\" size=\"72\" id=\"digiPath\" name=\"digiPath\" type=\"text\" value=\"" + String(config.digi_path) + "\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Text Comment:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"digiComment\" name=\"digiComment\" type=\"text\" value=\"%s\" /></td>\n", config.digi_comment);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Text Status:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"digiStatus\" name=\"digiStatus\" type=\"text\" value=\"%s\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"digiSTSInv\" type=\"number\" value=\"%d\" />Sec.</td>\n", config.digi_status, config.digi_sts_interval);
		html->print(tempHtml);

		html->print("</tr>\n");

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\"><b>Repeat Delay:</b></td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"100\" id=\"digiDelay\" name=\"digiDelay\" type=\"number\" value=\"%d\" /> mSec. <i>*0 is auto,Other random of delay time</i></td></tr>", config.digi_delay);
		html->print(tempHtml);

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Time Stamp:</b></td>\n");
		char timeStampFlag[10];
		if (config.digi_timestamp)
			strcpy(timeStampFlag, "checked");
		else
			strcpy(timeStampFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiTimeStamp\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", timeStampFlag);
		html->print(tempHtml);

		html->print("</tr>\n");

		html->print("<tr><td align=\"right\"><b>POSITION:</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");

		if (config.digi_bcn)
			strcpy(digiFlag, "checked");
		else
			strcpy(digiFlag, "");

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Beacon:</td><td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"digiBcnEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\">  Interval:<input min=\"0\" max=\"3600\" step=\"1\" id=\"digiPosInv\" name=\"digiPosInv\" type=\"number\" value=\"%d\" />Sec.</label></td></tr>", digiFlag, config.digi_interval);
		html->print(tempHtml);

		String digiPosFixFlag = "";
		String digiPosGPSFlag = "";
		String digiPos2RFFlag = "";
		String digiPos2INETFlag = "";
		if (config.digi_gps)
			digiPosGPSFlag = "checked=\"checked\"";
		else
			digiPosFixFlag = "checked=\"checked\"";

		if (config.digi_loc2rf)
			digiPos2RFFlag = "checked";
		if (config.digi_loc2inet)
			digiPos2INETFlag = "checked";
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"digiPosSel\" value=\"0\" %s/>Fix <input type=\"radio\" name=\"digiPosSel\" value=\"1\" %s/>GPS </td></tr>\n",
				 digiPosFixFlag.c_str(), digiPosGPSFlag.c_str());
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"digiPos2RF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"digiPos2INET\" value=\"OK\" %s/>Internet </td></tr>\n",
				 digiPos2RFFlag.c_str(), digiPos2INETFlag.c_str());
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"digiPosLat\" name=\"digiPosLat\" type=\"number\" value=\"%.5f\" />degrees (positive for North, negative for South)</td></tr>\n", config.digi_lat);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"digiPosLon\" name=\"digiPosLon\" type=\"number\" value=\"%.5f\" />degrees (positive for East, negative for West)</td></tr>\n", config.digi_lon);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"digiPosAlt\" name=\"digiPosAlt\" type=\"number\" value=\"%.2f\" /> meter. *Value 0 is not send height</td></tr>\n", config.digi_alt);
		html->print(tempHtml);
		html->print("</table></td>");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PHG:</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Radio TX Power</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"power\" id=\"power\">\n");
		html->print("<option value=\"1\" selected>1</option>\n");
		html->print("<option value=\"5\">5</option>\n");
		html->print("<option value=\"10\">10</option>\n");
		html->print("<option value=\"15\">15</option>\n");
		html->print("<option value=\"25\">25</option>\n");
		html->print("<option value=\"35\">35</option>\n");
		html->print("<option value=\"50\">50</option>\n");
		html->print("<option value=\"65\">65</option>\n");
		html->print("<option value=\"80\">80</option>\n");
		html->print("</select> Watts</td>\n");
		html->print("</tr>\n");
		html->print("<tr><td style=\"text-align: right;\">Antenna Gain</td><td style=\"text-align: left;\"><input size=\"3\" min=\"0\" max=\"100\" step=\"0.1\" id=\"gain\" name=\"gain\" type=\"number\" value=\"6\" /> dBi</td></tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Height</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"haat\" id=\"haat\">\n");
		int k = 10;
		for (uint8_t w = 0; w < 10; w++)
		{
			if (w == 0)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%d</option>\n", k, k);
			}
			else
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%d</option>\n", k, k);
			}
			html->print(tempHtml);
			k += k;
		}
		html->print("</select> Feet</td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\">Antenna/Direction</td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"direction\" id=\"direction\">\n");
		html->print("<option>Omni</option><option>NE</option><option>E</option><option>SE</option><option>S</option><option>SW</option><option>W</option><option>NW</option><option>N</option>\n");
		html->print("</select></td>\n");
		html->print("</tr>\n");

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>PHG Text</b></td><td align=\"left\"><input name=\"texttouse\" type=\"text\" size=\"6\" style=\"background-color: rgb(97, 239, 170);\" value=\"%s\"/> <input type=\"button\" value=\"Calculate PHG\" onclick=\"javascript:calculatePHGR()\" /></td></tr>\n", config.digi_phg);
		html->print(tempHtml);

		html->print("</table></tr>");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Filter:</b></td>\n");

		html->print("<td align=\"center\">\n");
		html->print("<fieldset id=\"FilterGrp\">\n");
		html->print("<legend>Filter repeater</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">");
		html->print("<tr style=\"background:unset;\">");

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterMessage\" type=\"checkbox\" value=\"OK\" %s/>Message</td>\n",
				 (config.digiFilter & FILTER_MESSAGE) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterStatus\" type=\"checkbox\" value=\"OK\" %s/>Status</td>\n",
				 (config.digiFilter & FILTER_STATUS) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterTelemetry\" type=\"checkbox\" value=\"OK\" %s/>Telemetry</td>\n",
				 (config.digiFilter & FILTER_TELEMETRY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterWeather\" type=\"checkbox\" value=\"OK\" %s/>Weather</td>\n",
				 (config.digiFilter & FILTER_WX) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterObject\" type=\"checkbox\" value=\"OK\" %s/>Object</td>\n",
				 (config.digiFilter & FILTER_OBJECT) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "</tr><tr style=\"background:unset;\"><td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterItem\" type=\"checkbox\" value=\"OK\" %s/>Item</td>\n",
				 (config.digiFilter & FILTER_ITEM) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterQuery\" type=\"checkbox\" value=\"OK\" %s/>Query</td>\n",
				 (config.digiFilter & FILTER_QUERY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterBuoy\" type=\"checkbox\" value=\"OK\" %s/>Buoy</td>\n",
				 (config.digiFilter & FILTER_BUOY) ? "checked" : "");
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"border:unset;\"><input class=\"field_checkbox\" name=\"FilterPosition\" type=\"checkbox\" value=\"OK\" %s/>Position</td>\n",
				 (config.digiFilter & FILTER_POSITION) ? "checked" : "");
		html->print(tempHtml);

		html->print("<td style=\"border:unset;\"></td>");
		html->print("</tr></table></fieldset>\n");
		html->print("</td></tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n");
		html->print("<td align=\"center\"><table>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"digiTlmInv\" name=\"digiTlmInv\" type=\"number\" value=\"%d\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>", config.digi_tlm_interval);
		html->print(tempHtml);
		for (int ax = 0; ax < 5; ax++)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>CH A%d:</b></td>\n", ax + 1);
			html->print(tempHtml);
			html->print("<td align=\"center\">\n");
			html->print("<table>");

			html->print("<tr><td style=\"text-align: right;\">Sensor:</td>\n");
			html->print("<td style=\"text-align: left;\">CH: ");

			snprintf(tempHtml, sizeof(tempHtml), "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax, ax);
			html->print(tempHtml);

			for (uint8_t idx = 0; idx < 11; idx++)
			{
				if (idx == 0)
				{
					if (config.digi_tlm_sensor[ax] == idx)
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>NONE</option>\n", idx);
					}
					else
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">NONE</option>\n", idx);
					}
				}
				else
				{
					if (config.digi_tlm_sensor[ax] == idx)
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>SENSOR#%d</option>\n", idx, idx);
					}
					else
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">SENSOR#%d</option>\n", idx, idx);
					}
				}
				html->print(tempHtml);
			}
			html->print("</select></td>\n");

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.digi_tlm_PARM[ax]);
			html->print(tempHtml);
			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.digi_tlm_UNIT[ax]);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision%d\" type=\"text\" value=\"%d\" onchange=\"selPrecision(%d)\"/></td></tr>\n", ax, config.digi_tlm_precision[ax], ax);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%da\" type=\"number\" value=\"%.5f\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%db\" type=\"number\" value=\"%.5f\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%dc\" type=\"number\" value=\"%.5f\" /> (av<sup>2</sup>+bv+c) </td>\n",
					 ax, config.digi_tlm_EQNS[ax][0], ax, config.digi_tlm_EQNS[ax][1], ax, config.digi_tlm_EQNS[ax][2]);
			html->print(tempHtml);

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset%d\" type=\"text\" value=\"%.5f\" onchange=\"selOffset(%d)\" /></td></tr>\n", ax, config.digi_tlm_offset[ax], ax);
			html->print(tempHtml);

			html->print("</table></td>");
			html->print("</tr>\n");
		}
		html->print("</table></td></tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitDIGI'  name=\"commitDIGI\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitDIGI\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");

		html->addHeader("digi", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
	String arg = "";

	if (request->hasArg("commitWX"))
	{
		for (int x = 0; x < WX_SENSOR_NUM; x++)
			config.wx_sensor_enable[x] = false;

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
		// Allocate initial memory for HTML content
		char tempHtml[300];
		log_e("[WX] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 28000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("document.getElementById(\"submitWX\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/wx',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		/************************ WX Mode **************************/
		html->print("<form id='formWX' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>[WX] Weather Station</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");
		char EnFlag[10] = "";
		if (config.wx_en)
			strcpy(EnFlag, "checked");

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", EnFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station Callsign:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"%s\" /></td>\n", config.wx_mycall);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station SSID:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"mySSID\" id=\"mySSID\">\n");
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			if (config.wx_ssid == ssid)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%d</option>\n", ssid, ssid);
			}
			else
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%d</option>\n", ssid, ssid);
			}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Object Name:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" name=\"Object\" type=\"text\" value=\"%s\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n", config.wx_object);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PATH:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"Path\" id=\"Path\">\n");
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			if (config.wx_path == pthIdx)
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
			}
			else
			{
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
			}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		// html->print("<td style=\"text-align: left;\"><input maxlength=\"72\" size=\"72\" name=\"Path\" type=\"text\" value=\"" + String(config.wx_path) + "\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Text Comment:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"50\" name=\"Comment\" type=\"text\" value=\"%s\" /></td>\n", config.wx_comment);
		html->print(tempHtml);
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Time Stamp:</b></td>\n");
		char timeStampFlag[10];
		if (config.wx_timestamp)
			strcpy(timeStampFlag, "checked");
		else
			strcpy(timeStampFlag, "");

		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wxTimeStamp\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", timeStampFlag);
		html->print(tempHtml);
		html->print("</tr>\n");

		html->print("<tr><td align=\"right\"><b>POSITION:</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" id=\"PosInv\" name=\"PosInv\" type=\"number\" value=\"%d\" />Sec.</td></tr>", config.wx_interval);
		html->print(tempHtml);
		String PosFixFlag = "";
		String PosGPSFlag = "";
		String Pos2RFFlag = "";
		String Pos2INETFlag = "";
		if (config.wx_gps)
			PosGPSFlag = "checked=\"checked\"";
		else
			PosFixFlag = "checked=\"checked\"";

		if (config.wx_2rf)
			Pos2RFFlag = "checked";
		if (config.wx_2inet)
			Pos2INETFlag = "checked";

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"PosSel\" value=\"0\" %s/>Fix <input type=\"radio\" name=\"PosSel\" value=\"1\" %s/>GPS </td></tr>\n", PosFixFlag.c_str(), PosGPSFlag.c_str());
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"Pos2RF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"Pos2INET\" value=\"OK\" %s/>Internet </td></tr>\n", Pos2RFFlag.c_str(), Pos2INETFlag.c_str());
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" name=\"PosLat\" type=\"number\" value=\"%.5f\" />degrees (positive for North, negative for South)</td></tr>\n", config.wx_lat);
		html->print(tempHtml);

		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" name=\"PosLon\" type=\"number\" value=\"%.5f\" />degrees (positive for East, negative for West)</td></tr>\n", config.wx_lon);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" name=\"PosAlt\" type=\"number\" value=\"%.2f\" /> meter. *The altitude in meters(m) above sea level</td></tr>\n", config.wx_alt);
		html->print(tempHtml);

		html->print("</table></td>");
		html->print("</tr>\n");

		// html->print("<tr>\n");
		// html->print("<td align=\"right\"><b>PORT:</b></td>\n");
		// html->print("<td style=\"text-align: left;\">\n");
		// html->print("<select name=\"channel\" id=\"channel\">\n");
		// for (int i = 0; i < 5; i++)
		// {
		// 	if (config.wx_channel == i)
		// 		html->print("<option value=\"" + String(i) + "\" selected>" + String(WX_PORT[i]) + " </option>\n");
		// 	else
		// 		html->print("<option value=\"" + String(i) + "\" >" + String(WX_PORT[i]) + " </option>\n");
		// }
		// html->print("</select>\n");
		// html->print("</td>\n");
		// html->print("</tr>\n");
		/************************ Sensor Config Mode **************************/
		// html->print("<form id='formSENSOR' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		// html->print("<table>\n");
		// html->print("<th colspan=\"2\"><span><b>Sensor Config</b></span></th>\n");

		html->print("<tr><td align=\"right\"><b>SENSOR:<br />Selection</b></td>\n");
		html->print("<td align=\"center\">\n");
		html->print("<table>");

		for (int ax = 0; ax < WX_SENSOR_NUM; ax++)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>%s:</b> \n", WX_SENSOR[ax]);
			html->print(tempHtml);

			char EnFlag[16] = "";
			if (config.wx_sensor_enable[ax])
				snprintf(EnFlag, sizeof(EnFlag), "checked");

			snprintf(tempHtml, sizeof(tempHtml), "<label class=\"switch\"><input type=\"checkbox\" name=\"senEn%d\" value=\"OK\" %s><span class=\"slider round\"></span></label>", ax, EnFlag);
			html->print(tempHtml);

			html->print("</td>\n");

			// html->print("<td style=\"text-align: lefe;\">Sensor:</td>\n");
			html->print("<td style=\"text-align: left;\">Sensor Channel: ");

			snprintf(tempHtml, sizeof(tempHtml), "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax, ax);
			html->print(tempHtml);
			for (uint8_t idx = 0; idx < 11; idx++)
			{
				if (idx == 0)
				{
					if (config.wx_sensor_ch[ax] == idx)
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>NONE</option>\n", idx);
					}
					else
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">NONE</option>\n", idx);
					}
				}
				else
				{
					if (config.wx_sensor_ch[ax] == idx)
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>SENSOR#%d</option>\n", idx, idx);
					}
					else
					{
						snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">SENSOR#%d</option>\n", idx, idx);
					}
				}
				html->print(tempHtml);
			}
			html->print("</select>\n");
			String avgFlag = "";
			String sampleFlag = "";
			if (config.wx_sensor_avg[ax])
				avgFlag = "checked=\"checked\"";
			else
				sampleFlag = "checked=\"checked\"";

			snprintf(tempHtml, sizeof(tempHtml), "<input type=\"radio\" name=\"avgSel%d\" value=\"0\" %s/>Sample <input type=\"radio\" name=\"avgSel%d\" value=\"1\" %s/>Average", ax, sampleFlag.c_str(), ax, avgFlag.c_str());
			html->print(tempHtml);
			html->print("</td></tr>");
		}
		html->print("</table></td></tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitWX'  name=\"commitWX\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitWX\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");

		html->addHeader("Weather", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 20000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("document.getElementById(\"submitTLM\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/tlm',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");

		/************************ TLM Mode **************************/
		html->print("<form id='formTLM' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>System Telemetry</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");
		char EnFlag[10] = "";
		if (config.tlm0_en)
			strcpy(EnFlag, "checked");
		{
			char *temp_flag = allocateStringMemory(512);
			if (temp_flag)
			{
				snprintf(temp_flag, 512, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"Enable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", EnFlag);
				html->print(temp_flag);
				free(temp_flag);
			}
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		{
			char *temp_callsign = allocateStringMemory(512);
			if (temp_callsign)
			{
				snprintf(temp_callsign, 512, "<td align=\"right\"><b>Station Callsign:</b></td>\n<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"%s\" /></td>\n", config.tlm0_mycall);
				html->print(temp_callsign);
				free(temp_callsign);
			}
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Station SSID:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"mySSID\" id=\"mySSID\">\n");
		for (uint8_t ssid = 0; ssid <= 15; ssid++)
		{
			char *temp_option = allocateStringMemory(256);
			if (temp_option)
			{
				if (config.tlm0_ssid == ssid)
				{
					snprintf(temp_option, 256, "<option value=\"%d\" selected>%d</option>\n", ssid, ssid);
				}
				else
				{
					snprintf(temp_option, 256, "<option value=\"%d\">%d</option>\n", ssid, ssid);
				}
				html->print(temp_option);
				free(temp_option);
			}
		}
		html->print("</select></td>\n");
		html->print("</tr>\n");

		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PATH:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"Path\" id=\"Path\">\n");
		for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
		{
			{
				char *temp_path = allocateStringMemory(256);
				if (temp_path)
				{
					if (config.tlm0_path == pthIdx)
					{
						snprintf(temp_path, 256, "<option value=\"%d\" selected>%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
					}
					else
					{
						snprintf(temp_path, 256, "<option value=\"%d\">%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
					}
					html->print(temp_path);
					free(temp_path);
				}
			}
		}
		html->print("</select></td>\n");
		// html->print("<td style=\"text-align: left;\"><input maxlength=\"72\" size=\"72\" name=\"Path\" type=\"text\" value=\"" + String(config.tlm0_path) + "\" /></td>\n");
		html->print("</tr>\n");
		html->print("<tr>\n");
		{
			char *temp_comment = allocateStringMemory(512);
			if (temp_comment)
			{
				snprintf(temp_comment, 512, "<td align=\"right\"><b>Text Comment:</b></td>\n<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"50\" name=\"Comment\" type=\"text\" value=\"%s\" /></td>\n", config.tlm0_comment);
				html->print(temp_comment);
				free(temp_comment);
			}
		}
		html->print("</tr>\n");

		{
			char *temp_intervals = allocateStringMemory(1024);
			if (temp_intervals)
			{
				snprintf(temp_intervals, 1024, "<tr><td style=\"text-align: right;\">Info Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" name=\"infoInv\" type=\"number\" value=\"%d\" />Sec.</td></tr>", config.tlm0_info_interval);
				html->print(temp_intervals);
				snprintf(temp_intervals, 1024, "<tr><td style=\"text-align: right;\">Data Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" name=\"dataInv\" type=\"number\" value=\"%d\" />Sec.</td></tr>", config.tlm0_data_interval);
				html->print(temp_intervals);
				free(temp_intervals);
			}
		}

		char Pos2RFFlag[10] = "";
		char Pos2INETFlag[10] = "";
		if (config.tlm0_2rf)
			strcpy(Pos2RFFlag, "checked");
		if (config.tlm0_2inet)
			strcpy(Pos2INETFlag, "checked");
		{
			char *temp_channels = allocateStringMemory(1024);
			if (temp_channels)
			{
				snprintf(temp_channels, 1024, "<tr><td style=\"text-align: right;\">TX Channel:</td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"Pos2RF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"Pos2INET\" value=\"OK\" %s/>Internet </td></tr>\n", Pos2RFFlag, Pos2INETFlag);
				html->print(temp_channels);
				free(temp_channels);
			}
		}

		// html->print("<tr>\n");
		// html->print("<td align=\"right\"><b>Time Stamp:</b></td>\n");
		// String timeStampFlag = "";
		// if (config.wx_timestamp)
		// 	timeStampFlag = "checked";
		// html->print("<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wxTimeStamp\" value=\"OK\" " + timeStampFlag + "><span class=\"slider round\"></span></label></td>\n");
		// html->print("</tr>\n");
		for (int ax = 0; ax < 5; ax++)
		{
			{
				char *temp_channel_a = allocateStringMemory(512);
				if (temp_channel_a)
				{
					snprintf(temp_channel_a, 512, "<tr><td align=\"right\"><b>Channel A%d:</b></td>\n", ax + 1);
					html->print(temp_channel_a);
					free(temp_channel_a);
				}
			}
			html->print("<td align=\"center\">\n");
			html->print("<table>");

			// html->print("<tr><td style=\"text-align: right;\">Name:</td><td style=\"text-align: center;\"><i>Sensor Type</i></td><td style=\"text-align: center;\"><i>Parameter</i></td><td style=\"text-align: center;\"><i>Unit</i></td></tr>\n");

			html->print("<tr><td style=\"text-align: right;\">Type/Name:</td>\n");
			html->print("<td style=\"text-align: left;\">Sensor Type: ");
			{
				char *temp_select = allocateStringMemory(256);
				if (temp_select)
				{
					snprintf(temp_select, 256, "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax, ax);
					html->print(temp_select);
					free(temp_select);
				}
			}
			for (uint8_t idx = 0; idx < SYSTEM_LEN; idx++)
			{
				{
					char *temp_option = allocateStringMemory(256);
					if (temp_option)
					{
						if (config.tml0_data_channel[ax] == idx)
						{
							snprintf(temp_option, 256, "<option value=\"%d\" selected>%s</option>\n", idx, SYSTEM_NAME[idx]);
						}
						else
						{
							snprintf(temp_option, 256, "<option value=\"%d\">%s</option>\n", idx, SYSTEM_NAME[idx]);
						}
						html->print(temp_option);
						free(temp_option);
					}
				}
			}
			html->print("</select></td>\n");

			{
				char *temp_param = allocateStringMemory(512);
				if (temp_param)
				{
					snprintf(temp_param, 512, "<td style=\"text-align: left;\">Parameter: <input maxlength=\"10\" size=\"8\" name=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.tlm0_PARM[ax]);
					html->print(temp_param);
					free(temp_param);
				}
			}
			{
				char *temp_unit = allocateStringMemory(512);
				if (temp_unit)
				{
					snprintf(temp_unit, 512, "<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.tlm0_UNIT[ax]);
					html->print(temp_unit);
					free(temp_unit);
				}
			}
			html->print("</tr>\n");
			{
				char *temp_eqns = allocateStringMemory(1024);
				if (temp_eqns)
				{
					snprintf(temp_eqns, 1024, "<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns%da\" type=\"number\" value=\"%.3f\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns%db\" type=\"number\" value=\"%.3f\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.0001\" name=\"eqns%dc\" type=\"number\" value=\"%.3f\" /> (av<sup>2</sup>+bv+c)</td></tr>\n",
							 ax, config.tlm0_EQNS[ax][0], ax, config.tlm0_EQNS[ax][1], ax, config.tlm0_EQNS[ax][2]);
					html->print(temp_eqns);
					free(temp_eqns);
				}
			}
			html->print("</table></td>");
			html->print("</tr>\n");
		}

		uint8_t b = 1;
		for (int ax = 0; ax < 8; ax++)
		{
			{
				char *temp_channel_b = allocateStringMemory(512);
				if (temp_channel_b)
				{
					snprintf(temp_channel_b, 512, "<tr><td align=\"right\"><b>Channel B%d:</b></td>\n", ax + 1);
					html->print(temp_channel_b);
					free(temp_channel_b);
				}
			}
			html->print("<td align=\"center\">\n");
			html->print("<table>");

			// html->print("<tr><td style=\"text-align: right;\">Type/Name:</td>\n");
			html->print("<td style=\"text-align: left;\">Type: ");
			{
				char *temp_select_b = allocateStringMemory(256);
				if (temp_select_b)
				{
					snprintf(temp_select_b, 256, "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax + 5, ax);
					html->print(temp_select_b);
					free(temp_select_b);
				}
			}
			for (uint8_t idx = 0; idx < SYSTEM_BIT_LEN; idx++)
			{
				{
					char *temp_option_b = allocateStringMemory(256);
					if (temp_option_b)
					{
						if (config.tml0_data_channel[ax + 5] == idx)
						{
							snprintf(temp_option_b, 256, "<option value=\"%d\" selected>%s</option>\n", idx, SYSTEM_BITS_NAME[idx]);
						}
						else
						{
							snprintf(temp_option_b, 256, "<option value=\"%d\">%s</option>\n", idx, SYSTEM_BITS_NAME[idx]);
						}
						html->print(temp_option_b);
						free(temp_option_b);
					}
				}
			}
			html->print("</select></td>\n");

			{
				char *temp_param_b = allocateStringMemory(512);
				if (temp_param_b)
				{
					snprintf(temp_param_b, 512, "<td style=\"text-align: left;\">Parameter: <input maxlength=\"10\" size=\"8\" name=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax + 5, config.tlm0_PARM[ax + 5]);
					html->print(temp_param_b);
					free(temp_param_b);
				}
			}
			{
				char *temp_unit_b = allocateStringMemory(512);
				if (temp_unit_b)
				{
					snprintf(temp_unit_b, 512, "<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax + 5, config.tlm0_UNIT[ax + 5]);
					html->print(temp_unit_b);
					free(temp_unit_b);
				}
			}
			char LowFlag[20] = "", HighFlag[20] = "";
			if (config.tlm0_BITS_Active & b)
				strcpy(HighFlag, "checked=\"checked\"");
			else
				strcpy(LowFlag, "checked=\"checked\"");
			{
				char *temp_radio_b = allocateStringMemory(512);
				if (temp_radio_b)
				{
					snprintf(temp_radio_b, 512, "<td style=\"text-align: left;\"> Active:<input type=\"radio\" name=\"bitact%d\" value=\"0\" %s/>LOW <input type=\"radio\" name=\"bitact%d\" value=\"1\" %s/>HIGH </td>\n", ax, LowFlag, ax, HighFlag);
					html->print(temp_radio_b);
					free(temp_radio_b);
				}
			}
			html->print("</tr>\n");
			// html->print("<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax + 1) + "a\" type=\"number\" value=\"" + String(config.tlm0_EQNS[ax][0], 3) + "\" />  b:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax + 1) + "b\" type=\"number\" value=\"" + String(config.tlm0_EQNS[ax][1], 3) + "\" /> c:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax + 1) + "c\" type=\"number\" value=\"" + String(config.tlm0_EQNS[ax][2], 3) + "\" /> (av<sup>2</sup>+bv+c)</td></tr>\n";
			html->print("</table></td>");
			html->print("</tr>\n");
			b <<= 1;
		}

		// html->print("<tr><td align=\"right\"><b>Parameter Name:</b></td>\n";
		// strcat(html, "<td align=\"center\">\n";
		// strcat(html, "<table>";

		// // strcat(html, "<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" name=\"PosLat\" type=\"number\" value=\"" + String(config.wx_lat, 5) + "\" />degrees (positive for North, negative for South)</td></tr>\n";
		// // strcat(html, "<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" name=\"PosLon\" type=\"number\" value=\"" + String(config.wx_lon, 5) + "\" />degrees (positive for East, negative for West)</td></tr>\n";
		// // strcat(html, "<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" name=\"PosAlt\" type=\"number\" value=\"" + String(config.wx_alt, 2) + "\" /> meter. *Value 0 is not send height</td></tr>\n";
		// strcat(html, "</table></td>";
		// strcat(html, "</tr>\n";
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitTLM'  name=\"commitTLM\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitTLM\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");

		html->addHeader("Telemetry", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

extern TaskHandle_t taskSensorHandle;

void handle_sensor(AsyncWebServerRequest *request)
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
		// Allocate initial memory for HTML content
		char temp_buffer[512];
		log_e("[SENSOR] before alloc: freeHeap=%u largestFreeBlock=%u", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		AsyncResponseStream *html = request->beginResponseStream("text/html", 36000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("document.getElementById(\"submitSENSOR\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/sensor',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("function setElm(name,val) {\n");
		html->print("document.getElementById(name).value=val;\n");
		html->print("};\n");
		html->print("function selSensorType(idx) {\n");
		html->print("var x=0;\n");
		html->print("var parm=\"param\"+idx;\n");
		html->print("var unit=\"unit\"+idx;\n");
		html->print("x = document.getElementById(\"sensorCH\"+idx).value;\n");
		html->print("if (x==1) {\n");
		html->print("setElm(parm,\"Co2\");");
		html->print("setElm(unit,\"ppm\");\n");
		html->print("}else if (x==2) {\n");
		html->print("setElm(parm,\"CH2O\");");
		html->print("setElm(unit,\"μg/m³\");\n");
		html->print("}else if (x==3) {\n");
		html->print("setElm(parm,\"TVOC\");");
		html->print("setElm(unit,\"μg/m³\");\n");
		html->print("}else if (x==4) {\n");
		html->print("setElm(parm,\"PM2.5\");");
		html->print("setElm(unit,\"μg/m³\");\n");
		html->print("}else if (x==5) {\n");
		html->print("setElm(parm,\"PM10.0\");");
		html->print("setElm(unit,\"μg/m³\");\n");
		html->print("}else if (x==6) {\n");
		html->print("setElm(parm,\"Temperature\");");
		html->print("setElm(unit,\"°C\");\n");
		html->print("}else if (x==7) {\n");
		html->print("setElm(parm,\"Humidity\");");
		html->print("setElm(unit,\"%RH\");\n");
		html->print("}else if (x==8) {\n");
		html->print("setElm(parm,\"Pressure\");");
		html->print("setElm(unit,\"hPa\");\n");
		html->print("}else if (x==9) {\n");
		html->print("setElm(parm,\"WindSpeed\");");
		html->print("setElm(unit,\"kPh\");\n");
		html->print("}else if (x==10) {\n");
		html->print("setElm(parm,\"WindCourse\");");
		html->print("setElm(unit,\"°\");\n");
		html->print("}else if (x==11) {\n");
		html->print("setElm(parm,\"Rain\");");
		html->print("setElm(unit,\"mm\");\n");
		html->print("}else if (x==12) {\n");
		html->print("setElm(parm,\"Luminosity\");");
		html->print("setElm(unit,\"W/m³\");\n");
		html->print("}else if (x==13) {\n");
		html->print("setElm(parm,\"SoilTemp\");");
		html->print("setElm(unit,\"°C\");\n");
		html->print("}else if (x==14) {\n");
		html->print("setElm(parm,\"SoilMoisture\");");
		html->print("setElm(unit,\"%VWC\");\n");
		html->print("}else if (x==15) {\n");
		html->print("setElm(parm,\"WaterTemp\");");
		html->print("setElm(unit,\"°C\");\n");
		html->print("}else if (x==16) {\n");
		html->print("setElm(parm,\"WaterTDS\");");
		html->print("setElm(unit,\" \");\n");
		html->print("}else if (x==17) {\n");
		html->print("setElm(parm,\"WaterLevel\");");
		html->print("setElm(unit,\"mm\");\n");
		html->print("}else if (x==18) {\n");
		html->print("setElm(parm,\"WaterFlow\");");
		html->print("setElm(unit,\"L/min\");\n");
		html->print("}else if (x==19) {\n");
		html->print("setElm(parm,\"Voltage\");");
		html->print("setElm(unit,\"V\");\n");
		html->print("}else if (x==20) {\n");
		html->print("setElm(parm,\"Current\");");
		html->print("setElm(unit,\"A\");\n");
		html->print("}else if (x==21) {\n");
		html->print("setElm(parm,\"Power\");");
		html->print("setElm(unit,\"W\");\n");
		html->print("}else if (x==22) {\n");
		html->print("setElm(parm,\"Energy\");");
		html->print("setElm(unit,\"Wh\");\n");
		html->print("}else if (x==23) {\n");
		html->print("setElm(parm,\"Frequency\");");
		html->print("setElm(unit,\"Hz\");\n");
		html->print("}else if (x==24) {\n");
		html->print("setElm(parm,\"PF\");");
		html->print("setElm(unit,\" \");\n");
		html->print("}else if (x==25) {\n");
		html->print("setElm(parm,\"Satellite\");");
		html->print("setElm(unit,\" \");\n");
		html->print("}else if (x==26) {\n");
		html->print("setElm(parm,\"HDOP\");");
		html->print("setElm(unit,\" \");\n");
		html->print("}else if (x==27) {\n");
		html->print("setElm(parm,\"Battery\");");
		html->print("setElm(unit,\"V\");\n");
		html->print("}else if (x==28) {\n");
		html->print("setElm(parm,\"BattLevel\");");
		html->print("setElm(unit,\"%\");\n");
		html->print("}\n}\n");

		html->print("function selSensor(idx) {\n");
		html->print("var x=0;\n");
		html->print("x = document.getElementById(\"sensorP\"+idx).value;\n");
		html->print("if (x>=10 && x<=13) {\n");
#ifdef TTGO_T_Beam_S3_SUPREME_V3
		html->print("document.getElementById(\"address\"+idx).value=119;\n");
#else
		html->print("document.getElementById(\"address\"+idx).value=118;\n");
#endif
		html->print("}else if (x==16 || x==17) {\n");
		html->print("document.getElementById(\"address\"+idx).value=90;\n");
		html->print("}else if (x==23) {\n");
		html->print("document.getElementById(\"address\"+idx).value=1;\n");
		html->print("}else if (x==24 || x==25) {\n");
		html->print("document.getElementById(\"address\"+idx).value=1000;\n");
		html->print("}else{\n");
		html->print("document.getElementById(\"address\"+idx).value=0;\n");
		html->print("}\n}\n");
		html->print("</script>\n");

		/************************ Sensor Monitor **************************/
		// html->print("<form id='formSENSOR' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"5\"><span><b>Sensor Monitor</b></span></th>\n");
		int ax = 0;
		for (int r = 0; r < 3; r++)
		{
			html->print("<tr>\n");
			for (int c = 0; c < 5; c++)
			{
				html->print("<td align=\"center\">\n");
				if (config.sensor[ax].enable)
				{
					snprintf(temp_buffer, sizeof(temp_buffer), "<fieldset id=\"SenGrp%d\">\n", ax + 1);
					html->print(temp_buffer);
				}
				else
				{
					snprintf(temp_buffer, sizeof(temp_buffer), "<fieldset id=\"SenGrp%d\" disabled>\n", ax + 1);
					html->print(temp_buffer);
				}

				snprintf(temp_buffer, sizeof(temp_buffer), "<legend>SEN#%d-%s</legend>\n", ax + 1, config.sensor[ax].parm);
				html->print(temp_buffer);
				snprintf(temp_buffer, sizeof(temp_buffer), "<input id=\"sVal%d\" style=\"text-align:right;\" size=\"5\" type=\"text\" value=\"%.2f\" readonly/> %s\n", ax, sen[ax].sample, config.sensor[ax].unit);
				html->print(temp_buffer);
				html->print("</td>\n");
				ax++;
				if (ax >= SENSOR_NUMBER)
					break;
			}
			html->print("</tr>\n");
			if (ax >= SENSOR_NUMBER)
				break;
		}
		html->print("</table>< /br>\n");

		/************************ Sensor Config Mode **************************/
		html->print("<form id='formSENSOR' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>Sensor Config</b></span></th>\n");
		String EnFlag = "";

		for (int ax = 0; ax < SENSOR_NUMBER; ax++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td align=\"right\"><b>SENSOR#%d:</b><br />\n", ax + 1);
			html->print(temp_buffer);
			EnFlag = "";
			if (config.sensor[ax].enable)
				EnFlag = "checked";
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<label class=\"switch\"><input type=\"checkbox\" name=\"En%d\" value=\"OK\" %s><span class=\"slider round\"></span></label>", ax, EnFlag.c_str());
				html->print(temp_buffer);
			}
			html->print("</td><td align=\"center\">\n");
			html->print("<table>");

			html->print("<tr><td style=\"text-align: right;\">Type:</td>\n");
			html->print("<td style=\"text-align: left;\">");
			{
				snprintf(temp_buffer, sizeof(temp_buffer), "<select name=\"sensorCH%d\" id=\"sensorCH%d\" onchange=\"selSensorType(%d)\">\n", ax, ax, ax);
				html->print(temp_buffer);
			}
			// for (uint8_t idx = 0; idx < SENSOR_NAME_NUM; idx++)
			// {
			// 	if (config.sensor[ax].type == idx)
			// 	{
			// 		html->print("<option value=\"" + String(idx) + "\" selected>" + String(SENSOR_NAME[idx]) + "</option>\n");
			// 	}
			// 	else
			// 	{
			// 		html->print("<option value=\"" + String(idx) + "\">" + String(SENSOR_NAME[idx]) + "</option>\n");
			// 	}
			// }
			html->print("</select></td>\n");

			snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\">Name: <input maxlength=\"15\" size=\"15\" name=\"param%d\" id=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax, ax, config.sensor[ax].parm);
			html->print(temp_buffer);

			snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\">Unit: <input maxlength=\"10\" size=\"5\" name=\"unit%d\" id=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax, ax, config.sensor[ax].unit);
			html->print(temp_buffer);
			html->print("</tr>\n");
			// html->print("<tr><td style=\"text-align: right;\">Port:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax) + "a\" type=\"number\" value=\"" + String(config.sensor[ax].eqns[0], 3) + "\" />  b:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax) + "b\" type=\"number\" value=\"" + String(config.sensor[ax].eqns[1], 3) + "\" /> c:<input min=\"-999\" max=\"999\" step=\"0.1\" name=\"eqns" + String(ax) + "c\" type=\"number\" value=\"" + String(config.sensor[ax].eqns[2], 3) + "\" /> (av<sup>2</sup>+bv+c)</td></tr>\n";
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td style=\"text-align: right;\">PORT:</td>\n<td style=\"text-align: left;\">\n<select name=\"sensorP%d\" id=\"sensorP%d\" onchange=\"selSensor(%d)\">\n", ax, ax, ax);
			html->print(temp_buffer);
			// for (uint8_t idx = 0; idx < SENSOR_PORT_NUM; idx++)
			// {
			// 	if (config.sensor[ax].port == idx)
			// 	{
			// 		html->print("<option value=\"" + String(idx) + "\" selected>" + String(SENSOR_PORT[idx]) + "</option>\n";
			// 	}
			// 	else
			// 	{
			// 		strcat(html, "<option value=\"" + String(idx) + "\">" + String(SENSOR_PORT[idx]) + "</option>\n";
			// 	}
			// }
			html->print("</select></td>\n");
			snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\">Addr/Reg/GPIO: <input style=\"text-align:right;\" min=\"0\" max=\"6500\" step=\"1\" name=\"address%d\" id=\"address%d\" type=\"number\" value=\"%d\" /></td>\n", ax, ax, config.sensor[ax].address);
			html->print(temp_buffer);
			snprintf(temp_buffer, sizeof(temp_buffer), "<td style=\"text-align: left;\">Sample: <input style=\"text-align:right;\" min=\"0\" max=\"9999\" step=\"1\" name=\"sample%d\" type=\"number\" value=\"%d\" />Sec.\n", ax, config.sensor[ax].samplerate);
			html->print(temp_buffer);
			snprintf(temp_buffer, sizeof(temp_buffer), "Average: <input style=\"text-align:right;\" min=\"0\" max=\"999\" step=\"1\" name=\"avg%d\" type=\"number\" value=\"%d\" />Sec.</td></tr>\n", ax, config.sensor[ax].averagerate);
			html->print(temp_buffer);
			snprintf(temp_buffer, sizeof(temp_buffer), "<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns%da\" type=\"number\" value=\"%.5f\" />  b:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns%db\" type=\"number\" value=\"%.5f\" /> c:<input min=\"-999\" max=\"999\" step=\"0.00001\" name=\"eqns%dc\" type=\"number\" value=\"%.5f\" /> (av<sup>2</sup>+bv+c)</td></tr>\n",
							 ax, config.sensor[ax].eqns[0], ax, config.sensor[ax].eqns[1], ax, config.sensor[ax].eqns[2]);
			html->print(temp_buffer);
			html->print("</table></td>");
			html->print("</tr>\n");
		}

		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitSENSOR'  name=\"commitSENSOR\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitSENSOR\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");

		html->print("<script type=\"text/javascript\">\n");
		html->print("if (typeof typeArry === 'undefined'){let typeArry = [];};\n");
		html->print("typeArry = new Array(");
		for (uint8_t idx = 0; idx < SENSOR_NAME_NUM; idx++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "'%s'", SENSOR_NAME[idx]);
			html->print(temp_buffer);
			if (idx < SENSOR_NAME_NUM - 1)
				html->print(",");
		}
		html->print(");\n");
		html->print("if (typeof portArry === 'undefined'){let portArry = [];};\n");
		html->print("portArry = new Array(");
		for (uint8_t idx = 0; idx < SENSOR_PORT_NUM; idx++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "'%s'", SENSOR_PORT[idx]);
			html->print(temp_buffer);
			if (idx < SENSOR_PORT_NUM - 1)
				html->print(",");
		}
		html->print(");\n");
		// html->print("delete typeSel;delete listType;delete portSel;delete listPort;\n";
		html->print("if (typeof typeSel === 'undefined'){var typeSel = [];};\n");
		html->print("if (typeof listType === 'undefined'){var listType = [];};\n");
		html->print("if (typeof portSel === 'undefined'){var portSel = [];};\n");
		html->print("if (typeof listPort === 'undefined'){var listPort = [];};\n");
		for (int i = 0; i < 10; i++)
		{
			snprintf(temp_buffer, sizeof(temp_buffer), "listType[%d] = document.querySelector('#sensorCH%d');typeSel[%d]=%d;\n", i, i, i, config.sensor[i].type);
			html->print(temp_buffer);
			snprintf(temp_buffer, sizeof(temp_buffer), "listPort[%d] = document.querySelector('#sensorP%d');portSel[%d]=%d;\n", i, i, i, config.sensor[i].port);
			html->print(temp_buffer);
		}

		html->print("for (let n = 0; n < 10; n++){\n");
		html->print("for (let i = 0; i < typeArry.length; i++) {\n");
		html->print("const optionType = new Option(typeArry[i], i);\n");
		html->print("listType[n].add(optionType, undefined);\n");
		html->print("};\n");
		html->print("listType[n].options[typeSel[n]].selected = true;\n");
		html->print("for (let p = 0; p < portArry.length; p++) {\n");
		html->print("const optionPort = new Option(portArry[p], p);\n");
		html->print("listPort[n].add(optionPort, undefined);\n");
		html->print("};\n");
		html->print("listPort[n].options[portSel[n]].selected = true;\n");
		html->print("};\n");

		html->print("</script>\n");

		html->addHeader("Sensor", "content");
		html->addHeader("Cache-Control", "no-cache");
		request->send(html);
	}
}

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

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	char tempHtml[500];
	AsyncResponseStream *html = request->beginResponseStream("text/html", 30000);
	if (!html)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	html->print("<script type=\"text/javascript\">\n");
	html->print("$('form').submit(function (e) {\n");
	html->print("e.preventDefault();\n");
	html->print("var data = new FormData(e.currentTarget);\n");
	html->print("document.getElementById(\"submitTRACKER\").disabled=true;\n");
	html->print("$.ajax({\n");
	html->print("url: '/tracker',\n");
	html->print("type: 'POST',\n");
	html->print("data: data,\n");
	html->print("contentType: false,\n");
	html->print("processData: false,\n");
	html->print("success: function (data) {\n");
	html->print("alert(\"Submited Successfully\");\n");
	html->print("},\n");
	html->print("error: function (data) {\n");
	html->print("alert(\"An error occurred.\");\n");
	html->print("}\n");
	html->print("});\n");
	html->print("});\n");
	html->print("</script>\n<script type=\"text/javascript\">\n");
	html->print("function openWindowSymbol(sel) {\n");
	html->print("var i, l, options = [{\n");
	html->print("value: 'first',\n");
	html->print("text: 'First'\n");
	html->print("}, {\n");
	html->print("value: 'second',\n");
	html->print("text: 'Second'\n");
	html->print("}],\n");
	html->print("newWindow = window.open(\"/symbol?sel=\"+sel.toString(), null, \"height=400,width=400,status=no,toolbar=no,menubar=no,location=no\");\n");
	html->print("}\n");

	html->print("function setValue(sel,symbol,table) {\n");
	html->print("var txtsymbol=document.getElementById('trackerSymbol');\n");
	html->print("var txttable=document.getElementById('trackerTable');\n");
	html->print("var imgicon=document.getElementById('trackerImgSymbol');\n");
	html->print("if(sel==1){\n");
	html->print("txtsymbol=document.getElementById('moveSymbol');\n");
	html->print("txttable=document.getElementById('moveTable');\n");
	html->print("imgicon= document.getElementById('moveImgSymbol');\n");
	html->print("}else if(sel==2){\n");
	html->print("txtsymbol=document.getElementById('stopSymbol');\n");
	html->print("txttable=document.getElementById('stopTable');\n");
	html->print("imgicon= document.getElementById('stopImgSymbol');\n");
	html->print("}\n");
	html->print("txtsymbol.value = String.fromCharCode(symbol);\n");
	html->print("if(table==1){\n txttable.value='/';\n");
	html->print("}else if(table==2){\n txttable.value='\\\\';\n}\n");
	html->print("imgicon.src = \"http://aprs.nakhonthai.net/symbols/icons/\"+symbol.toString()+'-'+table.toString()+'.png';\n");
	html->print("\n}\n");
	html->print("function onSmartCheck() {\n");
	html->print("if (document.querySelector('#smartBcnEnable').checked) {\n");
	// Checkbox has been checked
	html->print("document.getElementById(\"smartbcnGrp\").disabled=false;\n");
	html->print("} else {\n");
	// Checkbox has been unchecked
	html->print("document.getElementById(\"smartbcnGrp\").disabled=true;\n");
	html->print("}\n}\n");

	html->print("function selPrecision(idx) {\n");
	html->print("var x=0;\n");
	html->print("x = document.getElementsByName(\"precision\"+idx)[0].value;\n");
	html->print("document.getElementsByName(\"eqns\"+idx+\"b\")[0].value=1/Math.pow(10,x);\n");
	html->print("}\n");
	html->print("function selOffset(idx) {\n");
	html->print("var x=0;\n");
	html->print("x = document.getElementsByName(\"offset\"+idx)[0].value;\n");
	html->print("document.getElementsByName(\"eqns\"+idx+\"c\")[0].value=x*(-1);\n");
	html->print("}\n");
	html->print("</script>\n");

	delay(1);
	/************************ tracker Mode **************************/
	html->print("<form id='formtracker' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
	// html->print("<h2>[TRACKER] Tracker Position Mode</h2>\n");
	html->print("<table>\n");
	// html->print("<tr>\n");
	// html->print("<th width=\"200\"><span><b>Setting</b></span></th>\n");
	// html->print("<th><span><b>Value</b></span></th>\n");
	// html->print("</tr>\n");
	html->print("<th colspan=\"2\"><span><b>[TRACKER] Tracker Position Mode</b></span></th>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Enable:</b></td>\n");
	char trackerEnFlag[10] = "";
	if (config.trk_en)
		strcpy(trackerEnFlag, "checked");
	else
		strcpy(trackerEnFlag, "");

	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"trackerEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", trackerEnFlag);
	html->print(tempHtml);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Station Callsign:</b></td>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"7\" size=\"6\" id=\"myCall\" name=\"myCall\" type=\"text\" value=\"%s\" /></td>\n", config.trk_mycall);
	html->print(tempHtml);

	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Station SSID:</b></td>\n");
	html->print("<td style=\"text-align: left;\">\n");
	html->print("<select name=\"mySSID\" id=\"mySSID\">\n");
	for (uint8_t ssid = 0; ssid <= 15; ssid++)
	{
		if (config.trk_ssid == ssid)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%d</option>\n", ssid, ssid);
		}
		else
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%d</option>\n", ssid, ssid);
		}
		html->print(tempHtml);
	}
	html->print("</select></td>\n");
	html->print("</tr>\n");

	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Item/Obj Name:</b></td>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"9\" size=\"9\" id=\"trackerObject\" name=\"trackerObject\" type=\"text\" value=\"%s\" /><i> *If not used, leave it blank.In use 3-9 charactor</i></td>\n", config.trk_item);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>PATH:</b></td>\n");
	html->print("<td style=\"text-align: left;\">\n");
	html->print("<select name=\"trackerPath\" id=\"trackerPath\">\n");
	for (uint8_t pthIdx = 0; pthIdx < PATH_LEN; pthIdx++)
	{
		if (config.trk_path == pthIdx)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
		}
		else
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%s</option>\n", pthIdx, PATH_NAME[pthIdx]);
		}
		html->print(tempHtml);
	}
	html->print("</select></td>\n");
	// html->print("<td style=\"text-align: left;\"><input maxlength=\"72\" size=\"72\" id=\"trackerPath\" name=\"trackerPath\" type=\"text\" value=\"" + String(config.trk_path) + "\" /></td>\n";
	html->print("</tr>\n");

	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Text Comment:</b></td>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"25\" size=\"30\" id=\"trackerComment\" name=\"trackerComment\" type=\"text\" value=\"%s\" /></td>\n", config.trk_comment);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Text Status:</b></td>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"50\" size=\"60\" id=\"trkStatus\" name=\"trkStatus\" type=\"text\" value=\"%s\" />  Interval:<input min=\"0\" max=\"3600\" step=\"1\" name=\"trkSTSInv\" type=\"number\" value=\"%d\" />Sec.</td>\n", config.trk_status, config.trk_sts_interval);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Smart Beacon:</b></td>\n");
	char smartBcnEnFlag[10] = "";
	if (config.trk_smartbeacon)
		strcpy(smartBcnEnFlag, "checked");

	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" id=\"smartBcnEnable\" name=\"smartBcnEnable\" onclick=\"onSmartCheck()\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch use to smart beacon mode</i></label></td>\n", smartBcnEnFlag);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Compress:</b></td>\n");
	char compressEnFlag[10] = "";
	if (config.trk_compress)
		strcpy(compressEnFlag, "checked");

	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"compressEnable\" value=\"OK\" %s><span class=\"slider round\"></span></label><label style=\"vertical-align: bottom;font-size: 8pt;\"><i> *Switch compress packet</i></label></td>\n", compressEnFlag);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Mic-E Type:</b></td>\n");
	html->print("<td style=\"text-align: left;\">\n");
	html->print("<select name=\"trkMicEType\" id=\"trkMicEType\">\n");
	for (uint8_t micEIdx = 0; micEIdx < 9; micEIdx++)
	{
		if (config.trk_mice_type == micEIdx)
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%s</option>\n", micEIdx, MIC_E_MSG[micEIdx]);
		}
		else
		{
			snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">%s</option>\n", micEIdx, MIC_E_MSG[micEIdx]);
		}
		html->print(tempHtml);
	}
	html->print("</select><label style=\"vertical-align: bottom;font-size: 8pt;\"><i>*Support if Compress is enabled and not use Item/Obj,Time Stamp or UnUsed</i></label></td>\n");
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Time Stamp:</b></td>\n");

	char timeStampFlag[10] = "";
	if (config.trk_timestamp)
		strcpy(timeStampFlag, "checked");
	{
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"trackerTimeStamp\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", timeStampFlag);
		html->print(tempHtml);
	}
	html->print("</tr>\n");
	char trackerPos2RFFlag[10] = "";
	char trackerPos2INETFlag[10] = "";
	if (config.trk_loc2rf)
		strcpy(trackerPos2RFFlag, "checked");
	if (config.trk_loc2inet)
		strcpy(trackerPos2INETFlag, "checked");
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\"><b>TX Channel:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"trackerPos2RF\" value=\"OK\" %s/>RF <input type=\"checkbox\" name=\"trackerPos2INET\" value=\"OK\" %s/>Internet </td></tr>\n", trackerPos2RFFlag, trackerPos2INETFlag);
	html->print(tempHtml);
	char trackerOptBatFlag[10] = "";
	char trackerOptSatFlag[10] = "";
	char trackerOptAltFlag[10] = "";
	char trackerOptCSTFlag[10] = "";
	if (config.trk_rssi)
		strcpy(trackerOptBatFlag, "checked");
	if (config.trk_sat)
		strcpy(trackerOptSatFlag, "checked");
	if (config.trk_altitude)
		strcpy(trackerOptAltFlag, "checked");
	if (config.trk_log)
		strcpy(trackerOptCSTFlag, "checked");

	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\"><b>Option:</b></td><td style=\"text-align: left;\"><input type=\"checkbox\" name=\"trackerOptCST\" value=\"OK\" %s/>Telemetry <input type=\"checkbox\" name=\"trackerOptAlt\" value=\"OK\" %s/>Altutude <input type=\"checkbox\" name=\"trackerOptBat\" value=\"OK\" %s/>Audio Request </td></tr>\n",
			 trackerOptCSTFlag, trackerOptAltFlag, trackerOptBatFlag);
	html->print(tempHtml);

	html->print("<tr>");
	html->print("<td align=\"right\"><b>POSITION:</b></td>\n");
	html->print("<td align=\"center\">\n");
	html->print("<table>");
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"3600\" step=\"1\" id=\"trackerPosInv\" name=\"trackerPosInv\" type=\"number\" value=\"%d\" />Sec.</label></td></tr>", config.trk_interval);
	html->print(tempHtml);
	char trackerPosFixFlag[20] = "";
	char trackerPosGPSFlag[20] = "";

	if (config.trk_gps)
		strcpy(trackerPosGPSFlag, "checked=\"checked\"");
	else
		strcpy(trackerPosFixFlag, "checked=\"checked\"");

	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Location:</td><td style=\"text-align: left;\"><input type=\"radio\" name=\"trackerPosSel\" value=\"0\" %s/>Fix <input type=\"radio\" name=\"trackerPosSel\" value=\"1\" %s/>GPS </td></tr>\n",
			 trackerPosFixFlag, trackerPosGPSFlag);
	html->print(tempHtml);
	html->print("<tr>\n");
	html->print("<td align=\"right\">Symbol Icon:</td>\n");
	char table[5] = "1";
	if (config.trk_symbol[0] == 47)
		strcpy(table, "1");
	if (config.trk_symbol[0] == 92)
		strcpy(table, "2");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"trackerTable\" name=\"trackerTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"trackerSymbol\" name=\"trackerSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"trackerImgSymbol\" onclick=\"openWindowSymbol(0);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%s.png\"> <i>*Click icon for select symbol</i></td>\n",
			 config.trk_symbol[0], config.trk_symbol[1], (int)config.trk_symbol[1], table);
	html->print(tempHtml);
	html->print("</tr>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Latitude:</td><td style=\"text-align: left;\"><input min=\"-90\" max=\"90\" step=\"0.00001\" id=\"trackerPosLat\" name=\"trackerPosLat\" type=\"number\" value=\"%.5f\" />degrees (positive for North, negative for South)</td></tr>\n", config.trk_lat);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Longitude:</td><td style=\"text-align: left;\"><input min=\"-180\" max=\"180\" step=\"0.00001\" id=\"trackerPosLon\" name=\"trackerPosLon\" type=\"number\" value=\"%.5f\" />degrees (positive for East, negative for West)</td></tr>\n", config.trk_lon);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Altitude:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"10000\" step=\"0.1\" id=\"trackerPosAlt\" name=\"trackerPosAlt\" type=\"number\" value=\"%.2f\" /> meter. *Value 0 is not send height</td></tr>\n", config.trk_alt);
	html->print(tempHtml);
	html->print("</table></td>");
	html->print("</tr>\n");

	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Smart Beacon:</b></td>\n");
	html->print("<td align=\"center\">\n");
	if (config.trk_smartbeacon)
		html->print("<fieldset id=\"smartbcnGrp\">\n");
	else
		html->print("<fieldset id=\"smartbcnGrp\" disabled>\n");
	html->print("<legend>Smart beacon configuration</legend>\n<table>");
	html->print("<tr>\n");
	html->print("<td align=\"right\">Move Symbol:</td>\n");
	strcpy(table, "1");
	if (config.trk_symmove[0] == 47)
		strcpy(table, "1");
	if (config.trk_symmove[0] == 92)
		strcpy(table, "2");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"moveTable\" name=\"moveTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"moveSymbol\" name=\"moveSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"moveImgSymbol\" onclick=\"openWindowSymbol(1);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%s.png\"> <i>*Click icon for select MOVE symbol</i></td>\n",
			 config.trk_symmove[0], config.trk_symmove[1], (int)config.trk_symmove[1], table);
	html->print(tempHtml);
	html->print("</tr>\n");
	html->print("<tr>\n");
	html->print("<td align=\"right\">Stop Symbol:</td>\n");
	strcpy(table, "1");
	if (config.trk_symstop[0] == 47)
		strcpy(table, "1");
	if (config.trk_symstop[0] == 92)
		strcpy(table, "2");
	snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Table:<input maxlength=\"1\" size=\"1\" id=\"stopTable\" name=\"stopTable\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> Symbol:<input maxlength=\"1\" size=\"1\" id=\"stopSymbol\" name=\"stopSymbol\" type=\"text\" value=\"%c\" style=\"background-color: rgb(97, 239, 170);\" /> <img border=\"1\" style=\"vertical-align: middle;\" id=\"stopImgSymbol\" onclick=\"openWindowSymbol(2);\" src=\"http://aprs.nakhonthai.net/symbols/icons/%d-%s.png\"> <i>*Click icon for select STOP symbol</i></td>\n",
			 config.trk_symstop[0], config.trk_symstop[1], (int)config.trk_symstop[1], table);
	html->print(tempHtml);
	html->print("</tr>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">High Speed:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"10\" max=\"1000\" step=\"1\" id=\"hspeed\" name=\"hspeed\" type=\"number\" value=\"%d\" /> km/h</td></tr>\n", config.trk_hspeed);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Low Speed:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"250\" step=\"1\" id=\"lspeed\" name=\"lspeed\" type=\"number\" value=\"%d\" /> km/h</td></tr>\n", config.trk_lspeed);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Slow Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"60\" max=\"3600\" step=\"1\" id=\"slowInterval\" name=\"slowInterval\" type=\"number\" value=\"%d\" /> Sec.</td></tr>\n", config.trk_slowinterval);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Max Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"10\" max=\"255\" step=\"1\" id=\"maxInterval\" name=\"maxInterval\" type=\"number\" value=\"%d\" /> Sec.</td></tr>\n", config.trk_maxinterval);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Min Interval:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"100\" step=\"1\" id=\"minInterval\" name=\"minInterval\" type=\"number\" value=\"%d\" /> Sec.</td></tr>\n", config.trk_mininterval);
	html->print(tempHtml);
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Min Angle:</td><td style=\"text-align: left;\"><input size=\"3\" min=\"1\" max=\"359\" step=\"1\" id=\"minAngle\" name=\"minAngle\" type=\"number\" value=\"%d\" /> Degree.</td></tr>\n", config.trk_minangle);
	html->print(tempHtml);

	html->print("</table></fieldset></tr>");

	html->print("<tr>\n");
	html->print("<td align=\"right\"><b>Telemetry:</b><br />(v=0->8280)</td>\n");
	html->print("<td align=\"center\"><table>\n");
	snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">Interval:</td><td style=\"text-align: left;\"><input min=\"0\" max=\"1000\" step=\"1\" id=\"trkTlmInv\" name=\"trkTlmInv\" type=\"number\" value=\"%d\" /> *Number of packets interval,<i>Example: 0 not send,1 send every packet</i></label></td></tr>", config.trk_tlm_interval);
	html->print(tempHtml);
	for (int ax = 0; ax < 5; ax++)
	{
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td align=\"right\"><b>CH A%d:</b></td>\n", ax + 1);
		html->print(tempHtml);
		html->print("<td align=\"center\">\n");
		html->print("<table>");

		html->print("<tr><td style=\"text-align: right;\">Sensor:</td>\n");
		html->print("<td style=\"text-align: left;\">CH: ");
		snprintf(tempHtml, sizeof(tempHtml), "<select name=\"sensorCH%d\" id=\"sensorCH%d\">\n", ax, ax);
		html->print(tempHtml);
		for (uint8_t idx = 0; idx < 11; idx++)
		{
			if (idx == 0)
			{
				if (config.trk_tlm_sensor[ax] == idx)
				{
					snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>NONE</option>\n", idx);
				}
				else
				{
					snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">NONE</option>\n", idx);
				}
			}
			else
			{
				if (config.trk_tlm_sensor[ax] == idx)
				{
					snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>SENSOR#%d</option>\n", idx, idx);
				}
				else
				{
					snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\">SENSOR#%d</option>\n", idx, idx);
				}
			}
			html->print(tempHtml);
		}
		html->print("</select></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Name: <input maxlength=\"10\" size=\"8\" name=\"param%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.trk_tlm_PARM[ax]);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Unit: <input maxlength=\"8\" size=\"5\" name=\"unit%d\" type=\"text\" value=\"%s\" /></td>\n", ax, config.trk_tlm_UNIT[ax]);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Precision: <input min=\"0\" max=\"5\" step=\"1\" type=\"number\" style=\"width: 2em\" name=\"precision%d\" type=\"text\" value=\"%d\" onchange=\"selPrecision(%d)\" /></td></tr>\n", ax, config.trk_tlm_precision[ax], ax);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<tr><td style=\"text-align: right;\">EQNS:</td><td colspan=\"3\" style=\"text-align: left;\">a:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%da\" type=\"number\" value=\"%.5f\" />  b:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%db\" type=\"number\" value=\"%.5f\" /> c:<input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" name=\"eqns%dc\" type=\"number\" value=\"%.5f\" /> (av<sup>2</sup>+bv+c) </td>\n",
				 ax, config.trk_tlm_EQNS[ax][0], ax, config.trk_tlm_EQNS[ax][1], ax, config.trk_tlm_EQNS[ax][2]);
		html->print(tempHtml);
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\">Offset: <input min=\"-9999\" max=\"9999\" step=\"0.00001\" style=\"width: 5em\" type=\"number\" name=\"offset%d\" type=\"text\" value=\"%.5f\"  onchange=\"selOffset(%d)\" /></td></tr>\n", ax, config.trk_tlm_offset[ax], ax);
		html->print(tempHtml);
		html->print("</table></td>");
		html->print("</tr>\n");
	}
	html->print("</table></td></tr>\n");
	html->print("<tr><td colspan=\"2\" align=\"right\">\n");
	html->print("<div><button class=\"button\" type='submit' id='submitTRACKER'  name=\"commitTRACKER\"> Apply Change </button></div>\n");
	html->print("<input type=\"hidden\" name=\"commitTRACKER\"/>\n");
	html->print("</td></tr></table><br />\n");
	html->print("</form><br />");

	html->addHeader("Tracker", "content");
	html->addHeader("Cache-Control", "no-cache");
	request->send(html);
}

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
		// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
		char tempHtml[256] = "";
		AsyncResponseStream *html = request->beginResponseStream("text/html", 20000);
		if (!html)
		{
			request->send(500, "text/html", "Memory allocation failed");
			return;
		}
		html->print("<script type=\"text/javascript\">\n");
		html->print("$('form').submit(function (e) {\n");
		html->print("e.preventDefault();\n");
		html->print("var data = new FormData(e.currentTarget);\n");
		html->print("if(e.currentTarget.id===\"formBluetooth\") document.getElementById(\"submitBluetooth\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formWiFiAP\") document.getElementById(\"submitWiFiAP\").disabled=true;\n");
		html->print("if(e.currentTarget.id===\"formWiFiClient\") document.getElementById(\"submitWiFiClient\").disabled=true;\n");
		html->print("$.ajax({\n");
		html->print("url: '/wireless',\n");
		html->print("type: 'POST',\n");
		html->print("data: data,\n");
		html->print("contentType: false,\n");
		html->print("processData: false,\n");
		html->print("success: function (data) {\n");
		html->print("alert(\"Submited Successfully\");\n");
		html->print("},\n");
		html->print("error: function (data) {\n");
		html->print("alert(\"An error occurred.\");\n");
		html->print("}\n");
		html->print("});\n");
		html->print("});\n");
		html->print("</script>\n");
		
		/************************ WiFi AP **************************/
		html->print("<form id='formWiFiAP' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>WiFi Access Point</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\" width=\"120\"><b>Enable:</b></td>\n");
		const char *wifiAPEnFlagMode = (config.wifi_mode & WIFI_AP_FIX) ? "checked" : "";
		{
			char *temp_flag = allocateStringMemory(512);
			if (temp_flag)
			{
				snprintf(temp_flag, 512, "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wifiAP\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", wifiAPEnFlagMode);
				html->print(temp_flag);
				free(temp_flag);
			}
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>WiFi AP SSID:</b></td>\n");
		{
			char *temp_ssid = allocateStringMemory(512);
			if (temp_ssid)
			{
				snprintf(temp_ssid, 512, "<td style=\"text-align: left;\"><input size=\"32\" maxlength=\"32\" class=\"form-control\" id=\"wifi_ssidAP\" name=\"wifi_ssidAP\" type=\"text\" value=\"%s\" /></td>\n", config.wifi_ap_ssid);
				html->print(temp_ssid);
				free(temp_ssid);
			}
		}
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>WiFi AP PASSWORD:</b></td>\n");
		{
			char *temp_pass = allocateStringMemory(512);
			if (temp_pass)
			{
				snprintf(temp_pass, 512, "<td style=\"text-align: left;\"><input size=\"63\" maxlength=\"63\" class=\"form-control\" id=\"wifi_passAP\" name=\"wifi_passAP\" type=\"password\" value=\"%s\" /></td>\n", config.wifi_ap_pass);
				html->print(temp_pass);
				free(temp_pass);
			}
		}
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitWiFiAP'  name=\"commitWiFiAP\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitWiFiAP\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");
		/************************ WiFi Client **************************/
		html->print("<br />\n");
		html->print("<form id='formWiFiClient' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		html->print("<table>\n");
		html->print("<th colspan=\"2\"><span><b>WiFi Multi Station</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>WiFi STA Enable:</b></td>\n");
		String wifiClientEnFlag = "";
		if (config.wifi_mode & WIFI_STA_FIX)
			wifiClientEnFlag = "checked";
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wificlient\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", wifiClientEnFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>WiFi RF Power:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"wifi_pwr\" id=\"wifi_pwr\">\n");
		for (int i = 0; i < 12; i++)
		{
			if (config.wifi_power == (int8_t)wifiPwr[i][0])
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" selected>%.1f dBm</option>\n", (int8_t)wifiPwr[i][0], wifiPwr[i][1]);
			else
				snprintf(tempHtml, sizeof(tempHtml), "<option value=\"%d\" >%.1f dBm</option>\n", (int8_t)wifiPwr[i][0], wifiPwr[i][1]);
			html->print(tempHtml);
		}
		html->print("</select>\n");
		html->print("</td>\n");
		html->print("</tr>\n");
		for (int n = 0; n < 5; n++)
		{
			html->print("<tr>\n");
			snprintf(tempHtml, sizeof(tempHtml), "<td align=\"right\"><b>Station #%d:</b></td>\n", n + 1);
			html->print(tempHtml);
			html->print("<td align=\"center\">\n");
			snprintf(tempHtml, sizeof(tempHtml), "<fieldset id=\"filterDispGrp%d\">\n", n + 1);
			html->print(tempHtml);
			snprintf(tempHtml, sizeof(tempHtml), "<legend>WiFi Station #%d</legend>\n<table style=\"text-align:unset;border-width:0px;background:unset\">", n + 1);
			html->print(tempHtml);
			html->print("<tr style=\"background:unset;\">");
			// html->print("<tr>\n";
			html->print("<td align=\"right\" width=\"120\"><b>Enable:</b></td>\n");
			char wifiClientEnFlag[10];
			if (config.wifi_sta[n].enable)
				strcpy(wifiClientEnFlag, "checked");
			else
				strcpy(wifiClientEnFlag, "");

			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"wifiStation%d\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", n, wifiClientEnFlag);
			html->print(tempHtml);
			html->print("</tr>\n");
			html->print("<tr>\n");
			html->print("<td align=\"right\"><b>WiFi SSID:</b></td>\n");
			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input size=\"32\" maxlength=\"32\" name=\"wifi_ssid%d\" type=\"text\" value=\"%s\" /></td>\n", n, config.wifi_sta[n].wifi_ssid);
			html->print(tempHtml);
			html->print("</tr>\n");
			html->print("<tr>\n");
			html->print("<td align=\"right\"><b>WiFi PASSWORD:</b></td>\n");
			snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input size=\"63\" maxlength=\"63\" name=\"wifi_pass%d\" type=\"password\" value=\"%s\" /></td>\n", n, config.wifi_sta[n].wifi_pass);
			html->print(tempHtml);
			html->print("</tr>\n");
			html->print("</tr></table></fieldset>\n");
			html->print("</td></tr>\n");
		}
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitWiFiClient'  name=\"commitWiFiClient\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitWiFiClient\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form><br />");
		/************************ Bluetooth **************************/
#ifdef BLUETOOTH
		html->print("<br />\n");
		html->print("<form id='formBluetooth' method=\"POST\" action='#' enctype='multipart/form-data'>\n");
		// html->print("<h2>Bluetooth Master (BLE)</h2>\n";
		html->print("<table>\n");
		// html->print("<tr>\n";
		// strcat(html, "<th width=\"200\"><span><b>Setting</b></span></th>\n";
		// strcat(html, "<th><span><b>Value</b></span></th>\n";
		// strcat(html, "</tr>\n";
		html->print("<th colspan=\"2\"><span><b>Bluetooth Master (BLE)</b></span></th>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>Enable:</b></td>\n");

		char btFlag[10];
		if (config.bt_master)
			strcpy(btFlag, "checked");
		else
			strcpy(btFlag, "");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><label class=\"switch\"><input type=\"checkbox\" name=\"btMaster\" value=\"OK\" %s><span class=\"slider round\"></span></label></td>\n", btFlag);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>NAME:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"20\" id=\"bt_name\" name=\"bt_name\" type=\"text\" value=\"%s\" /></td>\n", config.bt_name);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>PIN:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input min=\"0\" max=\"999999\" id=\"bt_pin\" name=\"bt_pin\" type=\"number\" value=\"%d\" /></td> <i>*Value 0 is no auth.</i>\n", config.bt_pin);
		html->print(tempHtml);
		html->print("</tr>\n");
		#if !defined(CONFIG_IDF_TARGET_ESP32)
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UUID:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid\" name=\"bt_uuid\" type=\"text\" value=\"%s\" /></td>\n", config.bt_uuid);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UUID RX:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid_rx\" name=\"bt_uuid_rx\" type=\"text\" value=\"%s\" /></td>\n", config.bt_uuid_rx);
		html->print(tempHtml);
		html->print("</tr>\n");
		html->print("<tr>\n");
		html->print("<td align=\"right\"><b>UUID TX:</b></td>\n");
		snprintf(tempHtml, sizeof(tempHtml), "<td style=\"text-align: left;\"><input maxlength=\"37\" size=\"38\" id=\"bt_uuid_tx\" name=\"bt_uuid_tx\" type=\"text\" value=\"%s\" /></td>\n", config.bt_uuid_tx);
		html->print(tempHtml);
		html->print("</tr>\n");
		#endif
		html->print("<td align=\"right\"><b>MODE:</b></td>\n");
		html->print("<td style=\"text-align: left;\">\n");
		html->print("<select name=\"bt_mode\" id=\"bt_mode\">\n");
		const char *btModeOff = (config.bt_mode == 0) ? "selected" : "";
		const char *btModeTNC2 = (config.bt_mode == 1) ? "selected" : "";
		const char *btModeKISS = (config.bt_mode == 2) ? "selected" : "";
		snprintf(tempHtml, sizeof(tempHtml), "<option value=\"0\" %s>NONE</option>\n<option value=\"1\" %s>TNC2</option>\n<option value=\"2\" %s>KISS</option>\n", btModeOff, btModeTNC2, btModeKISS);
		html->print(tempHtml);
		html->print("</select>\n");

		html->print("<label style=\"font-size: 8pt;text-align: right;\">*See the following for generating UUIDs: <a href=\"https://www.uuidgenerator.net\" target=\"_blank\">https://www.uuidgenerator.net</a></label></td>\n");
		html->print("</tr>\n");
		html->print("<tr><td colspan=\"2\" align=\"right\">\n");
		html->print("<div><button class=\"button\" type='submit' id='submitBluetooth'  name=\"commitBluetooth\"> Apply Change </button></div>\n");
		html->print("<input type=\"hidden\" name=\"commitBluetooth\"/>\n");
		html->print("</td></tr></table><br />\n");
		html->print("</form>");
#endif

	html->addHeader("wifi", "content");
	html->addHeader("Cache-Control", "no-cache");
	request->send(html);

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

void handle_about(AsyncWebServerRequest *request)
{
	if (!request->authenticate(config.http_username, config.http_password))
	{
		return request->requestAuthentication();
	}
	char strCID[50];
	uint64_t chipid = ESP.getEfuseMac();
	sprintf(strCID, "%04X%08X", (uint16_t)(chipid >> 32), (uint32_t)chipid);

	// AsyncResponseStream grows its internal cbuf incrementally (small chunks)
	AsyncResponseStream *webString = request->beginResponseStream("text/html", 25000);
	if (!webString)
	{
		request->send(500, "text/html", "Memory allocation failed");
		return;
	}
	webString->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;\"><td width=\"49%\" style=\"border:unset;\">");

	webString->print("<table style=\"height:100%;\">");
	webString->print("<th colspan=\"2\"><span><b>System Information</b></span></th>\n");
	// webString->print("<tr><th width=\"200\"><span><b>Name</b></span></th><th><span><b>Information</b></span></th></tr>";
	webString->print("<tr><td align=\"right\"><b>Hardware Version: </b></td><td align=\"left\">");
	
	char FirmwareOTA[50];
	// Build a dot-free version string for use in firmware filenames
	char verNoDot[20];
	{
		const char *src = VERSION;
		char *dst = verNoDot;
		while (*src) { if (*src != '.') *dst++ = *src; src++; }
		*dst = '\0';
	}
	char ver[20];
	sprintf(ver, "v%s-%s.bin", VERSION, VERSION_BUILD);
#if defined(TTGO_TWR)	
	webString->print("LiLyGo T-TWRPlus");
	sprintf(FirmwareOTA, "ESP32S3_TWR_%s", ver);
#elif defined(CONFIG_IDF_TARGET_ESP32)	
	#ifdef SH1106
		webString->print("ESP32-WROOM+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32_SH1106_%s", ver);
	#elif defined(SSD1306)
		webString->print("ESP32-WROOM+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		webString->print("ESP32-WROOM NO OTA,ESP32 DoIt DevKit");
		sprintf(FirmwareOTA, "ESP32_NOOTA_%s", ver);
	#else
		webString->print("ESP32-WROOM,ESP32 DoIt DevKit");
		sprintf(FirmwareOTA, "ESP32_NODISP_%s", ver);	
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
	#ifdef SH1106
		webString->print("ESP32C3+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32C3_SH1106_%s", ver);
	#elif defined(SSD1306)
		webString->print("ESP32C3+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32C3_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		webString->print("ESP32C3 NO OTA,ESP32-C3 DIY");
		sprintf(FirmwareOTA, "ESP32C3_NOOTA_%s", ver);
	#else	
		webString->print("ESP32C3,ESP32-C3 DIY");
		sprintf(FirmwareOTA, "ESP32C3_NODISP_%s", ver);
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
	#ifdef SH1106
		webString->print("ESP32C6+SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32C6_SH1106_%s", ver);
	#elif defined(SSD1306)	
		webString->print("ESP32C6+SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32C6_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		webString->print("ESP32C6_NOOTA,ESP32-C6 DIY");
		sprintf(FirmwareOTA, "ESP32C6_NOOTA_%s", ver);
	#else
		webString->print("ESP32C6,ESP32-C6 DIY");
		sprintf(FirmwareOTA, "ESP32C6_%s", ver);
	#endif
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
	#ifdef ESP32S3_N16R8
		webString->print("ESP32-S3_16MB,ESP32-S3-DevKit");
		sprintf(FirmwareOTA, "ESP32S3_N16R8_%s", ver);
	#elif defined(SH1106)
		webString->print("ESP32-S3_8MB,SH1106 OLED");
		sprintf(FirmwareOTA, "ESP32S3_SH1106_%s", ver);	
	#elif defined(SSD1306)
		webString->print("ESP32-S3_8MB,SSD1306 OLED");
		sprintf(FirmwareOTA, "ESP32S3_SSD1306_%s", ver);
	#elif defined(NO_OTA)
		webString->print("ESP32-S3 Super mini,No OTA");
		sprintf(FirmwareOTA, "ESP32S3_NOOTA_%s", ver);
	#endif
#else
	webString->print("UNKNOWN,ESP32 DIY");
#endif
	webString->print("</td></tr>");

	char *temp_str = allocateStringMemory(512);
	if (temp_str)
	{
		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>Firmware Version: </b></td><td align=\"left\"> V%s%s</td></tr>\n", VERSION, VERSION_BUILD);
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>RF Module: </b></td><td align=\"left\"> %s</td></tr>\n", RF_TYPE[config.rf_type]);
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>ESP32 Model: </b></td><td align=\"left\"> %s</td></tr>", ESP.getChipModel());
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>Revision: </b></td><td align=\"left\"> %d</td></tr>", ESP.getChipRevision());
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>Chip ID: </b></td><td align=\"left\"> %s</td></tr>", strCID);
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>Flash: </b></td><td align=\"left\">%d KByte</td></tr>", ESP.getFlashChipSize() / 1024);
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>PSRAM: </b></td><td align=\"left\">%.1f/%.1f KByte</td></tr>",
				 (float)ESP.getFreePsram() / 1024, (float)ESP.getPsramSize() / 1024);
		webString->print(temp_str);

		snprintf(temp_str, 512, "<tr><td align=\"right\"><b>FILE SYSTEM: </b></td><td align=\"left\">%.1f/%.1f KByte</td></tr>",
				 (float)LITTLEFS.usedBytes() / 1024, (float)LITTLEFS.totalBytes() / 1024);
		webString->print(temp_str);

		free(temp_str);
	}

	webString->print("</table>");
	webString->print("</td><td width=\"2%\" style=\"border:unset;\"></td>");
	webString->print("<td width=\"49%\" style=\"border:unset;\">");

	webString->print("<table style=\"height:100%;\">");
    webString->print("<th colspan=\"2\"><span><b>Developer/Support Information</b></span></th>\n");
    webString->print("<tr><td align=\"right\"><b>Author: </b></td><td align=\"left\">Marcelo Ferreira (fork/mods)</td></tr>");
    webString->print("<tr><td align=\"right\"><b>Callsign: </b></td><td align=\"left\">LU6JMF-10</td></tr>");
    webString->print("<tr><td align=\"right\"><b>Country: </b></td><td align=\"left\">Concepcion del Uruguay, Entre Rios, Argentina</td></tr>\n");
    webString->print("<tr><td align=\"right\"><b>Github: </b></td><td align=\"left\"><a href=\"https://github.com/marceloferreirachile/ESP32APRS_Audio\" target=\"_blank\">github.com/marceloferreirachile/ESP32APRS_Audio</a></td></tr>");
    webString->print("<tr><td align=\"right\"><b>Original: </b></td><td align=\"left\">Mr.Somkiat Nakhonthai (HS5TQA) - <a href=\"https://github.com/nakhonthai\" target=\"_blank\">github.com/nakhonthai</a></td></tr>\n");
    webString->print("<tr><td align=\"right\"><b>WhatsApp: </b></td><td align=\"left\"><a href=\"https://wa.me/5493442311119\" target=\"_blank\">wa.me/5493442311119</a></td></tr>");
    webString->print("<tr><td align=\"right\"><b>Donate: </b></td><td align=\"left\"><a href=\"https://www.paypal.me/MarceloFerreira673\" target=\"_blank\">paypal.me/MarceloFerreira673</a></td></tr>");

	webString->print("</table>");
	webString->print("</td></tr></table><br />");

	webString->print("<table>\n");
	webString->print("<tr><td align=\"right\"><b>Contributor: </b></td><td align=\"left\">LU6JMF Marcelo</td></tr>\n");
	webString->print("<tr><td align=\"right\"><b>Version: </b></td><td align=\"left\">v2.0-lu6jmf (custom mods on top of V1.8a)</td></tr>\n");
	webString->print("<tr><td align=\"right\" style=\"vertical-align:top;\"><b>Changes: </b></td><td align=\"left\" style=\"white-space:normal;\">\n");
	webString->print("- Fixed CPU temperature display in web interface (sensor was reinitialized on every dashboard refresh)<br />\n");
	webString->print("- Added recurring Bulletins BLN1-BLN9 (MSG tab): auto-repeat with per-bulletin Interval and optional send-count Limit (0=unlimited)<br />\n");
	webString->print("- Bulletins never retry (nobody ACKs a BLN), unlike normal messages<br />\n");
	webString->print("- Dashboard LAST HEARD: Callsign links to QRZ.com, plus a small map icon linking to aprs.fi (both open in a new tab)<br />\n");
	webString->print("- Dashboard LAST HEARD icons now work without internet: loads from the internet first, falls back to a local copy already included on the device, then to a simple drawn icon (never blank)<br />\n");
	webString->print("- Dashboard icons now match the aprs.fi style (previously used a different icon set)<br />\n");
	webString->print("- New: TCP KISS Server (MOD tab) lets PC software (Xastir, APRSIS32, etc) use this device as a network TNC over WiFi, 2 ports, no cable needed. Off by default<br />\n");
webString->print("- Fixed a buffer overflow risk in the IGATE/DIGI PHG Text field (unbounded strcpy on an 8-byte buffer)<br />\n");
webString->print("- Simplified OTA Online Firmware Update: now only checks and links to the new version, no more auto-download/auto-flash. Use Manual Firmware Update to install<br />\n");
webString->print("- Manual Filesystem Update now auto-restores your WiFi/APRS config after writing icons, so it survives even without USB access<br />\n");
webString->print("- Fixed a bug where submitting Manual Filesystem Update also fired a stray request from Manual Firmware Update, breaking the icons write<br />\n");
	webString->print("</td></tr>\n");
	webString->print("</table><br />\n");

	webString->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;\"><td width=\"49%\" style=\"border:unset;\">");

	webString->print("<table>\n");
	webString->print("<th colspan=\"2\"><span><b>WiFi Status</b></span></th>\n");
	webString->print("<tr><td align=\"right\"><b>Mode:</b></td>\n");
	webString->print("<td align=\"left\">");
	if (config.wifi_mode == WIFI_AP_FIX)
	{
		webString->print("AP");
	}
	else if (config.wifi_mode == WIFI_STA_FIX)
	{
		webString->print("STA");
	}
	else if (config.wifi_mode == WIFI_AP_STA_FIX)
	{
		webString->print("AP+STA");
	}
	else
	{
		webString->print("OFF");
	}
	uint8_t proto = 0;
	esp_wifi_get_protocol(WIFI_IF_STA, &proto);
	webString->print(" (802.11");
	if (proto & WIFI_PROTOCOL_11B)
		webString->print("b");
	if (proto & WIFI_PROTOCOL_11G)
		webString->print("g");
	if (proto & WIFI_PROTOCOL_11N)
		webString->print("n");
	if (proto & WIFI_PROTOCOL_LR)
		webString->print("lr");
	webString->print(")");

	wifi_power_t wpr = WiFi.getTxPower();
	char wifipower[20] = "";
	if (wpr < 8)
	{
		strcpy(wifipower, "-1 dBm");
	}
	else if (wpr < 21)
	{
		strcpy(wifipower, "2 dBm");
	}
	else if (wpr < 29)
	{
		strcpy(wifipower, "5 dBm");
	}
	else if (wpr < 35)
	{
		strcpy(wifipower, "8.5 dBm");
	}
	else if (wpr < 45)
	{
		strcpy(wifipower, "11 dBm");
	}
	else if (wpr < 53)
	{
		strcpy(wifipower, "13 dBm");
	}
	else if (wpr < 61)
	{
		strcpy(wifipower, "15 dBm");
	}
	else if (wpr < 69)
	{
		strcpy(wifipower, "17 dBm");
	}
	else if (wpr < 75)
	{
		strcpy(wifipower, "18.5 dBm");
	}
	else if (wpr < 77)
	{
		strcpy(wifipower, "19 dBm");
	}
	else if (wpr < 80)
	{
		strcpy(wifipower, "19.5 dBm");
	}
	else
	{
		strcpy(wifipower, "20 dBm");
	}

	webString->print("</td></tr>\n");

	char *temp_str1 = allocateStringMemory(512);
	if (temp_str1)
	{
		snprintf(temp_str1, 512, "<tr><td align=\"right\" width=\"30%%\"><b>MAC:</b></td>\n<td align=\"left\">%s</td></tr>\n", WiFi.macAddress().c_str());
		webString->print(temp_str1);
		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>Channel:</b></td>\n<td align=\"left\">%d</td></tr>\n", WiFi.channel());
		webString->print(temp_str1);

		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>TX Power:</b></td>\n<td align=\"left\">%s</td></tr>\n", wifipower);
		webString->print(temp_str1);

		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>SSID:</b></td>\n<td align=\"left\">%s</td></tr>\n", WiFi.SSID().c_str());
		webString->print(temp_str1);

		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>Local IP:</b></td>\n<td align=\"left\">%s</td></tr>\n", WiFi.localIP().toString().c_str());
		webString->print(temp_str1);

		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>Gateway IP:</b></td>\n<td align=\"left\">%s</td></tr>\n", WiFi.gatewayIP().toString().c_str());
		webString->print(temp_str1);
		snprintf(temp_str1, 512, "<tr><td align=\"right\"><b>DNS:</b></td>\n<td align=\"left\">%s</td></tr>\n", WiFi.dnsIP().toString().c_str());
		webString->print(temp_str1);

		free(temp_str1);
	}

	webString->print("</table>\n");

	webString->print("</td><td width=\"2%\" style=\"border:unset;\"></td>");
	webString->print("<td width=\"49%\" style=\"border:unset;\">");
	webString->print("<table>\n");
#ifdef PPPOS
	webString->print("<th colspan=\"2\"><span><b>PPPoS Status</b></span></th>\n");

	char *temp_str2 = allocateStringMemory(512);
	if (temp_str2)
	{
		snprintf(temp_str2, 512, "<tr><td align=\"right\" width=\"30%%\"><b>Manufacturer:</b></td>\n<td align=\"left\">%s</td></tr>\n", pppStatus.manufacturer);
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>Model:</b></td>\n<td align=\"left\">%s</td></tr>\n", pppStatus.model);
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>IMEI:</b></td>\n<td align=\"left\">%s</td></tr>\n", pppStatus.imei);
		webString->print(temp_str2);
		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>IMSI:</b></td>\n<td align=\"left\">%s</td></tr>\n", pppStatus.imsi);
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>Operator:</b></td>\n<td align=\"left\">%s</td></tr>\n", pppStatus.oper);
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>RSSI:</b></td>\n<td align=\"left\">%d dBm</td></tr>\n", pppStatus.rssi);
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>IP:</b></td>\n<td align=\"left\">%s</td></tr>\n", IPAddress(pppStatus.ip).toString().c_str());
		webString->print(temp_str2);

		snprintf(temp_str2, 512, "<tr><td align=\"right\"><b>Gateway:</b></td>\n<td align=\"left\">%s</td></tr>\n", IPAddress(pppStatus.gateway).toString().c_str());
		webString->print(temp_str2);
		free(temp_str2);
	}

// webString->print("<tr><td align=\"right\"><b>DNS:</b></td>\n");
// webString->print("<td align=\"left\">%s</td></tr>\n", IPAddress(pppStatus.dns).toString().c_str());
#endif
	webString->print("</table>\n");
	webString->print("</td></tr></table><br />");

	// webString->print("<table style=\"text-align:unset;border-width:0px;background:unset\"><tr style=\"background:unset;\"><td width=\"96%\" style=\"border:unset;\">");
#ifndef NO_OTA
	webString->print("<form method='POST' action='#' enctype='multipart/form-data' id='upload_form' class=\"form-horizontal\">\n");
	webString->print("<table>");
	webString->print("<th colspan=\"2\"><span><b>Manual Firmware Update</b></span></th>\n");
	webString->print("<tr><td align=\"right\"><b>File:</b></td><td align=\"left\"><input id=\"file\" name=\"update\" type=\"file\" onchange='sub(this)' /></td></tr>\n");
	webString->print("<tr><td align=\"right\"><b>Progress:</b></td><td><div id='prgbar'><div id='bar' style=\"width: 0px;\"><label id='prg'></label></div></div></td></tr>\n");
	webString->print("<tr><td align=\"right\"><b>Support Firmware:</b></td><td align=\"left\"><a target=\"_download\" href=\"https://github.com/marceloferreirachile/ESP32APRS_Audio/releases\">https://github.com/marceloferreirachile/ESP32APRS_Audio/releases</a></td></tr>\n");
	
	webString->print("<tr><td colspan=\"2\" align=\"right\"><div class=\"col-sm-3 col-xs-4\"><input type='submit' class=\"btn btn-danger\" id=\"update_sumbit\" value='Firmware UpLoad'></div></td></tr>\n");
	webString->print("</table><br />\n");
	webString->print("</form>\n");
	// webString->print("</td></tr></table><br />");

	webString->print("<script>"
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
					  "</script>");

	webString->print("<form method='POST' action='#' enctype='multipart/form-data' id='updatefs_form' class=\"form-horizontal\">\n");
	webString->print("<table>");
	webString->print("<th colspan=\"2\"><span><b>Manual Filesystem Update (icons/data)</b></span></th>\n");
	webString->print("<tr><td align=\"right\"><b>File:</b></td><td align=\"left\"><input id=\"filefs\" name=\"updatefs\" type=\"file\" /></td></tr>\n");
	webString->print("<tr><td align=\"right\"><b>Progress:</b></td><td><div id='prgbarfs'><div id='barfs' style=\"width: 0px;\"><label id='prgfs'></label></div></div></td></tr>\n");
	webString->print("<tr><td colspan=\"2\" align=\"right\"><div class=\"col-sm-3 col-xs-4\"><input type='submit' class=\"btn btn-danger\" id=\"updatefs_sumbit\" value='Filesystem UpLoad'></div></td></tr>\n");
	webString->print("</table><br />\n");
	webString->print("</form>\n");

	webString->print("<script>"
					  "$('#updatefs_form').submit(function(e){"
					  "e.preventDefault();"
					  "var form = $('#updatefs_form')[0];"
					  "var data = new FormData(form);"
					  "document.getElementById('updatefs_sumbit').disabled = true;"
					  "$.ajax({"
					  "url: '/updatefs',"
					  "type: 'POST',"
					  "data: data,"
					  "contentType: false,"
					  "processData:false,"
					  "xhr: function() {"
					  "var xhr = new window.XMLHttpRequest();"
					  "xhr.upload.addEventListener('progress', function(evt) {"
					  "if (evt.lengthComputable) {"
					  "var per = evt.loaded / evt.total;"
					  "$('#prgfs').html(Math.round(per*100) + '%');"
					  "$('#barfs').css('width',Math.round(per*100) + '%');"
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
					  "</script>");

	
	{
		char ota_url_buf[200];
		snprintf(ota_url_buf, sizeof(ota_url_buf), "https://github.com/marceloferreirachile/ESP32APRS_Audio/releases/download/v%s-%s/%s", VERSION, VERSION_BUILD, FirmwareOTA);

		webString->print("<table>");
		webString->print("<th colspan=\"2\"><span><b>OTA Online Firmware Update</b></span></th>\n");
		char ota_row[512];
		snprintf(ota_row, sizeof(ota_row),
			"<tr><td align=\"right\"><b>Firmware File:</b></td><td align=\"left\"><a id=\"fw_file_link\" href=\"%s\" target=\"_blank\">%s</a></td></tr>\n",
			ota_url_buf, FirmwareOTA);
		webString->print(ota_row);
		snprintf(ota_row, sizeof(ota_row),
			"<tr><td align=\"right\"><b>Current Version:</b></td><td align=\"left\">V%s%s</td></tr>\n",
			VERSION, VERSION_BUILD);
		webString->print(ota_row);
		webString->print("<tr><td align=\"right\"><b>Status:</b></td><td><span id='ota_prg'>Ready</span></td></tr>\n");
		webString->print("<tr><td colspan=\"2\" align=\"right\"><div class=\"col-sm-3 col-xs-4\"><input type='button' class=\"btn btn-danger\" id=\"check_ver_btn\" value='Check New Version'></div></td></tr>\n");
		webString->print("</table><br />\n");
	}

	webString->print("<script>"
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
					  "</script>");
#endif
	webString->print("</body></html>\n");

	webString->addHeader("About", "content");
	webString->addHeader("Cache-Control", "no-cache");
	request->send(webString);
}

void handle_gnss(AsyncWebServerRequest *request)
{
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

void handleUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
        // Open the file in write mode on the first chunk
        request->_tempFile = LITTLEFS.open("/" + filename, "w");
    }
    if (len < (LITTLEFS.totalBytes()-LITTLEFS.usedBytes())) {
        // Write the data chunk to the file
        request->_tempFile.write(data, len);
    }else{
		// Not enough space to write the file
		request->_tempFile.close();
		LITTLEFS.remove("/" + filename); // Remove the incomplete file
		request->send(500, "text/plain", "Not enough space to upload the file");
		return;
	}
    if (final) {
        // Close the file on the last chunk and redirect
        request->_tempFile.close();
        request->redirect("/"); // Redirect back to the main page
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
	async_server.on("/vpn", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_vpn(request); });
#ifdef MQTT
	async_server.on("/mqtt", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mqtt(request); });
#endif
	async_server.on("/msg", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_msg(request); });
	async_server.on("/mod", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mod(request); });
	async_server.on("/mod2", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_mod2(request); });
	async_server.on("/default", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_default(); });
	async_server.on("/igate", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_igate(request); });
	async_server.on("/digi", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_digi(request); });
	async_server.on("/tracker", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_tracker(request); });
	async_server.on("/wx", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_wx(request); });
	async_server.on("/tlm", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_tlm(request); });
	async_server.on("/sensor", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request)
					{ handle_sensor(request); });
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
	async_server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *request)
					{ handle_dashboard(request); });
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
        request->send(200, "text/plain", "File uploaded successfully!");
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
