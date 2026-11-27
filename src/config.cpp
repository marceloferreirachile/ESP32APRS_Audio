/*
 Name:		ESP32APRS
 Created:	25-12-2024
 Author:	HS5TQA/Atten
 Github:	https://github.com/nakhonthai
 Facebook:	https://www.facebook.com/atten
*/

#include "config.h"
#include <LITTLEFS.h>
#include <SPIFFS.h>
#include "FS.h"
#include <ArduinoJson.h>
#include <stddef.h> // offsetof
//#include <StreamUtils.h>
#include "AFSK.h"
#include <esp_task_wdt.h>

extern fs::LITTLEFSFS LITTLEFS;

extern TaskHandle_t taskAPRSHandle;
extern TaskHandle_t taskAPRSPollHandle;

//JsonDocument doc;

extern volatile int8_t adcEn;
extern volatile int8_t dacEn;

// Saves the configuration to a file
// LU6JMF 2.1.9: the ~160 plain "one JSON key <-> one config field" lines of save/load are a
// table now (same keys, same values, same types and defaults - the JSON file does not change;
// only the order of the keys inside it). Arrays, old-version conversions and fields with special
// handling stay written out below, exactly as before. Saves flash.
enum : uint8_t { CF_B, CF_U8, CF_I8, CF_U16, CF_I16, CF_INT, CF_UINT, CF_LONG, CF_ULONG, CF_I32, CF_U32, CF_F, CF_D, CF_S };
struct CfgField { const char *key; uint8_t type; uint16_t off; uint16_t size; const char *dflt; };
static const CfgField CFG_FIELDS[] = {
    {"syncTime", CF_B, offsetof(Configuration, synctime), sizeof(((Configuration*)0)->synctime), nullptr},
    {"timeZone", CF_F, offsetof(Configuration, timeZone), sizeof(((Configuration*)0)->timeZone), nullptr},
    {"WiFiPwr", CF_I8, offsetof(Configuration, wifi_power), sizeof(((Configuration*)0)->wifi_power), nullptr},
    {"WiFiAPCH", CF_U8, offsetof(Configuration, wifi_ap_ch), sizeof(((Configuration*)0)->wifi_ap_ch), nullptr},
    {"fx25Mode", CF_U8, offsetof(Configuration, fx25_mode), sizeof(((Configuration*)0)->fx25_mode), nullptr},
    {"rfEnable", CF_B, offsetof(Configuration, rf_en), sizeof(((Configuration*)0)->rf_en), nullptr},
    {"rfType", CF_U8, offsetof(Configuration, rf_type), sizeof(((Configuration*)0)->rf_type), nullptr},
    {"rfModem", CF_U8, offsetof(Configuration, modem_type), sizeof(((Configuration*)0)->modem_type), nullptr},
    {"rfPreamble", CF_U8, offsetof(Configuration, preamble), sizeof(((Configuration*)0)->preamble), nullptr},
    {"rfFreqRX", CF_F, offsetof(Configuration, freq_rx), sizeof(((Configuration*)0)->freq_rx), nullptr},
    {"rfFreqTX", CF_F, offsetof(Configuration, freq_tx), sizeof(((Configuration*)0)->freq_tx), nullptr},
    {"rfToneRX", CF_INT, offsetof(Configuration, tone_rx), sizeof(((Configuration*)0)->tone_rx), nullptr},
    {"rfToneTX", CF_INT, offsetof(Configuration, tone_tx), sizeof(((Configuration*)0)->tone_tx), nullptr},
    {"rfSql", CF_U8, offsetof(Configuration, sql_level), sizeof(((Configuration*)0)->sql_level), nullptr},
    {"rfVolume", CF_U8, offsetof(Configuration, volume), sizeof(((Configuration*)0)->volume), nullptr},
    {"rfBand", CF_U8, offsetof(Configuration, band), sizeof(((Configuration*)0)->band), nullptr},
    {"audioLPF", CF_B, offsetof(Configuration, audio_lpf), sizeof(((Configuration*)0)->audio_lpf), nullptr},
    {"igateEn", CF_B, offsetof(Configuration, igate_en), sizeof(((Configuration*)0)->igate_en), nullptr},
    {"igateBcn", CF_B, offsetof(Configuration, igate_bcn), sizeof(((Configuration*)0)->igate_bcn), nullptr},
    {"rf2inet", CF_B, offsetof(Configuration, rf2inet), sizeof(((Configuration*)0)->rf2inet), nullptr},
    {"inet2rf", CF_B, offsetof(Configuration, inet2rf), sizeof(((Configuration*)0)->inet2rf), nullptr},
    {"igatePos2rf", CF_B, offsetof(Configuration, igate_loc2rf), sizeof(((Configuration*)0)->igate_loc2rf), nullptr},
    {"igatePos2inet", CF_B, offsetof(Configuration, igate_loc2inet), sizeof(((Configuration*)0)->igate_loc2inet), nullptr},
    {"rf2inetFilter", CF_U16, offsetof(Configuration, rf2inetFilter), sizeof(((Configuration*)0)->rf2inetFilter), nullptr},
    {"inet2rfFiltger", CF_U16, offsetof(Configuration, inet2rfFilter), sizeof(((Configuration*)0)->inet2rfFilter), nullptr},
    {"igateSSID", CF_U8, offsetof(Configuration, aprs_ssid), sizeof(((Configuration*)0)->aprs_ssid), nullptr},
    {"igatePort", CF_U16, offsetof(Configuration, aprs_port), sizeof(((Configuration*)0)->aprs_port), nullptr},
    {"igateMycall", CF_S, offsetof(Configuration, aprs_mycall), sizeof(((Configuration*)0)->aprs_mycall), "NOCALL"},
    {"igateHost", CF_S, offsetof(Configuration, aprs_host), sizeof(((Configuration*)0)->aprs_host), ""},
    {"igateFilter", CF_S, offsetof(Configuration, aprs_filter), sizeof(((Configuration*)0)->aprs_filter), ""},
    {"igateGPS", CF_B, offsetof(Configuration, igate_gps), sizeof(((Configuration*)0)->igate_gps), nullptr},
    {"igateLAT", CF_F, offsetof(Configuration, igate_lat), sizeof(((Configuration*)0)->igate_lat), nullptr},
    {"igateLON", CF_F, offsetof(Configuration, igate_lon), sizeof(((Configuration*)0)->igate_lon), nullptr},
    {"igateALT", CF_F, offsetof(Configuration, igate_alt), sizeof(((Configuration*)0)->igate_alt), nullptr},
    {"igateINV", CF_U16, offsetof(Configuration, igate_interval), sizeof(((Configuration*)0)->igate_interval), nullptr},
    {"igateObject", CF_S, offsetof(Configuration, igate_object), sizeof(((Configuration*)0)->igate_object), ""},
    {"igatePHG", CF_S, offsetof(Configuration, igate_phg), sizeof(((Configuration*)0)->igate_phg), ""},
    {"igatePath", CF_U8, offsetof(Configuration, igate_path), sizeof(((Configuration*)0)->igate_path), nullptr},
    {"igateComment", CF_S, offsetof(Configuration, igate_comment), sizeof(((Configuration*)0)->igate_comment), ""},
    {"igateSTSIntv", CF_U16, offsetof(Configuration, igate_sts_interval), sizeof(((Configuration*)0)->igate_sts_interval), nullptr},
    {"digiEn", CF_B, offsetof(Configuration, digi_en), sizeof(((Configuration*)0)->digi_en), nullptr},
    {"digiAuto", CF_B, offsetof(Configuration, digi_auto), sizeof(((Configuration*)0)->digi_auto), nullptr},
    {"digiPos2rf", CF_B, offsetof(Configuration, digi_loc2rf), sizeof(((Configuration*)0)->digi_loc2rf), nullptr},
    {"digiPos2inet", CF_B, offsetof(Configuration, digi_loc2inet), sizeof(((Configuration*)0)->digi_loc2inet), nullptr},
    {"digiTime", CF_B, offsetof(Configuration, digi_timestamp), sizeof(((Configuration*)0)->digi_timestamp), nullptr},
    {"digiSSID", CF_U8, offsetof(Configuration, digi_ssid), sizeof(((Configuration*)0)->digi_ssid), nullptr},
    {"digiMycall", CF_S, offsetof(Configuration, digi_mycall), sizeof(((Configuration*)0)->digi_mycall), ""},
    {"digiPath", CF_U8, offsetof(Configuration, digi_path), sizeof(((Configuration*)0)->digi_path), nullptr},
    {"digiDelay", CF_U16, offsetof(Configuration, digi_delay), sizeof(((Configuration*)0)->digi_delay), nullptr},
    {"digiFilter", CF_U16, offsetof(Configuration, digiFilter), sizeof(((Configuration*)0)->digiFilter), nullptr},
    {"digiBcn", CF_B, offsetof(Configuration, digi_bcn), sizeof(((Configuration*)0)->digi_bcn), nullptr},
    {"digiAlt", CF_F, offsetof(Configuration, digi_alt), sizeof(((Configuration*)0)->digi_alt), nullptr},
    {"digiGPS", CF_B, offsetof(Configuration, digi_gps), sizeof(((Configuration*)0)->digi_gps), nullptr},
    {"digiLAT", CF_F, offsetof(Configuration, digi_lat), sizeof(((Configuration*)0)->digi_lat), nullptr},
    {"digiLON", CF_F, offsetof(Configuration, digi_lon), sizeof(((Configuration*)0)->digi_lon), nullptr},
    {"digiINV", CF_U16, offsetof(Configuration, digi_interval), sizeof(((Configuration*)0)->digi_interval), nullptr},
    {"digiPHG", CF_S, offsetof(Configuration, digi_phg), sizeof(((Configuration*)0)->digi_phg), ""},
    {"digiComment", CF_S, offsetof(Configuration, digi_comment), sizeof(((Configuration*)0)->digi_comment), ""},
    {"digiSTSIntv", CF_U16, offsetof(Configuration, digi_sts_interval), sizeof(((Configuration*)0)->digi_sts_interval), nullptr},
    {"trkEn", CF_B, offsetof(Configuration, trk_en), sizeof(((Configuration*)0)->trk_en), nullptr},
    {"trkPos2rf", CF_B, offsetof(Configuration, trk_loc2rf), sizeof(((Configuration*)0)->trk_loc2rf), nullptr},
    {"trkPos2inet", CF_B, offsetof(Configuration, trk_loc2inet), sizeof(((Configuration*)0)->trk_loc2inet), nullptr},
    {"trkTime", CF_B, offsetof(Configuration, trk_timestamp), sizeof(((Configuration*)0)->trk_timestamp), nullptr},
    {"trkSSID", CF_U8, offsetof(Configuration, trk_ssid), sizeof(((Configuration*)0)->trk_ssid), nullptr},
    {"trkMycall", CF_S, offsetof(Configuration, trk_mycall), sizeof(((Configuration*)0)->trk_mycall), ""},
    {"trkPath", CF_U8, offsetof(Configuration, trk_path), sizeof(((Configuration*)0)->trk_path), nullptr},
    {"trkGPS", CF_B, offsetof(Configuration, trk_gps), sizeof(((Configuration*)0)->trk_gps), nullptr},
    {"trkLAT", CF_F, offsetof(Configuration, trk_lat), sizeof(((Configuration*)0)->trk_lat), nullptr},
    {"trkLON", CF_F, offsetof(Configuration, trk_lon), sizeof(((Configuration*)0)->trk_lon), nullptr},
    {"trkALT", CF_F, offsetof(Configuration, trk_alt), sizeof(((Configuration*)0)->trk_alt), nullptr},
    {"trkComment", CF_S, offsetof(Configuration, trk_comment), sizeof(((Configuration*)0)->trk_comment), ""},
    {"trkSTSIntv", CF_U16, offsetof(Configuration, trk_sts_interval), sizeof(((Configuration*)0)->trk_sts_interval), nullptr},
    {"wxEn", CF_B, offsetof(Configuration, wx_en), sizeof(((Configuration*)0)->wx_en), nullptr},
    {"wxTx2rf", CF_B, offsetof(Configuration, wx_2rf), sizeof(((Configuration*)0)->wx_2rf), nullptr},
    {"wxTx2inet", CF_B, offsetof(Configuration, wx_2inet), sizeof(((Configuration*)0)->wx_2inet), nullptr},
    {"wxTime", CF_B, offsetof(Configuration, wx_timestamp), sizeof(((Configuration*)0)->wx_timestamp), nullptr},
    {"wxSSID", CF_U8, offsetof(Configuration, wx_ssid), sizeof(((Configuration*)0)->wx_ssid), nullptr},
    {"wxMycall", CF_S, offsetof(Configuration, wx_mycall), sizeof(((Configuration*)0)->wx_mycall), ""},
    {"wxPath", CF_U8, offsetof(Configuration, wx_path), sizeof(((Configuration*)0)->wx_path), nullptr},
    {"wxGPS", CF_B, offsetof(Configuration, wx_gps), sizeof(((Configuration*)0)->wx_gps), nullptr},
    {"wxLAT", CF_F, offsetof(Configuration, wx_lat), sizeof(((Configuration*)0)->wx_lat), nullptr},
    {"wxLON", CF_F, offsetof(Configuration, wx_lon), sizeof(((Configuration*)0)->wx_lon), nullptr},
    {"wxALT", CF_F, offsetof(Configuration, wx_alt), sizeof(((Configuration*)0)->wx_alt), nullptr},
    {"wxInv", CF_U16, offsetof(Configuration, wx_interval), sizeof(((Configuration*)0)->wx_interval), nullptr},
    {"wxFlage", CF_U32, offsetof(Configuration, wx_flage), sizeof(((Configuration*)0)->wx_flage), nullptr},
    {"wxObject", CF_S, offsetof(Configuration, wx_object), sizeof(((Configuration*)0)->wx_object), ""},
    {"wxComment", CF_S, offsetof(Configuration, wx_comment), sizeof(((Configuration*)0)->wx_comment), ""},
    {"tlmEn", CF_B, offsetof(Configuration, tlm0_en), sizeof(((Configuration*)0)->tlm0_en), nullptr},
    {"tlmTx2rf", CF_B, offsetof(Configuration, tlm0_2rf), sizeof(((Configuration*)0)->tlm0_2rf), nullptr},
    {"tlmTx2inet", CF_B, offsetof(Configuration, tlm0_2inet), sizeof(((Configuration*)0)->tlm0_2inet), nullptr},
    {"tlmSSID", CF_U8, offsetof(Configuration, tlm0_ssid), sizeof(((Configuration*)0)->tlm0_ssid), nullptr},
    {"tlmMycall", CF_S, offsetof(Configuration, tlm0_mycall), sizeof(((Configuration*)0)->tlm0_mycall), ""},
    {"tlmPath", CF_U8, offsetof(Configuration, tlm0_path), sizeof(((Configuration*)0)->tlm0_path), nullptr},
    {"tlmInfoInv", CF_U16, offsetof(Configuration, tlm0_info_interval), sizeof(((Configuration*)0)->tlm0_info_interval), nullptr},
    {"tlmDataInv", CF_U16, offsetof(Configuration, tlm0_data_interval), sizeof(((Configuration*)0)->tlm0_data_interval), nullptr},
    {"tlmBIT", CF_U8, offsetof(Configuration, tlm0_BITS_Active), sizeof(((Configuration*)0)->tlm0_BITS_Active), nullptr},
    {"dspEn", CF_B, offsetof(Configuration, oled_enable), sizeof(((Configuration*)0)->oled_enable), nullptr},
    {"dspTOut", CF_INT, offsetof(Configuration, oled_timeout), sizeof(((Configuration*)0)->oled_timeout), nullptr},
    {"dspDim", CF_U8, offsetof(Configuration, dim), sizeof(((Configuration*)0)->dim), nullptr},
    {"dspContrast", CF_U8, offsetof(Configuration, contrast), sizeof(((Configuration*)0)->contrast), nullptr},
    {"dspBright", CF_U8, offsetof(Configuration, disp_brightness), sizeof(((Configuration*)0)->disp_brightness), nullptr},
    {"dspStartUp", CF_U8, offsetof(Configuration, startup), sizeof(((Configuration*)0)->startup), nullptr},
    {"dspDelay", CF_UINT, offsetof(Configuration, dispDelay), sizeof(((Configuration*)0)->dispDelay), nullptr},
    {"dspDxFilter", CF_UINT, offsetof(Configuration, filterDistant), sizeof(((Configuration*)0)->filterDistant), nullptr},
    {"dspFilter", CF_U16, offsetof(Configuration, dispFilter), sizeof(((Configuration*)0)->dispFilter), nullptr},
    {"dspRF", CF_B, offsetof(Configuration, dispRF), sizeof(((Configuration*)0)->dispRF), nullptr},
    {"dspINET", CF_B, offsetof(Configuration, dispINET), sizeof(((Configuration*)0)->dispINET), nullptr},
    {"dspFlip", CF_B, offsetof(Configuration, disp_flip), sizeof(((Configuration*)0)->disp_flip), nullptr},
    {"dspType", CF_U8, offsetof(Configuration, disp_type), sizeof(((Configuration*)0)->disp_type), nullptr}, // 2.1.9
    {"dspRotate", CF_U8, offsetof(Configuration, disp_rotate), sizeof(((Configuration*)0)->disp_rotate), nullptr}, // 2.1.9
    {"dspBtn", CF_I8, offsetof(Configuration, disp_btn), sizeof(((Configuration*)0)->disp_btn), nullptr}, // 2.1.9 (missing = 0 = BOOT)
    {"vpnEn", CF_B, offsetof(Configuration, vpn), sizeof(((Configuration*)0)->vpn), nullptr},
    {"vpnPort", CF_U16, offsetof(Configuration, wg_port), sizeof(((Configuration*)0)->wg_port), nullptr},
    {"vpnPeer", CF_S, offsetof(Configuration, wg_peer_address), sizeof(((Configuration*)0)->wg_peer_address), ""},
    {"vpnLocal", CF_S, offsetof(Configuration, wg_local_address), sizeof(((Configuration*)0)->wg_local_address), ""},
    {"vpnNetmark", CF_S, offsetof(Configuration, wg_netmask_address), sizeof(((Configuration*)0)->wg_netmask_address), ""},
    {"vpnGW", CF_S, offsetof(Configuration, wg_gw_address), sizeof(((Configuration*)0)->wg_gw_address), ""},
    {"vpnPubKey", CF_S, offsetof(Configuration, wg_public_key), sizeof(((Configuration*)0)->wg_public_key), ""},
    {"vpnPriKey", CF_S, offsetof(Configuration, wg_private_key), sizeof(((Configuration*)0)->wg_private_key), ""},
    {"httpUser", CF_S, offsetof(Configuration, http_username), sizeof(((Configuration*)0)->http_username), ""},
    {"httpPass", CF_S, offsetof(Configuration, http_password), sizeof(((Configuration*)0)->http_password), ""},
    {"gnssEn", CF_B, offsetof(Configuration, gnss_enable), sizeof(((Configuration*)0)->gnss_enable), nullptr},
    {"gnssTCPPort", CF_U16, offsetof(Configuration, gnss_tcp_port), sizeof(((Configuration*)0)->gnss_tcp_port), nullptr},
    {"gnssTCPHost", CF_S, offsetof(Configuration, gnss_tcp_host), sizeof(((Configuration*)0)->gnss_tcp_host), ""},
    {"gnssAT", CF_S, offsetof(Configuration, gnss_at_command), sizeof(((Configuration*)0)->gnss_at_command), ""},
    {"adcOffset", CF_U16, offsetof(Configuration, adc_dc_offset), sizeof(((Configuration*)0)->adc_dc_offset), nullptr},
    {"rfBaudrate", CF_ULONG, offsetof(Configuration, rf_baudrate), sizeof(((Configuration*)0)->rf_baudrate), nullptr},
    {"i2cEn", CF_B, offsetof(Configuration, i2c_enable), sizeof(((Configuration*)0)->i2c_enable), nullptr},
    {"i2c1En", CF_B, offsetof(Configuration, i2c1_enable), sizeof(((Configuration*)0)->i2c1_enable), nullptr},
    {"uart0BR", CF_ULONG, offsetof(Configuration, uart0_baudrate), sizeof(((Configuration*)0)->uart0_baudrate), nullptr},
    {"uart1BR", CF_ULONG, offsetof(Configuration, uart1_baudrate), sizeof(((Configuration*)0)->uart1_baudrate), nullptr},
#ifdef BLUETOOTH
    {"btSlave", CF_B, offsetof(Configuration, bt_slave), sizeof(((Configuration*)0)->bt_slave), nullptr},
    {"btMaster", CF_B, offsetof(Configuration, bt_master), sizeof(((Configuration*)0)->bt_master), nullptr},
    {"btMode", CF_U8, offsetof(Configuration, bt_mode), sizeof(((Configuration*)0)->bt_mode), nullptr},
    {"btPower", CF_U8, offsetof(Configuration, bt_power), sizeof(((Configuration*)0)->bt_power), nullptr},
    {"btPin", CF_U32, offsetof(Configuration, bt_pin), sizeof(((Configuration*)0)->bt_pin), nullptr},
    {"btName", CF_S, offsetof(Configuration, bt_name), sizeof(((Configuration*)0)->bt_name), ""},
#endif
#ifdef BLUETOOTH
#if !defined(CONFIG_IDF_TARGET_ESP32)
    {"btUUID", CF_S, offsetof(Configuration, bt_uuid), sizeof(((Configuration*)0)->bt_uuid), ""},
    {"btUUIDRx", CF_S, offsetof(Configuration, bt_uuid_rx), sizeof(((Configuration*)0)->bt_uuid_rx), ""},
    {"btUUIDTx", CF_S, offsetof(Configuration, bt_uuid_tx), sizeof(((Configuration*)0)->bt_uuid_tx), ""},
#endif
#endif
    {"pppAPN", CF_S, offsetof(Configuration, ppp_apn), sizeof(((Configuration*)0)->ppp_apn), ""},
    {"pppPin", CF_S, offsetof(Configuration, ppp_pin), sizeof(((Configuration*)0)->ppp_pin), ""},
#ifdef MQTT
    {"mqttEnable", CF_B, offsetof(Configuration, en_mqtt), sizeof(((Configuration*)0)->en_mqtt), nullptr},
    {"mqttHost", CF_S, offsetof(Configuration, mqtt_host), sizeof(((Configuration*)0)->mqtt_host), ""},
    {"mqttTopic", CF_S, offsetof(Configuration, mqtt_topic), sizeof(((Configuration*)0)->mqtt_topic), ""},
    {"mqttSub", CF_S, offsetof(Configuration, mqtt_subscribe), sizeof(((Configuration*)0)->mqtt_subscribe), ""},
    {"mqttTopicFlag", CF_U16, offsetof(Configuration, mqtt_topic_flag), sizeof(((Configuration*)0)->mqtt_topic_flag), nullptr},
    {"mqttSubFlag", CF_U16, offsetof(Configuration, mqtt_subscribe_flag), sizeof(((Configuration*)0)->mqtt_subscribe_flag), nullptr},
    {"mqttPort", CF_U16, offsetof(Configuration, mqtt_port), sizeof(((Configuration*)0)->mqtt_port), nullptr},
    {"mqttUser", CF_S, offsetof(Configuration, mqtt_user), sizeof(((Configuration*)0)->mqtt_user), ""},
    {"mqttPass", CF_S, offsetof(Configuration, mqtt_pass), sizeof(((Configuration*)0)->mqtt_pass), ""},
#endif
    {"trkMicEType", CF_U8, offsetof(Configuration, trk_mice_type), sizeof(((Configuration*)0)->trk_mice_type), nullptr},
    {"trkTlmInv", CF_U8, offsetof(Configuration, trk_tlm_interval), sizeof(((Configuration*)0)->trk_tlm_interval), nullptr},
    {"digiTlmInv", CF_U8, offsetof(Configuration, digi_tlm_interval), sizeof(((Configuration*)0)->digi_tlm_interval), nullptr},
    {"igateTlmInv", CF_U8, offsetof(Configuration, igate_tlm_interval), sizeof(((Configuration*)0)->igate_tlm_interval), nullptr},
    {"hostName", CF_S, offsetof(Configuration, host_name), sizeof(((Configuration*)0)->host_name), ""},
    {"resetTimeout", CF_U16, offsetof(Configuration, reset_timeout), sizeof(((Configuration*)0)->reset_timeout), nullptr},
    {"cmdOnMqtt", CF_B, offsetof(Configuration, at_cmd_mqtt), sizeof(((Configuration*)0)->at_cmd_mqtt), nullptr},
    {"cmdOnMsg", CF_B, offsetof(Configuration, at_cmd_msg), sizeof(((Configuration*)0)->at_cmd_msg), nullptr},
    {"cmdOnBluetooth", CF_B, offsetof(Configuration, at_cmd_bluetooth), sizeof(((Configuration*)0)->at_cmd_bluetooth), nullptr},
    {"cmdOnUart", CF_U8, offsetof(Configuration, at_cmd_uart), sizeof(((Configuration*)0)->at_cmd_uart), nullptr},
};

static void cfgSaveTable(JsonDocument &doc, const Configuration &c)
{
    for (const CfgField &e : CFG_FIELDS)
    {
        const uint8_t *p = (const uint8_t *)&c + e.off;
        switch (e.type)
        {
        case CF_B: doc[e.key] = *(const bool *)p; break;
        case CF_U8: doc[e.key] = *(const uint8_t *)p; break;
        case CF_I8: doc[e.key] = *(const int8_t *)p; break;
        case CF_U16: doc[e.key] = *(const uint16_t *)p; break;
        case CF_I16: doc[e.key] = *(const int16_t *)p; break;
        case CF_INT: doc[e.key] = *(const int *)p; break;
        case CF_UINT: doc[e.key] = *(const unsigned int *)p; break;
        case CF_LONG: doc[e.key] = *(const long *)p; break;
        case CF_ULONG: doc[e.key] = *(const unsigned long *)p; break;
        case CF_I32: doc[e.key] = *(const int32_t *)p; break;
        case CF_U32: doc[e.key] = *(const uint32_t *)p; break;
        case CF_F: doc[e.key] = *(const float *)p; break;
        case CF_D: doc[e.key] = *(const double *)p; break;
        case CF_S: doc[e.key] = (const char *)p; break;
        }
    }
}

static void cfgLoadTable(JsonDocument &doc, Configuration &c)
{
    for (const CfgField &e : CFG_FIELDS)
    {
        uint8_t *p = (uint8_t *)&c + e.off;
        JsonVariant v = doc[e.key];
        switch (e.type)
        {
        case CF_B: *(bool *)p = v.as<bool>(); break;
        case CF_U8: *(uint8_t *)p = v.as<uint8_t>(); break;
        case CF_I8: *(int8_t *)p = v.as<int8_t>(); break;
        case CF_U16: *(uint16_t *)p = v.as<uint16_t>(); break;
        case CF_I16: *(int16_t *)p = v.as<int16_t>(); break;
        case CF_INT: *(int *)p = v.as<int>(); break;
        case CF_UINT: *(unsigned int *)p = v.as<unsigned int>(); break;
        case CF_LONG: *(long *)p = v.as<long>(); break;
        case CF_ULONG: *(unsigned long *)p = v.as<unsigned long>(); break;
        case CF_I32: *(int32_t *)p = v.as<int32_t>(); break;
        case CF_U32: *(uint32_t *)p = v.as<uint32_t>(); break;
        case CF_F: *(float *)p = v.as<float>(); break;
        case CF_D: *(double *)p = v.as<double>(); break;
        case CF_S: strlcpy((char *)p, v | e.dflt, e.size); break;
        }
    }
}

bool saveConfiguration(const char *filename, const Configuration &config)
{
    // Delete existing file, otherwise the configuration is appended to the file
    //AFSK_TimerEnable(false);
    // if (LITTLEFS.exists(filename))
    // {
    //     LITTLEFS.remove(filename);
    //     log_d("Remove file %s",filename);
    // }

    // Open file for writing
    // File file = LITTLEFS.open(filename, FILE_WRITE);
    // if (!file)
    // {
    //     log_d("Failed to create file");
    //     return false;
    // }
    
    log_d("Create file %s",filename);
    //AFSK_TimerEnable(false);
    // Allocate a temporary JsonDocument
    //adcEn=-1;
    //delay(500);
    //StaticJsonDocument<6000> doc;    
    JsonDocument doc;
    cfgSaveTable(doc, config); // 2.1.9: plain fields (table above)
    
    // Set the values in the document
    doc["cpuFreq"] = config.cpuFreq;
    doc["txTimeSlot"] = config.tx_timeslot;
    doc["ntpHost"] = config.ntp_host;
    doc["WiFiMode"] = (signed char)config.wifi_mode;
    doc["WiFiAP_SSID"] = config.wifi_ap_ssid;
    doc["WiFiAP_PASS"] = config.wifi_ap_pass;
    JsonArray wifiArray = doc["WiFiSTA"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
    {
        wifiArray.add(config.wifi_sta[i].enable);
        wifiArray.add(config.wifi_sta[i].wifi_ssid);
        wifiArray.add(config.wifi_sta[i].wifi_pass);
    }

    doc["rfPwr"] = config.rf_power;
    doc["txAudioGain"] = config.tx_audio_gain;
    doc["txPreemphasis"] = config.tx_preemphasis;
    doc["txPreemphasisTilt"] = config.tx_preemphasis_tilt;
    // doc["rfFreq"] = config.rf_freq;
    // doc["rfOffset"] = config.rf_freq_offset;
    // doc["rfBW"] = config.rf_bw;
    // doc["rfSF"] = config.rf_sf;
    // doc["rfCR"] = config.rf_cr;
    // doc["rfSync"] = config.rf_sync;
    // doc["rfPwr"] = config.rf_power;
    // doc["rfPream"] = config.rf_preamable;
    // doc["rfLNA"] = config.rf_lna;
    // doc["rfMode"] = config.rf_mode;
    // doc["rfAX25"] = config.rf_ax25;
    // doc["rfBR"] = config.rf_br;
    // doc["rfShaping"] = config.rf_shaping;
    // doc["rfEncoding"] = config.rf_encoding;

    // IGate group

    // doc["igatePassCode"]=config.aprs_passcode;
    doc["igateSymbol"] = config.igate_symbol;
    doc["igateStatus"] = config.igate_status;
    JsonArray igateTlmAvg = doc["igateTlmAvg"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmAvg.add(config.igate_tlm_avg[i]);
    JsonArray igateTlmSen = doc["igateTlmSen"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmSen.add(config.igate_tlm_sensor[i]);
    JsonArray igateTlmPrec = doc["igateTlmPrec"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmPrec.add(config.igate_tlm_precision[i]);
    JsonArray igateTlmOffset = doc["igateTlmOffset"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmOffset.add(config.igate_tlm_offset[i]);
    JsonArray igateTlmPARM = doc["igateTlmPARM"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmPARM.add(config.igate_tlm_PARM[i]);
    JsonArray igateTlmUNIT = doc["igateTlmUNIT"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        igateTlmUNIT.add(config.igate_tlm_UNIT[i]);
    JsonArray igateTlmEQNS = doc["igateTlmEQNS"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
    {
        igateTlmEQNS.add(config.igate_tlm_EQNS[i][0]);
        igateTlmEQNS.add(config.igate_tlm_EQNS[i][1]);
        igateTlmEQNS.add(config.igate_tlm_EQNS[i][2]);
    }

    // Digi group
    doc["digiSymbol"] = config.digi_symbol;
    doc["digiStatus"] = config.digi_status;
    JsonArray digiTlmAvg = doc["digiTlmAvg"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmAvg.add(config.digi_tlm_avg[i]);
    JsonArray digiTlmSen = doc["digiTlmSen"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmSen.add(config.digi_tlm_sensor[i]);
    JsonArray digiTlmPrec = doc["digiTlmPrec"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmPrec.add(config.digi_tlm_precision[i]);
    JsonArray digiTlmOffset = doc["digiTlmOffset"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmOffset.add(config.digi_tlm_offset[i]);
    JsonArray digiTlmPARM = doc["digiTlmPARM"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmPARM.add(config.digi_tlm_PARM[i]);
    JsonArray digiTlmUNIT = doc["digiTlmUNIT"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        digiTlmUNIT.add(config.digi_tlm_UNIT[i]);
    JsonArray digiTlmEQNS = doc["digiTlmEQNS"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
    {
        digiTlmEQNS.add(config.digi_tlm_EQNS[i][0]);
        digiTlmEQNS.add(config.digi_tlm_EQNS[i][1]);
        digiTlmEQNS.add(config.digi_tlm_EQNS[i][2]);
    }

    // Tracker group
    doc["trkINV"] = config.trk_interval;
    doc["trkSmart"] = config.trk_smartbeacon;
    doc["trkCompress"] = config.trk_compress;
    doc["trkOptAlt"] = config.trk_altitude;
    doc["trkLog"] = config.trk_log;
    doc["trkOptRSSI"] = config.trk_rssi;
    doc["trkLSpeed"] = config.trk_lspeed;
    doc["trkHSpeed"] = config.trk_hspeed;
    doc["trkMaxInv"] = config.trk_maxinterval;
    doc["trkMinInv"] = config.trk_mininterval;
    doc["trkMinDir"] = config.trk_minangle;
    doc["trkSlowInv"] = config.trk_slowinterval;
    doc["trkSymbol"] = config.trk_symbol;
    doc["trkSymbolMove"] = config.trk_symmove;
    doc["trkSymbolStop"] = config.trk_symstop;
    doc["trkItem"] = config.trk_item;
    doc["trkStatus"] = config.trk_status;
    JsonArray trkTlmAvg = doc["trkTlmAvg"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmAvg.add(config.trk_tlm_avg[i]);
    JsonArray trkTlmSen = doc["trkTlmSen"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmSen.add(config.trk_tlm_sensor[i]);
    JsonArray trkTlmPrec = doc["trkTlmPrec"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmPrec.add(config.trk_tlm_precision[i]);
    JsonArray trkTlmOffset = doc["trkTlmOffset"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmOffset.add(config.trk_tlm_offset[i]);
    JsonArray trkTlmPARM = doc["trkTlmPARM"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmPARM.add(config.trk_tlm_PARM[i]);
    JsonArray trkTlmUNIT = doc["trkTlmUNIT"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
        trkTlmUNIT.add(config.trk_tlm_UNIT[i]);
    JsonArray trkTlmEQNS = doc["trkTlmEQNS"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
    {
        trkTlmEQNS.add(config.trk_tlm_EQNS[i][0]);
        trkTlmEQNS.add(config.trk_tlm_EQNS[i][1]);
        trkTlmEQNS.add(config.trk_tlm_EQNS[i][2]);
    }

    // WX group

    JsonArray wxSenEn = doc["wxSenEn"].to<JsonArray>();
    for (int i = 0; i < WX_SENSOR_NUM; i++)
    {
        wxSenEn.add(config.wx_sensor_enable[i]);
    }
    JsonArray wxSenAvg = doc["wxSenAvg"].to<JsonArray>();
    for (int i = 0; i < WX_SENSOR_NUM; i++)
    {
        wxSenAvg.add(config.wx_sensor_avg[i]);
    }
    JsonArray wxSenCH = doc["wxSenCH"].to<JsonArray>();
    for (int i = 0; i < WX_SENSOR_NUM; i++)
    {
        wxSenCH.add(config.wx_sensor_ch[i]);
    }

    // Telemetry
    // doc["tlmDataCH"]=config.tml0_data_channel;
    JsonArray tlmEQNS = doc["tlmEQNS"].to<JsonArray>();
    for (int i = 0; i < 5; i++)
    {
        tlmEQNS.add(config.tlm0_EQNS[i][0]);
        tlmEQNS.add(config.tlm0_EQNS[i][1]);
        tlmEQNS.add(config.tlm0_EQNS[i][2]);
    }
    JsonArray tlmPARM = doc["tlmPARM"].to<JsonArray>();
    for (int i = 0; i < 13; i++)
    {
        tlmPARM.add(config.tlm0_PARM[i]);
    }
    JsonArray tlmUNIT = doc["tlmUNIT"].to<JsonArray>();
    for (int i = 0; i < 13; i++)
    {
        tlmUNIT.add(config.tlm0_UNIT[i]);
    }
    JsonArray tlmDataCH = doc["tlmDataCH"].to<JsonArray>();
    for (int i = 0; i < 13; i++)
    {
        tlmDataCH.add(config.tml0_data_channel[i]);
    }

    // OLED Display
    doc["dspHUp"] = config.h_up;
    doc["dspTX"] = config.tx_display;
    doc["dspRX"] = config.rx_display;

    // VPN Wireguard

    // System

    JsonArray path = doc["path"].to<JsonArray>();
    for (int i = 0; i < 4; i++)
    {
        path.add(config.path[i]);
    }

    // MOD GNSS group
    doc["gnssCH"] = config.gnss_channel;
    doc["gnssPPS"] = config.gnss_pps_gpio;

    // MOD RF group
    doc["rfTx"] = config.rf_tx_gpio;
    doc["rfRx"] = config.rf_rx_gpio;
    doc["rfSQL"] = config.rf_sql_gpio;
    doc["rfPD"] = config.rf_pd_gpio;
    doc["rfPWR"] = config.rf_pwr_gpio;
    doc["rfPTT"] = config.rf_ptt_gpio;
    doc["rfSQLAct"] = config.rf_sql_active;
    doc["rfPDAct"] = config.rf_pd_active;
    doc["rfPWRAct"] = config.rf_pwr_active;
    doc["rfPTTAct"] = config.rf_ptt_active;
    doc["adcAtten"] = config.adc_atten;

    // doc["rfRST"] = config.rf_reset_gpio;
    // doc["rfDIO0"] = config.rf_dio0_gpio;
    // doc["rfDIO1"] = config.rf_dio1_gpio;
    // doc["rfDIO2"] = config.rf_dio2_gpio;
    // doc["rfNSS"] = config.rf_nss_gpio;
    // doc["rfSCK"] = config.rf_sclk_gpio;
    // doc["rfMISO"] = config.rf_miso_gpio;
    // doc["rfMOSI"] = config.rf_mosi_gpio;
    // doc["rfTxAct"] = config.rf_tx_active;
    // doc["rfRxAct"] = config.rf_rx_active;
    // doc["rfRSTAct"] = config.rf_reset_active;
    // doc["rfNSSAct"] = config.rf_nss_active;
    // MOD I2C group
    doc["i2cSDA"] = config.i2c_sda_pin;
    doc["i2cSCK"] = config.i2c_sck_pin;
    // doc["i2cRST"]=config.i2c_rst_pin;
    doc["i2cFreq"] = config.i2c_freq;
    doc["i2c1SDA"] = config.i2c1_sda_pin;
    doc["i2c1SCK"] = config.i2c1_sck_pin;
    doc["i2c1Freq"] = config.i2c1_freq;
    // MOD 1-Wire group
    doc["oneWireEn"] = config.onewire_enable;
    doc["oneWireIO"] = config.onewire_gpio;

    // MOD UART
    doc["uart0En"] = config.uart0_enable;
    doc["uart0TX"] = config.uart0_tx_gpio;
    doc["uart0RX"] = config.uart0_rx_gpio;
    doc["uart0RTS"] = config.uart0_rts_gpio;
    doc["uart1En"] = config.uart1_enable;
    doc["uart1TX"] = config.uart1_tx_gpio;
    doc["uart1RX"] = config.uart1_rx_gpio;
    doc["uart1RTS"] = config.uart1_rts_gpio;
#if SOC_UART_NUM > 2
    doc["uart2En"] = config.uart2_enable;
    doc["uart2BR"] = config.uart2_baudrate;
    doc["uart2TX"] = config.uart2_tx_gpio;
    doc["uart2RX"] = config.uart2_rx_gpio;
#endif
    // MOD Modbus
    doc["modbusEn"] = config.modbus_enable;
    doc["modbusAddr"] = config.modbus_address;
    doc["modbusCh"] = config.modbus_channel;
    doc["modbusDE"] = config.modbus_de_gpio;
    // MOD Counter
    doc["cnt0En"] = config.counter0_enable;
    doc["cnt0Act"] = config.counter0_active;
    doc["cnt0IO"] = config.counter0_gpio;
    doc["cnt1En"] = config.counter1_enable;
    doc["cnt1Act"] = config.counter1_active;
    doc["cnt1IO"] = config.counter1_gpio;
    // MOD External TNC
    doc["extTNCEn"] = config.ext_tnc_enable;
    doc["extTNCCh"] = config.ext_tnc_channel;
    doc["extTNCMode"] = config.ext_tnc_mode;

    // MOD TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
    doc["tcpKissEn"] = config.tcp_kiss_enable;
    doc["tcpKissPort1"] = config.tcp_kiss_port1;
    doc["tcpKissPort2"] = config.tcp_kiss_port2;

    // Power control - only pwr_stanby_delay survives (Set/2026, LU6JMF): the
    // rest was a dead "Power Save Mode" toggle, removed to save space.
    doc["pwrStanby"] = config.pwr_stanby_delay;

#ifdef BLUETOOTH
#if !defined(CONFIG_IDF_TARGET_ESP32)
        // Bluetooth BLE
#endif
#endif

    doc["logFile"] = config.log;

    JsonArray sensor = doc["Sensor"].to<JsonArray>();
    for (int i = 0; i < SENSOR_NUMBER; i++)
    {
        sensor.add(config.sensor[i].enable);
        sensor.add(config.sensor[i].port);
        sensor.add(config.sensor[i].address);
        sensor.add(config.sensor[i].samplerate);
        sensor.add(config.sensor[i].averagerate);
        sensor.add(config.sensor[i].eqns[0]);
        sensor.add(config.sensor[i].eqns[1]);
        sensor.add(config.sensor[i].eqns[2]);
        sensor.add(config.sensor[i].type);
        sensor.add(config.sensor[i].parm);
        sensor.add(config.sensor[i].unit);
    }

     // PPP Modem
    doc["pppEn"] = config.ppp_enable;
    doc["pppRST"] = config.ppp_rst_gpio;
    doc["pppRSTAct"] = config.ppp_rst_active;
    doc["pppTX"] = config.ppp_tx_gpio;
    doc["pppRX"] = config.ppp_rx_gpio;
    doc["pppRTS"] = config.ppp_rts_gpio;
    doc["pppDTR"] = config.ppp_dtr_gpio;
    doc["pppCTS"] = config.ppp_cts_gpio;
    doc["pppRI"] = config.ppp_ri_gpio;
    doc["pppPWR"] = config.ppp_pwr_gpio;
    doc["pppPWRAct"] = config.ppp_pwr_active;
    doc["pppRSTDelay"] = config.ppp_rst_delay;
    doc["pppSerial"] = config.ppp_serial;
    doc["pppSerialBaudrate"] = config.ppp_serial_baudrate;
    doc["pppModel"] = config.ppp_model;
    doc["pppFlow"] = config.ppp_flow_ctrl;
    doc["pppGNSS"] = config.ppp_gnss;
    doc["pppNAPT"] = config.ppp_napt;

    #ifdef MQTT
    #endif



    doc["msgEnable"] = config.msg_enable;
    doc["msgMycall"] = config.msg_mycall;
    doc["msgRf"] = config.msg_rf;
    doc["msgInet"] = config.msg_inet;
    doc["msgPath"] = config.msg_path;
    doc["msgEncrypt"] = config.msg_encrypt;
    doc["msgAESKey"] = config.msg_key;
    doc["msgRetry"] = config.msg_retry;
    doc["msgInterval"] = config.msg_interval;

    // Bulletins BLN1-BLN9 - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
    for (uint8_t bi = 0; bi < 9; bi++)
    {
        char key[20];
        snprintf(key, sizeof(key), "blnEn%d", bi + 1);
        doc[key] = config.bln_en[bi];
        snprintf(key, sizeof(key), "blnText%d", bi + 1);
        doc[key] = config.bln_text[bi];
        snprintf(key, sizeof(key), "blnInterval%d", bi + 1);
        doc[key] = config.bln_interval[bi];
        snprintf(key, sizeof(key), "blnLimit%d", bi + 1);
        doc[key] = config.bln_limit[bi];

        // LU6JMF - Marcelo Ferreira:
        // BLN1-BLN9 share the same campaign lifetime and real-send counter.
        snprintf(key, sizeof(key), "blnDuration%d", bi + 1);
        doc[key] = config.bln_duration[bi];

        snprintf(key, sizeof(key), "blnStarted%d", bi + 1);
        doc[key] = config.bln_started_at[bi];

        snprintf(key, sizeof(key), "blnSentTotal%d", bi + 1);
        doc[key] = config.bln_sent_total[bi];
    }

    // Objects (Object1-4) - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
    // LU6JMF fix (Set/2026): estes campos nunca eram salvos aqui - o Object
    // vivia so em RAM e qualquer reboot apagava tudo (nome/texto/posicao/
    // ativo), mesmo com o commitObj corrigido. Agora persiste igual ao BLN.
    for (uint8_t oi = 0; oi < 4; oi++)
    {
        char okey[20];
        snprintf(okey, sizeof(okey), "objEn%d", oi + 1);
        doc[okey] = config.obj_en[oi];
        snprintf(okey, sizeof(okey), "objName%d", oi + 1);
        doc[okey] = config.obj_name[oi];
        snprintf(okey, sizeof(okey), "objLat%d", oi + 1);
        doc[okey] = config.obj_lat[oi];
        snprintf(okey, sizeof(okey), "objLon%d", oi + 1);
        doc[okey] = config.obj_lon[oi];
        snprintf(okey, sizeof(okey), "objSymbol%d", oi + 1);
        doc[okey] = config.obj_symbol[oi];
        snprintf(okey, sizeof(okey), "objText%d", oi + 1);
        doc[okey] = config.obj_text[oi];
        snprintf(okey, sizeof(okey), "objMode%d", oi + 1);
        doc[okey] = config.obj_mode[oi];
        snprintf(okey, sizeof(okey), "objInterval%d", oi + 1);
        doc[okey] = config.obj_interval[oi];
        snprintf(okey, sizeof(okey), "objLimit%d", oi + 1);
        doc[okey] = config.obj_limit[oi];
        snprintf(okey, sizeof(okey), "objActiveFor%d", oi + 1);
        doc[okey] = config.obj_activefor[oi];
        snprintf(okey, sizeof(okey), "objPermanent%d", oi + 1);
        doc[okey] = config.obj_permanent[oi];
    }

    // Serialize JSON to file
    File file = LITTLEFS.open(filename, FILE_WRITE);
    if (file)
    {
        int8_t adcEnBak=adcEn;
        int8_t dacEnBak=dacEn;
        //adcEn=-1;
        //dacEn=-1;
        
        //delay(500);
        if (serializeJson(doc, file) == 0)
        {
            log_d("Failed to write to file");
        }else{
            log_d("Write file configuration susses.");
        }
        // Close the file
        file.close();
        delay(100);
        //adcEn=1;
        dacEn=0;
    }
    doc.clear();
    return true;
}

// Loads the configuration from a file
bool loadConfiguration(const char *filename, Configuration &config)
{

    if (!LITTLEFS.exists(filename))
    {
        log_d("File %s not found.", filename);
        return false;
    }

    // Open file for reading
    File f = LITTLEFS.open(filename);
    if (f)
    {
        // Open file for reading
        log_d("Loading %s file. Size=%d", filename, f.size());

        // String str=f.readString();
        // log_d("%s",str.c_str());
        //  Allocate a temporary JsonDocument
        JsonDocument doc;

        // Deserialize the JSON document
        DeserializationError error = deserializeJson(doc, f);
        f.close();
        if (error)
        {
            log_d("Failed to read file, using default configuration");
            return false;
        }

        cfgLoadTable(doc, config); // 2.1.9: plain fields (table above)
        if (doc["dspRotate"].isNull())
            config.disp_rotate = 10; // 2.1.9: configuration saved before this option: screens change every 10 s
        config.cpuFreq = doc["cpuFreq"] | 160;
        config.tx_timeslot = doc["txTimeSlot"] | 2000;
        strlcpy(config.ntp_host, doc["ntpHost"], sizeof(config.ntp_host));
        config.wifi_mode = doc["WiFiMode"];
        strlcpy(config.wifi_ap_ssid, doc["WiFiAP_SSID"] | "ESP32LoRa", sizeof(config.wifi_ap_ssid));
        strlcpy(config.wifi_ap_pass, doc["WiFiAP_PASS"] | "", sizeof(config.wifi_ap_pass));
        // log_d("Load WiFiAP: SSID=%s Pass=%s",config.wifi_ap_ssid,config.wifi_ap_pass);
        for (int i = 0; i < 5; i++)
        {
            config.wifi_sta[i].enable = doc["WiFiSTA"][i * 3];
            strlcpy(config.wifi_sta[i].wifi_ssid, doc["WiFiSTA"][(i * 3) + 1] | "APRSTH", sizeof(config.wifi_sta[i].wifi_ssid));
            strlcpy(config.wifi_sta[i].wifi_pass, doc["WiFiSTA"][(i * 3) + 2] | "aprsthnetwork", sizeof(config.wifi_sta[i].wifi_pass));
            // if(config.wifi_sta[i].enable)
            // log_d("Load WiFiSTA[%i]: SSID=%s Pass=%s",i,config.wifi_sta[i].wifi_ssid,config.wifi_sta[i].wifi_pass);
        }

        config.rf_power = doc["rfPwr"];
        config.tx_audio_gain = doc["txAudioGain"] | 100;
        config.tx_preemphasis = doc["txPreemphasis"] | false;
        config.tx_preemphasis_tilt = doc["txPreemphasisTilt"] | 20;
        // config.rf_freq = doc["rfFreq"];
        // config.rf_freq_offset = doc["rfOffset"];
        // config.rf_bw = doc["rfBW"];
        // config.rf_sf = doc["rfSF"];
        // config.rf_cr = doc["rfCR"];
        // config.rf_sync = doc["rfSync"];
        // config.rf_power = doc["rfPwr"];
        // config.rf_preamable = doc["rfPream"];
        // config.rf_lna = doc["rfLNA"];
        // config.rf_mode = doc["rfMode"];
        // config.rf_ax25 = doc["rfAX25"];
        // config.rf_br = doc["rfBR"];
        // config.rf_shaping = doc["rfShaping"];
        // config.rf_encoding = doc["rfEncoding"];

        // IGate group

        strlcpy(config.igate_symbol, doc["igateSymbol"] | "", sizeof(config.igate_symbol));
        strlcpy(config.igate_status, doc["igateStatus"] | "", sizeof(config.igate_status));

        for (int i = 0; i < 5; i++)
            config.igate_tlm_avg[i] = doc["igateTlmAvg"][i];
        for (int i = 0; i < 5; i++)
            config.igate_tlm_sensor[i] = doc["igateTlmSen"][i];
        for (int i = 0; i < 5; i++)
            config.igate_tlm_precision[i] = doc["igateTlmPrec"][i];
        for (int i = 0; i < 5; i++)
            config.igate_tlm_offset[i] = doc["igateTlmOffset"][i];
        for (int i = 0; i < 5; i++)
            strlcpy(config.igate_tlm_PARM[i], doc["igateTlmPARM"][i] | "", sizeof(config.igate_tlm_PARM[i]));
        for (int i = 0; i < 5; i++)
            strlcpy(config.igate_tlm_UNIT[i], doc["igateTlmUNIT"][i] | "", sizeof(config.igate_tlm_UNIT[i]));
        for (int i = 0; i < 5; i++)
        {
            config.igate_tlm_EQNS[i][0] = doc["igateTlmEQNS"][i * 3];
            config.igate_tlm_EQNS[i][1] = doc["igateTlmEQNS"][(i * 3) + 1];
            config.igate_tlm_EQNS[i][2] = doc["igateTlmEQNS"][(i * 3) + 2];
        }
        // Digi group
        strlcpy(config.digi_symbol, doc["digiSymbol"] | "", sizeof(config.digi_symbol));
        strlcpy(config.digi_status, doc["digiStatus"] | "", sizeof(config.digi_status));
        for (int i = 0; i < 5; i++)
            config.digi_tlm_avg[i] = doc["digiTlmAvg"][i];
        for (int i = 0; i < 5; i++)
            config.digi_tlm_sensor[i] = doc["digiTlmSen"][i];
        for (int i = 0; i < 5; i++)
            config.digi_tlm_precision[i] = doc["digiTlmPrec"][i];
        for (int i = 0; i < 5; i++)
            config.digi_tlm_offset[i] = doc["digiTlmOffset"][i];
        for (int i = 0; i < 5; i++)
            strlcpy(config.digi_tlm_PARM[i], doc["digiTlmPARM"][i] | "", sizeof(config.digi_tlm_PARM[i]));
        for (int i = 0; i < 5; i++)
            strlcpy(config.digi_tlm_UNIT[i], doc["digiTlmUNIT"][i] | "", sizeof(config.digi_tlm_UNIT[i]));
        for (int i = 0; i < 5; i++)
        {
            config.digi_tlm_EQNS[i][0] = doc["digiTlmEQNS"][i * 3];
            config.digi_tlm_EQNS[i][1] = doc["digiTlmEQNS"][(i * 3) + 1];
            config.digi_tlm_EQNS[i][2] = doc["digiTlmEQNS"][(i * 3) + 2];
        }
        // Tracker group
        config.trk_interval = doc["trkINV"];
        config.trk_smartbeacon = doc["trkSmart"];
        config.trk_compress = doc["trkCompress"];
        config.trk_altitude = doc["trkOptAlt"];
        config.trk_log = doc["trkLog"];
        config.trk_rssi = doc["trkOptRSSI"];
        config.trk_lspeed = doc["trkLSpeed"];
        config.trk_hspeed = doc["trkHSpeed"];
        config.trk_maxinterval = doc["trkMaxInv"];
        config.trk_mininterval = doc["trkMinInv"];
        config.trk_minangle = doc["trkMinDir"];
        config.trk_slowinterval = doc["trkSlowInv"];
        strlcpy(config.trk_symbol, doc["trkSymbol"] | "", sizeof(config.trk_symbol));
        strlcpy(config.trk_symmove, doc["trkSymbolMove"] | "", sizeof(config.trk_symmove));
        strlcpy(config.trk_symstop, doc["trkSymbolStop"] | "", sizeof(config.trk_symstop));
        strlcpy(config.trk_item, doc["trkItem"] | "", sizeof(config.trk_item));
        strlcpy(config.trk_status, doc["trkStatus"] | "", sizeof(config.trk_status));
        for (int i = 0; i < 5; i++)
            config.trk_tlm_avg[i] = doc["trkTlmAvg"][i];
        for (int i = 0; i < 5; i++)
            config.trk_tlm_sensor[i] = doc["trkTlmSen"][i];
        for (int i = 0; i < 5; i++)
            config.trk_tlm_precision[i] = doc["trkTlmPrec"][i];
        for (int i = 0; i < 5; i++)
            config.trk_tlm_offset[i] = doc["trkTlmOffset"][i];
        for (int i = 0; i < 5; i++)
            strlcpy(config.trk_tlm_PARM[i], doc["trkTlmPARM"][i] | "", sizeof(config.trk_tlm_PARM[i]));
        for (int i = 0; i < 5; i++)
            strlcpy(config.trk_tlm_UNIT[i], doc["trkTlmUNIT"][i] | "", sizeof(config.trk_tlm_UNIT[i]));
        for (int i = 0; i < 5; i++)
        {
            config.trk_tlm_EQNS[i][0] = doc["trkTlmEQNS"][i * 3];
            config.trk_tlm_EQNS[i][1] = doc["trkTlmEQNS"][(i * 3) + 1];
            config.trk_tlm_EQNS[i][2] = doc["trkTlmEQNS"][(i * 3) + 2];
        }
        // WX group

        for (int i = 0; i < WX_SENSOR_NUM; i++)
        {
            config.wx_sensor_enable[i] = doc["wxSenEn"][i];
        }
        for (int i = 0; i < WX_SENSOR_NUM; i++)
        {
            config.wx_sensor_avg[i] = doc["wxSenAvg"][i];
        }
        for (int i = 0; i < WX_SENSOR_NUM; i++)
        {
            config.wx_sensor_ch[i] = doc["wxSenCH"][i];
        }

        // Telemetry

        for (int i = 0; i < 5; i++)
        {
            config.tlm0_EQNS[i][0] = doc["tlmEQNS"][i * 3];
            config.tlm0_EQNS[i][1] = doc["tlmEQNS"][(i * 3) + 1];
            config.tlm0_EQNS[i][2] = doc["tlmEQNS"][(i * 3) + 2];
        }
        for (int i = 0; i < 13; i++)
        {
            strlcpy(config.tlm0_PARM[i], doc["tlmPARM"][i] | "", sizeof(config.tlm0_PARM[i]));
        }
        for (int i = 0; i < 13; i++)
        {
            strlcpy(config.tlm0_UNIT[i], doc["tlmUNIT"][i] | "", sizeof(config.tlm0_UNIT[i]));
        }
        for (int i = 0; i < 13; i++)
        {
            config.tml0_data_channel[i] = doc["tlmDataCH"][i];
        }
        // OLED Display
        config.h_up = doc["dspHUp"];
        config.tx_display = doc["dspTX"];
        config.rx_display = doc["dspRX"];

        // VPN Wireguard

        // System

        for (int i = 0; i < 4; i++)
        {
            strlcpy(config.path[i], doc["path"][i] | "", sizeof(config.path[i]));
        }

        // MOD GNSS group
        config.gnss_channel = doc["gnssCH"];
        config.gnss_pps_gpio = doc["gnssPPS"];

        // MOD RF group
        config.rf_tx_gpio = doc["rfTx"];
        config.rf_rx_gpio = doc["rfRx"];
        config.rf_sql_gpio = doc["rfSQL"];
        config.rf_pd_gpio = doc["rfPD"];
        config.rf_pwr_gpio = doc["rfPWR"];
        config.rf_ptt_gpio = doc["rfPTT"];
        config.rf_sql_active = doc["rfSQLAct"];
        config.rf_pd_active = doc["rfPDAct"];
        config.rf_pwr_active = doc["rfPWRAct"];
        config.rf_ptt_active = doc["rfPTTAct"];
        config.adc_atten = doc["adcAtten"];

        // config.rf_reset_gpio = doc["rfRST"];
        // config.rf_dio0_gpio = doc["rfDIO0"];
        // config.rf_dio1_gpio = doc["rfDIO1"];
        // config.rf_dio2_gpio = doc["rfDIO2"];
        // config.rf_nss_gpio = doc["rfNSS"];
        // config.rf_sclk_gpio = doc["rfSCK"];
        // config.rf_miso_gpio = doc["rfMISO"];
        // config.rf_mosi_gpio = doc["rfMOSI"];
        // config.rf_tx_active = doc["rfTxAct"];
        // config.rf_rx_active = doc["rfRxAct"];
        // config.rf_reset_active = doc["rfRSTAct"];
        // config.rf_nss_active = doc["rfNSSAct"];
        // MOD I2C group
        config.i2c_sda_pin = doc["i2cSDA"];
        config.i2c_sck_pin = doc["i2cSCK"];
        // doc["i2cRST"]=config.i2c_rst_pin;
        config.i2c_freq = doc["i2cFreq"];
        config.i2c1_sda_pin = doc["i2c1SDA"];
        config.i2c1_sck_pin = doc["i2c1SCK"];
        config.i2c1_freq = doc["i2c1Freq"];
        // MOD 1-Wire group
        config.onewire_enable = doc["oneWireEn"];
        config.onewire_gpio = doc["oneWireIO"];

        // MOD UART
        config.uart0_enable = doc["uart0En"];
        config.uart0_tx_gpio = doc["uart0TX"];
        config.uart0_rx_gpio = doc["uart0RX"];
        config.uart0_rts_gpio = doc["uart0RTS"];
        config.uart1_enable = doc["uart1En"];
        config.uart1_tx_gpio = doc["uart1TX"];
        config.uart1_rx_gpio = doc["uart1RX"];
        config.uart1_rts_gpio = doc["uart1RTS"];
        // #if SOC_UART_NUM > 2
        config.uart2_baudrate = doc["uart2BR"];
        config.uart2_tx_gpio = doc["uart2TX"];
        config.uart2_rx_gpio = doc["uart2RX"];
        // #endif
        // MOD Modbus
        config.modbus_enable = doc["modbusEn"];
        config.modbus_address = doc["modbusAddr"];
        config.modbus_channel = doc["modbusCh"];
        config.modbus_de_gpio = doc["modbusDE"];
        // MOD Counter
        config.counter0_enable = doc["cnt0En"];
        config.counter0_active = doc["cnt0Act"];
        config.counter0_gpio = doc["cnt0IO"];
        config.counter1_enable = doc["cnt1En"];
        config.counter1_active = doc["cnt1Act"];
        config.counter1_gpio = doc["cnt1IO"];
        // MOD External TNC
        config.ext_tnc_enable = doc["extTNCEn"];
        config.ext_tnc_channel = doc["extTNCCh"];
        config.ext_tnc_mode = doc["extTNCMode"];

        // MOD TCP KISS Server - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
        // Uses "| default" so devices updating from an older config file (which won't have
        // these keys yet) get safe defaults instead of 0/false-for-everything.
        config.tcp_kiss_enable = doc["tcpKissEn"] | false;
        config.tcp_kiss_port1 = doc["tcpKissPort1"] | 8001;
        config.tcp_kiss_port2 = doc["tcpKissPort2"] | 8002;

        // Power control - only pwr_stanby_delay survives (Set/2026, LU6JMF)
        config.pwr_stanby_delay = doc["pwrStanby"] | 30;

#ifdef BLUETOOTH
#if !defined(CONFIG_IDF_TARGET_ESP32)
        // Bluetooth BLE
#endif
#endif

        config.log = doc["logFile"];

        for (int i = 0; i < SENSOR_NUMBER; i++)
        {
            config.sensor[i].enable = doc["Sensor"][(i * 11) + 0];
            config.sensor[i].port = doc["Sensor"][(i * 11) + 1];
            config.sensor[i].address = doc["Sensor"][(i * 11) + 2];
            config.sensor[i].samplerate = doc["Sensor"][(i * 11) + 3];
            config.sensor[i].averagerate = doc["Sensor"][(i * 11) + 4];
            config.sensor[i].eqns[0] = doc["Sensor"][(i * 11) + 5];
            config.sensor[i].eqns[1] = doc["Sensor"][(i * 11) + 6];
            config.sensor[i].eqns[2] = doc["Sensor"][(i * 11) + 7];
            config.sensor[i].type = doc["Sensor"][(i * 11) + 8];
            strlcpy(config.sensor[i].parm, doc["Sensor"][(i * 11) + 9] | "", sizeof(config.sensor[i].parm));
            strlcpy(config.sensor[i].unit, doc["Sensor"][(i * 11) + 10] | "", sizeof(config.sensor[i].unit));
        }

        // PPP Modem
        config.ppp_enable = doc["pppEn"];
        config.ppp_rst_gpio = doc["pppRST"];
        config.ppp_rst_active = doc["pppRSTAct"];
        config.ppp_rst_delay = doc["pppRSTDelay"];
        config.ppp_tx_gpio = doc["pppTX"];
        config.ppp_rx_gpio = doc["pppRX"];
        config.ppp_rts_gpio = doc["pppRTS"];
        config.ppp_dtr_gpio = doc["pppDTR"];
        config.ppp_cts_gpio = doc["pppCTS"];
        config.ppp_ri_gpio = doc["pppRI"];
        config.ppp_pwr_gpio = doc["pppPWR"];
        config.ppp_pwr_active = doc["pppPWRAct"];   
        config.ppp_serial = doc["pppSerial"];
        config.ppp_serial_baudrate = doc["pppSerialBaudrate"];
        config.ppp_model = doc["pppModel"];
        config.ppp_flow_ctrl = doc["pppFlowCtrl"];
        config.ppp_gnss = doc["pppGNSS"];
        config.ppp_napt = doc["pppNAPT"];

        #ifdef MQTT
        #endif


        if(doc["msgEnable"].isNull()){ //old version compatibility
            config.msg_enable = true;
            config.msg_encrypt = false;
            config.msg_rf = true;
            config.msg_inet = true;
            config.msg_retry = 3;
            config.msg_interval = 30;
            config.msg_path = 9;
            sprintf(config.msg_key, "8EC8233E91D59B0164C24E771BA66307");
            sprintf(config.msg_mycall, "NOCALL");
        }else{
            config.msg_enable = doc["msgEnable"];
            config.msg_path = doc["msgPath"];
            config.msg_rf = doc["msgRf"];
            config.msg_inet = doc["msgInet"];
            config.msg_encrypt = doc["msgEncrypt"];
            config.msg_retry = doc["msgRetry"];
            config.msg_interval = doc["msgInterval"];
            strlcpy(config.msg_key, doc["msgAESKey"] | "", sizeof(config.msg_key));
            strlcpy(config.msg_mycall, doc["msgMycall"] | "", sizeof(config.msg_mycall));
        }

        // Bulletins BLN1-BLN9 - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
        for (uint8_t bi = 0; bi < 9; bi++)
        {
            char key[20];
            snprintf(key, sizeof(key), "blnEn%d", bi + 1);
            if (doc[key].isNull())
            {
                config.bln_en[bi] = false;
                config.bln_text[bi][0] = 0;
                config.bln_interval[bi] = 30; // LU6JMF - Marcelo Ferreira: minutes
                config.bln_limit[bi] = 0;
                config.bln_duration[bi] = 72;
                config.bln_started_at[bi] = 0;
                config.bln_sent_total[bi] = 0;
            }
            else
            {
                config.bln_en[bi] = doc[key];
                snprintf(key, sizeof(key), "blnText%d", bi + 1);
                strlcpy(config.bln_text[bi], doc[key] | "", sizeof(config.bln_text[bi]));
                snprintf(key, sizeof(key), "blnInterval%d", bi + 1);
                config.bln_interval[bi] = doc[key] | 30; // LU6JMF - Marcelo Ferreira: minutes

                // LU6JMF - Marcelo Ferreira:
                // One-time compatibility with our earlier BLN implementation,
                // where the interval was stored in seconds.
                // Values above the current UI maximum (1440 min) are legacy seconds.
                // 2.1.7: also NEWS5-NEWS9 (before, only BLN1-BLN4 were converted and NEWS kept e.g. 1800 = old 30 min in seconds)
                if (config.bln_interval[bi] > 1440)
                {
                    config.bln_interval[bi] =
                        (config.bln_interval[bi] + 30) / 60;

                    if (config.bln_interval[bi] == 0)
                        config.bln_interval[bi] = 1;
                }

                snprintf(key, sizeof(key), "blnLimit%d", bi + 1);
                config.bln_limit[bi] = doc[key] | 0;
            }

            // LU6JMF - Marcelo Ferreira:
            // Common BLN1-BLN9 campaign state (replaces bln_activefor).
            snprintf(key, sizeof(key), "blnDuration%d", bi + 1);
            config.bln_duration[bi] = doc[key] | 72;

            // Only the supported calendar lifetimes are accepted.
            if (config.bln_duration[bi] != 24 &&
                config.bln_duration[bi] != 36 &&
                config.bln_duration[bi] != 48 &&
                config.bln_duration[bi] != 72)
            {
                config.bln_duration[bi] = 72;
            }

            snprintf(key, sizeof(key), "blnStarted%d", bi + 1);
            if (!doc[key].isNull())
            {
                config.bln_started_at[bi] = doc[key] | 0UL;
            }
            else
            {
                config.bln_started_at[bi] = 0;
            }

            snprintf(key, sizeof(key), "blnSentTotal%d", bi + 1);
            config.bln_sent_total[bi] = doc[key] | 0;
        }

        // Objects (Object1-4) - custom mod by LU6JMF (Marcelo, CdU/Entre Rios, Argentina) - Set/2026
        // LU6JMF fix (Set/2026): faltava o load pareado do save acima - sem
        // isso o Object voltava sempre pro default (desativado/vazio) em
        // todo boot, mesmo tendo sido salvo certinho no arquivo.
        for (uint8_t oi = 0; oi < 4; oi++)
        {
            char okey[20];
            snprintf(okey, sizeof(okey), "objEn%d", oi + 1);
            if (doc[okey].isNull())
            {
                config.obj_en[oi] = false;
                config.obj_name[oi][0] = 0;
                config.obj_lat[oi] = 0;
                config.obj_lon[oi] = 0;
                strlcpy(config.obj_symbol[oi], "/r", sizeof(config.obj_symbol[oi]));
                config.obj_text[oi][0] = 0;
                config.obj_mode[oi] = 0;
                config.obj_interval[oi] = 900;
                config.obj_limit[oi] = 0;
                config.obj_activefor[oi] = 0;
                config.obj_permanent[oi] = false;
            }
            else
            {
                config.obj_en[oi] = doc[okey];
                snprintf(okey, sizeof(okey), "objName%d", oi + 1);
                strlcpy(config.obj_name[oi], doc[okey] | "", sizeof(config.obj_name[oi]));
                snprintf(okey, sizeof(okey), "objLat%d", oi + 1);
                config.obj_lat[oi] = doc[okey] | 0.0;
                snprintf(okey, sizeof(okey), "objLon%d", oi + 1);
                config.obj_lon[oi] = doc[okey] | 0.0;
                snprintf(okey, sizeof(okey), "objSymbol%d", oi + 1);
                strlcpy(config.obj_symbol[oi], doc[okey] | "/r", sizeof(config.obj_symbol[oi]));
                snprintf(okey, sizeof(okey), "objText%d", oi + 1);
                strlcpy(config.obj_text[oi], doc[okey] | "", sizeof(config.obj_text[oi]));
                snprintf(okey, sizeof(okey), "objMode%d", oi + 1);
                config.obj_mode[oi] = doc[okey] | 0;
                snprintf(okey, sizeof(okey), "objInterval%d", oi + 1);
                config.obj_interval[oi] = doc[okey] | 900;
                snprintf(okey, sizeof(okey), "objLimit%d", oi + 1);
                config.obj_limit[oi] = doc[okey] | 0;
                snprintf(okey, sizeof(okey), "objActiveFor%d", oi + 1);
                config.obj_activefor[oi] = doc[okey] | 0;
                snprintf(okey, sizeof(okey), "objPermanent%d", oi + 1);
                config.obj_permanent[oi] = doc[okey];
            }
        }

        // Close the file (Curiously, File's destructor doesn't close the file)
        // f.close();
        // LU6JMF: status text left by nakhonthai's firmware (his GitHub link) -> AR-APRS text per function
        if (strstr(config.igate_status, "github.com/nakhonthai") != NULL)
            strlcpy(config.igate_status, "AR-APRS iGate - Argentina APRS System", sizeof(config.igate_status));
        if (strstr(config.digi_status, "github.com/nakhonthai") != NULL)
            strlcpy(config.digi_status, "AR-APRS Digi - Argentina APRS System", sizeof(config.digi_status));
        if (strstr(config.trk_status, "github.com/nakhonthai") != NULL)
            strlcpy(config.trk_status, "AR-APRS Tracker - Argentina APRS System", sizeof(config.trk_status));
        return true;
    }
    else
    {
        log_d("Can't load %s file.", filename);
    }
    return false;
}