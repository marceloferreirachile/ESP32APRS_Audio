#include <Arduino.h>
#include "handleATCommand.h"
#include "config.h"
#include <WiFi.h>

extern Configuration config;

// LU6JMF 2.1.9: the plain "read / write one config field" AT commands (AT+NAME? / AT+NAME=value)
// are a table instead of ~190 copies of the same code (same answers as before: value, "OK",
// bools only =1 / =0, text without quotes). Saves flash; every other command is unchanged below.
enum : uint8_t { AT_B, AT_S, AT_U8, AT_I8, AT_U16, AT_I16, AT_U32, AT_I32, AT_F };
struct AtField { const char *name; uint8_t type; void *ptr; uint16_t size; };
static const AtField AT_FIELDS[] = {
    {"TIMEZONE", AT_F, &config.timeZone, sizeof(config.timeZone)},
    {"SYNCTIME", AT_B, &config.synctime, sizeof(config.synctime)},
    {"TITLE", AT_B, &config.title, sizeof(config.title)},
    {"WIFI_MODE", AT_U8, &config.wifi_mode, sizeof(config.wifi_mode)},
    {"WIFI_POWER", AT_I8, &config.wifi_power, sizeof(config.wifi_power)},
    {"WIFI_AP_CH", AT_U8, &config.wifi_ap_ch, sizeof(config.wifi_ap_ch)},
    {"WIFI_AP_SSID", AT_S, &config.wifi_ap_ssid, sizeof(config.wifi_ap_ssid)},
    {"WIFI_AP_PASS", AT_S, &config.wifi_ap_pass, sizeof(config.wifi_ap_pass)},
    {"BT_SLAVE", AT_B, &config.bt_slave, sizeof(config.bt_slave)},
    {"BT_MASTER", AT_B, &config.bt_master, sizeof(config.bt_master)},
    {"BT_MODE", AT_U8, &config.bt_mode, sizeof(config.bt_mode)},
    {"BT_UUID", AT_S, &config.bt_uuid, sizeof(config.bt_uuid)},
    {"BT_UUID_RX", AT_S, &config.bt_uuid_rx, sizeof(config.bt_uuid_rx)},
    {"BT_UUID_TX", AT_S, &config.bt_uuid_tx, sizeof(config.bt_uuid_tx)},
    {"BT_NAME", AT_S, &config.bt_name, sizeof(config.bt_name)},
    {"BT_PIN", AT_U32, &config.bt_pin, sizeof(config.bt_pin)},
    {"BT_POWER", AT_U8, &config.bt_power, sizeof(config.bt_power)},
    {"RF_EN", AT_B, &config.rf_en, sizeof(config.rf_en)},
    {"RF_TYPE", AT_U8, &config.rf_type, sizeof(config.rf_type)},
    {"FREQ_RX", AT_F, &config.freq_rx, sizeof(config.freq_rx)},
    {"FREQ_TX", AT_F, &config.freq_tx, sizeof(config.freq_tx)},
    {"OFFSET_RX", AT_I32, &config.offset_rx, sizeof(config.offset_rx)},
    {"OFFSET_TX", AT_I32, &config.offset_tx, sizeof(config.offset_tx)},
    {"TONE_RX", AT_I32, &config.tone_rx, sizeof(config.tone_rx)},
    {"TONE_TX", AT_I32, &config.tone_tx, sizeof(config.tone_tx)},
    {"BAND", AT_U8, &config.band, sizeof(config.band)},
    {"SQL_LEVEL", AT_U8, &config.sql_level, sizeof(config.sql_level)},
    {"RF_POWER", AT_B, &config.rf_power, sizeof(config.rf_power)},
    {"VOLUME", AT_U8, &config.volume, sizeof(config.volume)},
    {"MIC", AT_U8, &config.mic, sizeof(config.mic)},
    {"IGATE_EN", AT_B, &config.igate_en, sizeof(config.igate_en)},
    {"RF2INET", AT_B, &config.rf2inet, sizeof(config.rf2inet)},
    {"INET2RF", AT_B, &config.inet2rf, sizeof(config.inet2rf)},
    {"IGATE_LOC2RF", AT_B, &config.igate_loc2rf, sizeof(config.igate_loc2rf)},
    {"IGATE_LOC2INET", AT_B, &config.igate_loc2inet, sizeof(config.igate_loc2inet)},
    {"RF2INETFILTER", AT_U16, &config.rf2inetFilter, sizeof(config.rf2inetFilter)},
    {"INET2RFFILTER", AT_U16, &config.inet2rfFilter, sizeof(config.inet2rfFilter)},
    {"APRS_SSID", AT_U8, &config.aprs_ssid, sizeof(config.aprs_ssid)},
    {"APRS_PORT", AT_U16, &config.aprs_port, sizeof(config.aprs_port)},
    {"APRS_MYCALL", AT_S, &config.aprs_mycall, sizeof(config.aprs_mycall)},
    {"APRS_HOST", AT_S, &config.aprs_host, sizeof(config.aprs_host)},
    {"APRS_PASSCODE", AT_S, &config.aprs_passcode, sizeof(config.aprs_passcode)},
    {"APRS_MONICALL", AT_S, &config.aprs_moniCall, sizeof(config.aprs_moniCall)},
    {"APRS_FILTER", AT_S, &config.aprs_filter, sizeof(config.aprs_filter)},
    {"IGATE_BCN", AT_B, &config.igate_bcn, sizeof(config.igate_bcn)},
    {"IGATE_GPS", AT_B, &config.igate_gps, sizeof(config.igate_gps)},
    {"IGATE_TIMESTAMP", AT_B, &config.igate_timestamp, sizeof(config.igate_timestamp)},
    {"IGATE_LAT", AT_F, &config.igate_lat, sizeof(config.igate_lat)},
    {"IGATE_LON", AT_F, &config.igate_lon, sizeof(config.igate_lon)},
    {"IGATE_ALT", AT_F, &config.igate_alt, sizeof(config.igate_alt)},
    {"IGATE_INTERVAL", AT_U16, &config.igate_interval, sizeof(config.igate_interval)},
    {"IGATE_OBJECT", AT_S, &config.igate_object, sizeof(config.igate_object)},
    {"IGATE_PHG", AT_S, &config.igate_phg, sizeof(config.igate_phg)},
    {"IGATE_PATH", AT_U8, &config.igate_path, sizeof(config.igate_path)},
    {"IGATE_COMMENT", AT_S, &config.igate_comment, sizeof(config.igate_comment)},
    {"IGATE_STS_INTERVAL", AT_U16, &config.igate_sts_interval, sizeof(config.igate_sts_interval)},
    {"IGATE_STATUS", AT_S, &config.igate_status, sizeof(config.igate_status)},
    {"DIGI_EN", AT_B, &config.digi_en, sizeof(config.digi_en)},
    {"DIGI_AUTO", AT_B, &config.digi_auto, sizeof(config.digi_auto)},
    {"DIGI_LOC2RF", AT_B, &config.digi_loc2rf, sizeof(config.digi_loc2rf)},
    {"DIGI_LOC2INET", AT_B, &config.digi_loc2inet, sizeof(config.digi_loc2inet)},
    {"DIGI_TIMESTAMP", AT_B, &config.digi_timestamp, sizeof(config.digi_timestamp)},
    {"DIGI_SSID", AT_U8, &config.digi_ssid, sizeof(config.digi_ssid)},
    {"DIGI_MYCALL", AT_S, &config.digi_mycall, sizeof(config.digi_mycall)},
    {"DIGI_PATH", AT_U8, &config.digi_path, sizeof(config.digi_path)},
    {"DIGI_DELAY", AT_U16, &config.digi_delay, sizeof(config.digi_delay)},
    {"DIGIFILTER", AT_U16, &config.digiFilter, sizeof(config.digiFilter)},
    {"DIGI_BCN", AT_B, &config.digi_bcn, sizeof(config.digi_bcn)},
    {"DIGI_GPS", AT_B, &config.digi_gps, sizeof(config.digi_gps)},
    {"DIGI_LAT", AT_F, &config.digi_lat, sizeof(config.digi_lat)},
    {"DIGI_LON", AT_F, &config.digi_lon, sizeof(config.digi_lon)},
    {"DIGI_ALT", AT_F, &config.digi_alt, sizeof(config.digi_alt)},
    {"DIGI_INTERVAL", AT_U16, &config.digi_interval, sizeof(config.digi_interval)},
    {"DIGI_PHG", AT_S, &config.digi_phg, sizeof(config.digi_phg)},
    {"DIGI_COMMENT", AT_S, &config.digi_comment, sizeof(config.digi_comment)},
    {"DIGI_STS_INTERVAL", AT_U16, &config.digi_sts_interval, sizeof(config.digi_sts_interval)},
    {"DIGI_STATUS", AT_S, &config.digi_status, sizeof(config.digi_status)},
    {"TRK_EN", AT_B, &config.trk_en, sizeof(config.trk_en)},
    {"TRK_LOC2RF", AT_B, &config.trk_loc2rf, sizeof(config.trk_loc2rf)},
    {"TRK_LOC2INET", AT_B, &config.trk_loc2inet, sizeof(config.trk_loc2inet)},
    {"TRK_TIMESTAMP", AT_B, &config.trk_timestamp, sizeof(config.trk_timestamp)},
    {"TRK_SSID", AT_U8, &config.trk_ssid, sizeof(config.trk_ssid)},
    {"TRK_MYCALL", AT_S, &config.trk_mycall, sizeof(config.trk_mycall)},
    {"TRK_PATH", AT_U8, &config.trk_path, sizeof(config.trk_path)},
    {"TRK_GPS", AT_B, &config.trk_gps, sizeof(config.trk_gps)},
    {"TRK_LAT", AT_F, &config.trk_lat, sizeof(config.trk_lat)},
    {"TRK_LON", AT_F, &config.trk_lon, sizeof(config.trk_lon)},
    {"TRK_ALT", AT_F, &config.trk_alt, sizeof(config.trk_alt)},
    {"TRK_COMMENT", AT_S, &config.trk_comment, sizeof(config.trk_comment)},
    {"TRK_STS_INTERVAL", AT_U16, &config.trk_sts_interval, sizeof(config.trk_sts_interval)},
    {"TRK_STATUS", AT_S, &config.trk_status, sizeof(config.trk_status)},
    {"WX_EN", AT_B, &config.wx_en, sizeof(config.wx_en)},
    {"WX_2RF", AT_B, &config.wx_2rf, sizeof(config.wx_2rf)},
    {"WX_2INET", AT_B, &config.wx_2inet, sizeof(config.wx_2inet)},
    {"WX_TIMESTAMP", AT_B, &config.wx_timestamp, sizeof(config.wx_timestamp)},
    {"WX_SSID", AT_U8, &config.wx_ssid, sizeof(config.wx_ssid)},
    {"WX_MYCALL", AT_S, &config.wx_mycall, sizeof(config.wx_mycall)},
    {"WX_PATH", AT_U8, &config.wx_path, sizeof(config.wx_path)},
    {"WX_GPS", AT_B, &config.wx_gps, sizeof(config.wx_gps)},
    {"WX_LAT", AT_F, &config.wx_lat, sizeof(config.wx_lat)},
    {"WX_LON", AT_F, &config.wx_lon, sizeof(config.wx_lon)},
    {"WX_ALT", AT_F, &config.wx_alt, sizeof(config.wx_alt)},
    {"WX_INTERVAL", AT_U16, &config.wx_interval, sizeof(config.wx_interval)},
    {"WX_FLAGE", AT_U32, &config.wx_flage, sizeof(config.wx_flage)},
    {"WX_OBJECT", AT_S, &config.wx_object, sizeof(config.wx_object)},
    {"WX_COMMENT", AT_S, &config.wx_comment, sizeof(config.wx_comment)},
    {"TLM0_EN", AT_B, &config.tlm0_en, sizeof(config.tlm0_en)},
    {"TLM0_2RF", AT_B, &config.tlm0_2rf, sizeof(config.tlm0_2rf)},
    {"TLM0_2INET", AT_B, &config.tlm0_2inet, sizeof(config.tlm0_2inet)},
    {"TLM0_SSID", AT_U8, &config.tlm0_ssid, sizeof(config.tlm0_ssid)},
    {"TLM0_MYCALL", AT_S, &config.tlm0_mycall, sizeof(config.tlm0_mycall)},
    {"TLM0_PATH", AT_U8, &config.tlm0_path, sizeof(config.tlm0_path)},
    {"TLM0_DATA_INTERVAL", AT_U16, &config.tlm0_data_interval, sizeof(config.tlm0_data_interval)},
    {"TLM0_INFO_INTERVAL", AT_U16, &config.tlm0_info_interval, sizeof(config.tlm0_info_interval)},
    {"TLM0_BITS_ACTIVE", AT_U8, &config.tlm0_BITS_Active, sizeof(config.tlm0_BITS_Active)},
    {"TLM0_COMMENT", AT_S, &config.tlm0_comment, sizeof(config.tlm0_comment)},
    {"TLM1_EN", AT_B, &config.tlm1_en, sizeof(config.tlm1_en)},
    {"TLM1_2RF", AT_B, &config.tlm1_2rf, sizeof(config.tlm1_2rf)},
    {"TLM1_2INET", AT_B, &config.tlm1_2inet, sizeof(config.tlm1_2inet)},
    {"TLM1_SSID", AT_U8, &config.tlm1_ssid, sizeof(config.tlm1_ssid)},
    {"TLM1_MYCALL", AT_S, &config.tlm1_mycall, sizeof(config.tlm1_mycall)},
    {"TLM1_PATH", AT_U8, &config.tlm1_path, sizeof(config.tlm1_path)},
    {"TLM1_DATA_INTERVAL", AT_U16, &config.tlm1_data_interval, sizeof(config.tlm1_data_interval)},
    {"TLM1_INFO_INTERVAL", AT_U16, &config.tlm1_info_interval, sizeof(config.tlm1_info_interval)},
    {"TLM1_BITS_ACTIVE", AT_U8, &config.tlm1_BITS_Active, sizeof(config.tlm1_BITS_Active)},
    {"TLM1_COMMENT", AT_S, &config.tlm1_comment, sizeof(config.tlm1_comment)},
    {"OLED_ENABLE", AT_B, &config.oled_enable, sizeof(config.oled_enable)},
    {"OLED_TIMEOUT", AT_I32, &config.oled_timeout, sizeof(config.oled_timeout)},
    {"DIM", AT_U8, &config.dim, sizeof(config.dim)},
    {"CONTRAST", AT_U8, &config.contrast, sizeof(config.contrast)},
    {"STARTUP", AT_U8, &config.startup, sizeof(config.startup)},
    {"DISPFILTER", AT_U16, &config.dispFilter, sizeof(config.dispFilter)},
    {"DISPRF", AT_B, &config.dispRF, sizeof(config.dispRF)},
    {"DISPINET", AT_B, &config.dispINET, sizeof(config.dispINET)},
    {"AUDIO_HPF", AT_B, &config.audio_hpf, sizeof(config.audio_hpf)},
    {"AUDIO_LPF", AT_B, &config.audio_lpf, sizeof(config.audio_lpf)},
    {"PREAMBLE", AT_U8, &config.preamble, sizeof(config.preamble)},
    {"MODEM_TYPE", AT_U8, &config.modem_type, sizeof(config.modem_type)},
    {"FX25_MODE", AT_U8, &config.fx25_mode, sizeof(config.fx25_mode)},
    {"TX_TIMESLOT", AT_U16, &config.tx_timeslot, sizeof(config.tx_timeslot)},
    {"NTP_HOST", AT_S, &config.ntp_host, sizeof(config.ntp_host)},
    {"VPN", AT_B, &config.vpn, sizeof(config.vpn)},
    {"MODEM", AT_B, &config.modem, sizeof(config.modem)},
    {"WG_PORT", AT_U16, &config.wg_port, sizeof(config.wg_port)},
    {"WG_PEER_ADDRESS", AT_S, &config.wg_peer_address, sizeof(config.wg_peer_address)},
    {"WG_LOCAL_ADDRESS", AT_S, &config.wg_local_address, sizeof(config.wg_local_address)},
    {"WG_NETMASK_ADDRESS", AT_S, &config.wg_netmask_address, sizeof(config.wg_netmask_address)},
    {"WG_GW_ADDRESS", AT_S, &config.wg_gw_address, sizeof(config.wg_gw_address)},
    {"WG_PUBLIC_KEY", AT_S, &config.wg_public_key, sizeof(config.wg_public_key)},
    {"WG_PRIVATE_KEY", AT_S, &config.wg_private_key, sizeof(config.wg_private_key)},
    {"HTTP_USERNAME", AT_S, &config.http_username, sizeof(config.http_username)},
    {"HTTP_PASSWORD", AT_S, &config.http_password, sizeof(config.http_password)},
    {"GNSS_ENABLE", AT_B, &config.gnss_enable, sizeof(config.gnss_enable)},
    {"GNSS_TCP_PORT", AT_U16, &config.gnss_tcp_port, sizeof(config.gnss_tcp_port)},
    {"GNSS_TCP_HOST", AT_S, &config.gnss_tcp_host, sizeof(config.gnss_tcp_host)},
    {"GNSS_AT_COMMAND", AT_S, &config.gnss_at_command, sizeof(config.gnss_at_command)},
    {"ADC_DC_OFFSET", AT_U16, &config.adc_dc_offset, sizeof(config.adc_dc_offset)},
    {"I2C_ENABLE", AT_B, &config.i2c_enable, sizeof(config.i2c_enable)},
    {"I2C1_ENABLE", AT_B, &config.i2c1_enable, sizeof(config.i2c1_enable)},
    {"DISP_FLIP", AT_B, &config.disp_flip, sizeof(config.disp_flip)},
    {"DISP_BRIGHTNESS", AT_U8, &config.disp_brightness, sizeof(config.disp_brightness)},
    {"PPP_APN", AT_S, &config.ppp_apn, sizeof(config.ppp_apn)},
    {"PPP_PIN", AT_S, &config.ppp_pin, sizeof(config.ppp_pin)},
#ifdef MQTT
    {"EN_MQTT", AT_B, &config.en_mqtt, sizeof(config.en_mqtt)},
    {"MQTT_HOST", AT_S, &config.mqtt_host, sizeof(config.mqtt_host)},
    {"MQTT_TOPIC", AT_S, &config.mqtt_topic, sizeof(config.mqtt_topic)},
    {"MQTT_SUBSCRIBE", AT_S, &config.mqtt_subscribe, sizeof(config.mqtt_subscribe)},
    {"MQTT_USER", AT_S, &config.mqtt_user, sizeof(config.mqtt_user)},
    {"MQTT_PASS", AT_S, &config.mqtt_pass, sizeof(config.mqtt_pass)},
    {"MQTT_PORT", AT_U16, &config.mqtt_port, sizeof(config.mqtt_port)},
    {"MQTT_TOPIC_FLAG", AT_U16, &config.mqtt_topic_flag, sizeof(config.mqtt_topic_flag)},
    {"MQTT_SUBSCRIBE_FLAG", AT_U16, &config.mqtt_subscribe_flag, sizeof(config.mqtt_subscribe_flag)},
#endif
    {"TRK_MICE_TYPE", AT_U8, &config.trk_mice_type, sizeof(config.trk_mice_type)},
    {"TRK_TLM_INTERVAL", AT_U8, &config.trk_tlm_interval, sizeof(config.trk_tlm_interval)},
    {"DIGI_TLM_INTERVAL", AT_U8, &config.digi_tlm_interval, sizeof(config.digi_tlm_interval)},
    {"IGATE_TLM_INTERVAL", AT_U8, &config.igate_tlm_interval, sizeof(config.igate_tlm_interval)},
    {"WX_TLM_INTERVAL", AT_U8, &config.wx_tlm_interval, sizeof(config.wx_tlm_interval)},
    {"HOST_NAME", AT_S, &config.host_name, sizeof(config.host_name)},
    {"RESET_TIMEOUT", AT_U16, &config.reset_timeout, sizeof(config.reset_timeout)},
    {"MSG_ENABLE", AT_B, &config.msg_enable, sizeof(config.msg_enable)},
    {"MSG_MYCALL", AT_S, &config.msg_mycall, sizeof(config.msg_mycall)},
    {"MSG_PATH", AT_U8, &config.msg_path, sizeof(config.msg_path)},
    {"MSG_RF", AT_B, &config.msg_rf, sizeof(config.msg_rf)},
    {"MSG_INET", AT_B, &config.msg_inet, sizeof(config.msg_inet)},
    {"MSG_ENCRYPT", AT_B, &config.msg_encrypt, sizeof(config.msg_encrypt)},
    {"MSG_KEY", AT_S, &config.msg_key, sizeof(config.msg_key)},
    {"MSG_RETRY", AT_U8, &config.msg_retry, sizeof(config.msg_retry)},
    {"MSG_INTERVAL", AT_U16, &config.msg_interval, sizeof(config.msg_interval)},
};

static bool atFieldCommand(const String &cmd, String &reply)
{
    if (!cmd.startsWith("AT+"))
        return false;
    const char *c = cmd.c_str() + 3;
    size_t rest = cmd.length() - 3;
    for (const AtField &e : AT_FIELDS)
    {
        size_t nl = strlen(e.name);
        if (rest <= nl || strncmp(c, e.name, nl) != 0)
            continue;
        char op = c[nl];
        if (op == '?' && rest == nl + 1)
        {
            switch (e.type)
            {
            case AT_B: reply = String(*(bool *)e.ptr ? "1" : "0"); break;
            case AT_S: reply = String((const char *)e.ptr); break;
            case AT_U8: reply = String(*(uint8_t *)e.ptr); break;
            case AT_I8: reply = String((int)*(int8_t *)e.ptr); break;
            case AT_U16: reply = String(*(uint16_t *)e.ptr); break;
            case AT_I16: reply = String(*(int16_t *)e.ptr); break;
            case AT_U32: reply = String(*(uint32_t *)e.ptr); break;
            case AT_I32: reply = String(*(int32_t *)e.ptr); break;
            case AT_F: reply = String(*(float *)e.ptr, 6); break;
            }
            return true;
        }
        if (op != '=')
            continue;
        String v = cmd.substring(3 + nl + 1);
        switch (e.type)
        {
        case AT_B:
            if (v == "1")
                *(bool *)e.ptr = true;
            else if (v == "0")
                *(bool *)e.ptr = false;
            else
                return false; // as before: other values fall through to the rest of the commands
            break;
        case AT_S:
            v.replace("\"", "");
            strncpy((char *)e.ptr, v.c_str(), e.size);
            break;
        case AT_U8: *(uint8_t *)e.ptr = v.toInt(); break;
        case AT_I8: *(int8_t *)e.ptr = v.toInt(); break;
        case AT_U16: *(uint16_t *)e.ptr = v.toInt(); break;
        case AT_I16: *(int16_t *)e.ptr = v.toInt(); break;
        case AT_U32: *(uint32_t *)e.ptr = v.toInt(); break;
        case AT_I32: *(int32_t *)e.ptr = v.toInt(); break;
        case AT_F: *(float *)e.ptr = v.toFloat(); break;
        }
        reply = "OK";
        return true;
    }
    return false;
}

String handleATCommand(String cmd)
{
    cmd.trim();
    if (!cmd.startsWith("AT"))
        return "";
    if (cmd == "AT")
        return "OK";

    {
        String reply;
        if (atFieldCommand(cmd, reply)) // 2.1.9: table of the plain field commands
            return reply;
    }

    if (cmd == "AT+RESET" || cmd == "AT+RESTART")
    {
        log_d("CMD Reset System");
        delay(3000);
        esp_restart();
    }

    // if(cmd == "AT+VERSION?")
    // {
    //     String ver = "Firmware Version: " + String(FIRMWARE_VERSION);
    //     ver += ", SDK Version: " + String(ESP.getSdkVersion());
    //     ver += ", Core Version: " + String(ARDUINO_ESP32_RELEASE);
    //     return ver;
    // }

    if (cmd == "AT+CHIPID?")
    {
        uint64_t chipid = ESP.getEfuseMac();
        String id = String((uint16_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
        id.toUpperCase();
        return id;
    }

    if (cmd == "AT+SAVECONFIG")
    {
        if (saveConfiguration("/default.cfg", config))
        {
            return "Configuration Saved";
        }
        else
        {
            return "Failed to Save Configuration";
        }
    }

    if (cmd == "LOADCONFIG")
    {
        if (loadConfiguration("/default.cfg", config))
        {
            return "Configuration Loaded";
        }
        else
        {
            return "Failed to Load Configuration";
        }
    }

    if (cmd == "AT+WIFI_DISCONNECT")
    {
        WiFi.disconnect(true);
        return "WiFi Disconnected";
    }

    if (cmd == "AT+WIFI_CONNECT")
    {
        WiFi.reconnect();
        return "WiFi Reconnecting";
    }

    if (cmd == "AT+WIFI_STATUS?")
    {
        String status = "WiFi Status: ";
        if (WiFi.status() == WL_CONNECTED)
        {
            status += "Connected";
        }
        else
        {
            status += "Disconnected";
        }
        return status;
    }

    if (cmd == "AT+WIFI_SCAN")
    {
        int16_t n = WiFi.scanNetworks();
        String scanResult = "Scan Complete: " + String(n) + " networks found.\n";
        for (int i = 0; i < n; ++i)
        {
            scanResult += String(i + 1) + ": ";
            scanResult += WiFi.SSID(i);
            scanResult += ", RSSI: " + String(WiFi.RSSI(i)) + "dBm";
            scanResult += ", BSSID: " + WiFi.BSSIDstr(i);
            scanResult += ", Channel: " + String(WiFi.channel(i));
            scanResult += ", Encryption: " + String((int)WiFi.encryptionType(i)) + "\n";
        }
        WiFi.scanDelete();
        return scanResult;
    }

    if (cmd == "AT+WIFI?")
    {
        String mode = "";
        if (config.wifi_mode == WIFI_AP)
            mode = "AP";
        else if (config.wifi_mode == WIFI_STA)
            mode = "STA";
        else if (config.wifi_mode == WIFI_AP_STA)
            mode = "AP+STA";
        else
            mode = "OFF";
        mode = "Mode:" + mode + ",SSID:" + String(WiFi.SSID()) + ",RSSI:" + String(WiFi.RSSI()) + "dBm";
        return mode;
    }

    if (cmd == "AT+MODE?")
    {
        String mode = "MODE: ";
        if (config.igate_en)
            mode += "IGATE";
        if (config.digi_en)
            mode += ",DIGI";
        if (config.trk_en)
            mode += ",TRK";
        if (config.wx_en)
            mode += ",WX";
        if (config.tlm0_en)
            mode += ",TLM0";
        return mode;
    }

    if (cmd == "AT+TIME?")
    {
        struct tm tmstruct;
        char strTime[20];
        tmstruct.tm_year = 0;
        getLocalTime(&tmstruct, 100);
        sprintf(strTime, "%d-%02d-%02d %02d:%02d:%02d", (tmstruct.tm_year) + 1900, (tmstruct.tm_mon) + 1, tmstruct.tm_mday, tmstruct.tm_hour, tmstruct.tm_min, tmstruct.tm_sec);
        return String(strTime);
    }
    else if (cmd.startsWith("AT+TIME="))
    {
        cmd.remove(0, 8);
        cmd.replace("\"", "");
        char strTime[20];
        strncpy(strTime, cmd.c_str(), sizeof(strTime));
        String date = getValue(strTime, ' ', 0);
        String time = getValue(strTime, ' ', 1);
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
        time_t rtc = timeStamp - (config.timeZone * 3600);
        timeval tv = {rtc, 0};
        timezone tz = {(0) + DST_MN, 0};
        settimeofday(&tv, &tz);
        log_d("Set Time: %s", strTime);
        return "Set Time: " + String(strTime);
    }

    // Generate AT-Command from script python

    // ---- WiFi STA 0 ----
    if (cmd == "AT+WIFI0EN?")
        return String(config.wifi_sta[0].enable ? "1" : "0");
    else if (cmd == "AT+WIFI0EN=1")
    {
        config.wifi_sta[0].enable = true;
        return "OK";
    }
    else if (cmd == "AT+WIFI0EN=0")
    {
        config.wifi_sta[0].enable = false;
        return "OK";
    }
    else if (cmd == "AT+WIFI0SSID?")
        return String(config.wifi_sta[0].wifi_ssid);
    else if (cmd.startsWith("AT+WIFI0SSID="))
    {
        cmd.remove(0, 13 + String(0).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[0].wifi_ssid, cmd.c_str(), sizeof(config.wifi_sta[0].wifi_ssid));
        return "OK";
    }
    else if (cmd == "AT+WIFI0PASS?")
        return String(config.wifi_sta[0].wifi_pass);
    else if (cmd.startsWith("AT+WIFI0PASS="))
    {
        cmd.remove(0, 13 + String(0).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[0].wifi_pass, cmd.c_str(), sizeof(config.wifi_sta[0].wifi_pass));
        return "OK";
    }

    // ---- WiFi STA 1 ----
    if (cmd == "AT+WIFI1EN?")
        return String(config.wifi_sta[1].enable ? "1" : "0");
    else if (cmd == "AT+WIFI1EN=1")
    {
        config.wifi_sta[1].enable = true;
        return "OK";
    }
    else if (cmd == "AT+WIFI1EN=0")
    {
        config.wifi_sta[1].enable = false;
        return "OK";
    }
    else if (cmd == "AT+WIFI1SSID?")
        return String(config.wifi_sta[1].wifi_ssid);
    else if (cmd.startsWith("AT+WIFI1SSID="))
    {
        cmd.remove(0, 13 + String(1).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[1].wifi_ssid, cmd.c_str(), sizeof(config.wifi_sta[1].wifi_ssid));
        return "OK";
    }
    else if (cmd == "AT+WIFI1PASS?")
        return String(config.wifi_sta[1].wifi_pass);
    else if (cmd.startsWith("AT+WIFI1PASS="))
    {
        cmd.remove(0, 13 + String(1).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[1].wifi_pass, cmd.c_str(), sizeof(config.wifi_sta[1].wifi_pass));
        return "OK";
    }

    // ---- WiFi STA 2 ----
    if (cmd == "AT+WIFI2EN?")
        return String(config.wifi_sta[2].enable ? "1" : "0");
    else if (cmd == "AT+WIFI2EN=1")
    {
        config.wifi_sta[2].enable = true;
        return "OK";
    }
    else if (cmd == "AT+WIFI2EN=0")
    {
        config.wifi_sta[2].enable = false;
        return "OK";
    }
    else if (cmd == "AT+WIFI2SSID?")
        return String(config.wifi_sta[2].wifi_ssid);
    else if (cmd.startsWith("AT+WIFI2SSID="))
    {
        cmd.remove(0, 13 + String(2).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[2].wifi_ssid, cmd.c_str(), sizeof(config.wifi_sta[2].wifi_ssid));
        return "OK";
    }
    else if (cmd == "AT+WIFI2PASS?")
        return String(config.wifi_sta[2].wifi_pass);
    else if (cmd.startsWith("AT+WIFI2PASS="))
    {
        cmd.remove(0, 13 + String(2).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[2].wifi_pass, cmd.c_str(), sizeof(config.wifi_sta[2].wifi_pass));
        return "OK";
    }

    // ---- WiFi STA 3 ----
    if (cmd == "AT+WIFI3EN?")
        return String(config.wifi_sta[3].enable ? "1" : "0");
    else if (cmd == "AT+WIFI3EN=1")
    {
        config.wifi_sta[3].enable = true;
        return "OK";
    }
    else if (cmd == "AT+WIFI3EN=0")
    {
        config.wifi_sta[3].enable = false;
        return "OK";
    }
    else if (cmd == "AT+WIFI3SSID?")
        return String(config.wifi_sta[3].wifi_ssid);
    else if (cmd.startsWith("AT+WIFI3SSID="))
    {
        cmd.remove(0, 13 + String(3).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[3].wifi_ssid, cmd.c_str(), sizeof(config.wifi_sta[3].wifi_ssid));
        return "OK";
    }
    else if (cmd == "AT+WIFI3PASS?")
        return String(config.wifi_sta[3].wifi_pass);
    else if (cmd.startsWith("AT+WIFI3PASS="))
    {
        cmd.remove(0, 13 + String(3).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[3].wifi_pass, cmd.c_str(), sizeof(config.wifi_sta[3].wifi_pass));
        return "OK";
    }

    // ---- WiFi STA 4 ----
    if (cmd == "AT+WIFI4EN?")
        return String(config.wifi_sta[4].enable ? "1" : "0");
    else if (cmd == "AT+WIFI4EN=1")
    {
        config.wifi_sta[4].enable = true;
        return "OK";
    }
    else if (cmd == "AT+WIFI4EN=0")
    {
        config.wifi_sta[4].enable = false;
        return "OK";
    }
    else if (cmd == "AT+WIFI4SSID?")
        return String(config.wifi_sta[4].wifi_ssid);
    else if (cmd.startsWith("AT+WIFI4SSID="))
    {
        cmd.remove(0, 13 + String(4).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[4].wifi_ssid, cmd.c_str(), sizeof(config.wifi_sta[4].wifi_ssid));
        return "OK";
    }
    else if (cmd == "AT+WIFI4PASS?")
        return String(config.wifi_sta[4].wifi_pass);
    else if (cmd.startsWith("AT+WIFI4PASS="))
    {
        cmd.remove(0, 13 + String(4).length());
        cmd.replace("\"", "");
        strncpy(config.wifi_sta[4].wifi_pass, cmd.c_str(), sizeof(config.wifi_sta[4].wifi_pass));
        return "OK";
    }

    if (cmd == "AT+IGATE_SYMBOL?")
        return String(config.igate_symbol);
    else if (cmd.startsWith("AT+IGATE_SYMBOL="))
    {
        cmd.remove(0, 16);
        cmd.replace("\"", "");
        strncpy(config.igate_symbol, cmd.c_str(), sizeof(config.igate_symbol));
        return "OK";
    }

    if (cmd == "AT+DIGI_SYMBOL?")
        return String(config.digi_symbol);
    else if (cmd.startsWith("AT+DIGI_SYMBOL="))
    {
        cmd.remove(0, 15);
        cmd.replace("\"", "");
        strncpy(config.digi_symbol, cmd.c_str(), sizeof(config.digi_symbol));
        return "OK";
    }

    if (cmd == "AT+TRK_INTERVAL?")
        return String(config.trk_interval);
    else if (cmd.startsWith("AT+TRK_INTERVAL="))
    {
        config.trk_interval = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_SMARTBEACON?")
        return String(config.trk_smartbeacon ? "1" : "0");
    else if (cmd == "AT+TRK_SMARTBEACON=1")
    {
        config.trk_smartbeacon = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_SMARTBEACON=0")
    {
        config.trk_smartbeacon = false;
        return "OK";
    }

    if (cmd == "AT+TRK_COMPRESS?")
        return String(config.trk_compress ? "1" : "0");
    else if (cmd == "AT+TRK_COMPRESS=1")
    {
        config.trk_compress = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_COMPRESS=0")
    {
        config.trk_compress = false;
        return "OK";
    }

    if (cmd == "AT+TRK_ALTITUDE?")
        return String(config.trk_altitude ? "1" : "0");
    else if (cmd == "AT+TRK_ALTITUDE=1")
    {
        config.trk_altitude = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_ALTITUDE=0")
    {
        config.trk_altitude = false;
        return "OK";
    }

    if (cmd == "AT+TRK_LOG?")
        return String(config.trk_log ? "1" : "0");
    else if (cmd == "AT+TRK_LOG=1")
    {
        config.trk_log = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_LOG=0")
    {
        config.trk_log = false;
        return "OK";
    }

    if (cmd == "AT+TRK_RSSI?")
        return String(config.trk_rssi ? "1" : "0");
    else if (cmd == "AT+TRK_RSSI=1")
    {
        config.trk_rssi = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_RSSI=0")
    {
        config.trk_rssi = false;
        return "OK";
    }

    if (cmd == "AT+TRK_SAT?")
        return String(config.trk_sat ? "1" : "0");
    else if (cmd == "AT+TRK_SAT=1")
    {
        config.trk_sat = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_SAT=0")
    {
        config.trk_sat = false;
        return "OK";
    }

    if (cmd == "AT+TRK_DX?")
        return String(config.trk_dx ? "1" : "0");
    else if (cmd == "AT+TRK_DX=1")
    {
        config.trk_dx = true;
        return "OK";
    }
    else if (cmd == "AT+TRK_DX=0")
    {
        config.trk_dx = false;
        return "OK";
    }

    if (cmd == "AT+TRK_HSPEED?")
        return String(config.trk_hspeed);
    else if (cmd.startsWith("AT+TRK_HSPEED="))
    {
        config.trk_hspeed = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_LSPEED?")
        return String(config.trk_lspeed);
    else if (cmd.startsWith("AT+TRK_LSPEED="))
    {
        config.trk_lspeed = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_MAXINTERVAL?")
        return String(config.trk_maxinterval);
    else if (cmd.startsWith("AT+TRK_MAXINTERVAL="))
    {
        config.trk_maxinterval = cmd.substring(19).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_MININTERVAL?")
        return String(config.trk_mininterval);
    else if (cmd.startsWith("AT+TRK_MININTERVAL="))
    {
        config.trk_mininterval = cmd.substring(19).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_MINANGLE?")
        return String(config.trk_minangle);
    else if (cmd.startsWith("AT+TRK_MINANGLE="))
    {
        config.trk_minangle = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_SLOWINTERVAL?")
        return String(config.trk_slowinterval);
    else if (cmd.startsWith("AT+TRK_SLOWINTERVAL="))
    {
        config.trk_slowinterval = cmd.substring(20).toInt();
        return "OK";
    }

    if (cmd == "AT+TRK_SYMBOL?")
        return String(config.trk_symbol);
    else if (cmd.startsWith("AT+TRK_SYMBOL="))
    {
        cmd.remove(0, 14);
        cmd.replace("\"", "");
        strncpy(config.trk_symbol, cmd.c_str(), sizeof(config.trk_symbol));
        return "OK";
    }

    if (cmd == "AT+TRK_SYMMOVE?")
        return String(config.trk_symmove);
    else if (cmd.startsWith("AT+TRK_SYMMOVE="))
    {
        cmd.remove(0, 15);
        cmd.replace("\"", "");
        strncpy(config.trk_symmove, cmd.c_str(), sizeof(config.trk_symmove));
        return "OK";
    }

    if (cmd == "AT+TRK_SYMSTOP?")
        return String(config.trk_symstop);
    else if (cmd.startsWith("AT+TRK_SYMSTOP="))
    {
        cmd.remove(0, 15);
        cmd.replace("\"", "");
        strncpy(config.trk_symstop, cmd.c_str(), sizeof(config.trk_symstop));
        return "OK";
    }

    if (cmd == "AT+TRK_ITEM?")
        return String(config.trk_item);
    else if (cmd.startsWith("AT+TRK_ITEM="))
    {
        cmd.remove(0, 12);
        cmd.replace("\"", "");
        strncpy(config.trk_item, cmd.c_str(), sizeof(config.trk_item));
        return "OK";
    }

    // if (cmd == "AT+TML0_DATA_CHANNEL?")
    //     return String(config.tml0_data_channel);
    // else if (cmd.startsWith("AT+TML0_DATA_CHANNEL="))
    // {
    //     config.tml0_data_channel = cmd.substring(21).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+TML1_DATA_CHANNEL?")
    //     return String(config.tml1_data_channel);
    // else if (cmd.startsWith("AT+TML1_DATA_CHANNEL="))
    // {
    //     config.tml1_data_channel = cmd.substring(21).toInt();
    //     return "OK";
    // }

    if (cmd == "AT+H_UP?")
        return String(config.h_up ? "1" : "0");
    else if (cmd == "AT+H_UP=1")
    {
        config.h_up = true;
        return "OK";
    }
    else if (cmd == "AT+H_UP=0")
    {
        config.h_up = false;
        return "OK";
    }

    if (cmd == "AT+TX_DISPLAY?")
        return String(config.tx_display ? "1" : "0");
    else if (cmd == "AT+TX_DISPLAY=1")
    {
        config.tx_display = true;
        return "OK";
    }
    else if (cmd == "AT+TX_DISPLAY=0")
    {
        config.tx_display = false;
        return "OK";
    }

    if (cmd == "AT+RX_DISPLAY?")
        return String(config.rx_display ? "1" : "0");
    else if (cmd == "AT+RX_DISPLAY=1")
    {
        config.rx_display = true;
        return "OK";
    }
    else if (cmd == "AT+RX_DISPLAY=0")
    {
        config.rx_display = false;
        return "OK";
    }

    if (cmd == "AT+GNSS_PPS_GPIO?")
        return String(config.gnss_pps_gpio);
    else if (cmd.startsWith("AT+GNSS_PPS_GPIO="))
    {
        config.gnss_pps_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+GNSS_CHANNEL?")
        return String(config.gnss_channel);
    else if (cmd.startsWith("AT+GNSS_CHANNEL="))
    {
        config.gnss_channel = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_TX_GPIO?")
        return String(config.rf_tx_gpio);
    else if (cmd.startsWith("AT+RF_TX_GPIO="))
    {
        config.rf_tx_gpio = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_RX_GPIO?")
        return String(config.rf_rx_gpio);
    else if (cmd.startsWith("AT+RF_RX_GPIO="))
    {
        config.rf_rx_gpio = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_SQL_GPIO?")
        return String(config.rf_sql_gpio);
    else if (cmd.startsWith("AT+RF_SQL_GPIO="))
    {
        config.rf_sql_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_PD_GPIO?")
        return String(config.rf_pd_gpio);
    else if (cmd.startsWith("AT+RF_PD_GPIO="))
    {
        config.rf_pd_gpio = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_PWR_GPIO?")
        return String(config.rf_pwr_gpio);
    else if (cmd.startsWith("AT+RF_PWR_GPIO="))
    {
        config.rf_pwr_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_PTT_GPIO?")
        return String(config.rf_ptt_gpio);
    else if (cmd.startsWith("AT+RF_PTT_GPIO="))
    {
        config.rf_ptt_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+RF_SQL_ACTIVE?")
        return String(config.rf_sql_active ? "1" : "0");
    else if (cmd == "AT+RF_SQL_ACTIVE=1")
    {
        config.rf_sql_active = true;
        return "OK";
    }
    else if (cmd == "AT+RF_SQL_ACTIVE=0")
    {
        config.rf_sql_active = false;
        return "OK";
    }

    if (cmd == "AT+RF_PD_ACTIVE?")
        return String(config.rf_pd_active ? "1" : "0");
    else if (cmd == "AT+RF_PD_ACTIVE=1")
    {
        config.rf_pd_active = true;
        return "OK";
    }
    else if (cmd == "AT+RF_PD_ACTIVE=0")
    {
        config.rf_pd_active = false;
        return "OK";
    }

    if (cmd == "AT+RF_PWR_ACTIVE?")
        return String(config.rf_pwr_active ? "1" : "0");
    else if (cmd == "AT+RF_PWR_ACTIVE=1")
    {
        config.rf_pwr_active = true;
        return "OK";
    }
    else if (cmd == "AT+RF_PWR_ACTIVE=0")
    {
        config.rf_pwr_active = false;
        return "OK";
    }

    if (cmd == "AT+RF_PTT_ACTIVE?")
        return String(config.rf_ptt_active ? "1" : "0");
    else if (cmd == "AT+RF_PTT_ACTIVE=1")
    {
        config.rf_ptt_active = true;
        return "OK";
    }
    else if (cmd == "AT+RF_PTT_ACTIVE=0")
    {
        config.rf_ptt_active = false;
        return "OK";
    }

    if (cmd == "AT+ADC_GPIO?")
        return String(config.adc_gpio);
    else if (cmd.startsWith("AT+ADC_GPIO="))
    {
        config.adc_gpio = cmd.substring(12).toInt();
        return "OK";
    }

    if (cmd == "AT+DAC_GPIO?")
        return String(config.dac_gpio);
    else if (cmd.startsWith("AT+DAC_GPIO="))
    {
        config.dac_gpio = cmd.substring(12).toInt();
        return "OK";
    }

    if (cmd == "AT+ADC_SEL_GPIO?")
        return String(config.adc_sel_gpio);
    else if (cmd.startsWith("AT+ADC_SEL_GPIO="))
    {
        config.adc_sel_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+DAC_SEL_GPIO?")
        return String(config.dac_sel_gpio);
    else if (cmd.startsWith("AT+DAC_SEL_GPIO="))
    {
        config.dac_sel_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+ADC_ATTEN?")
        return String(config.adc_atten);
    else if (cmd.startsWith("AT+ADC_ATTEN="))
    {
        config.adc_atten = cmd.substring(13).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C_SDA_PIN?")
        return String(config.i2c_sda_pin);
    else if (cmd.startsWith("AT+I2C_SDA_PIN="))
    {
        config.i2c_sda_pin = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C_SCK_PIN?")
        return String(config.i2c_sck_pin);
    else if (cmd.startsWith("AT+I2C_SCK_PIN="))
    {
        config.i2c_sck_pin = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C_RST_PIN?")
        return String(config.i2c_rst_pin);
    else if (cmd.startsWith("AT+I2C_RST_PIN="))
    {
        config.i2c_rst_pin = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C_FREQ?")
        return String(config.i2c_freq);
    else if (cmd.startsWith("AT+I2C_FREQ="))
    {
        config.i2c_freq = cmd.substring(12).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C1_SDA_PIN?")
        return String(config.i2c1_sda_pin);
    else if (cmd.startsWith("AT+I2C1_SDA_PIN="))
    {
        config.i2c1_sda_pin = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C1_SCK_PIN?")
        return String(config.i2c1_sck_pin);
    else if (cmd.startsWith("AT+I2C1_SCK_PIN="))
    {
        config.i2c1_sck_pin = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+I2C1_FREQ?")
        return String(config.i2c1_freq);
    else if (cmd.startsWith("AT+I2C1_FREQ="))
    {
        config.i2c1_freq = cmd.substring(13).toInt();
        return "OK";
    }

    if (cmd == "AT+ONEWIRE_ENABLE?")
        return String(config.onewire_enable ? "1" : "0");
    else if (cmd == "AT+ONEWIRE_ENABLE=1")
    {
        config.onewire_enable = true;
        return "OK";
    }
    else if (cmd == "AT+ONEWIRE_ENABLE=0")
    {
        config.onewire_enable = false;
        return "OK";
    }

    if (cmd == "AT+ONEWIRE_GPIO?")
        return String(config.onewire_gpio);
    else if (cmd.startsWith("AT+ONEWIRE_GPIO="))
    {
        config.onewire_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+UART0_ENABLE?")
        return String(config.uart0_enable ? "1" : "0");
    else if (cmd == "AT+UART0_ENABLE=1")
    {
        config.uart0_enable = true;
        return "OK";
    }
    else if (cmd == "AT+UART0_ENABLE=0")
    {
        config.uart0_enable = false;
        return "OK";
    }

    if (cmd == "AT+UART0_TX_GPIO?")
        return String(config.uart0_tx_gpio);
    else if (cmd.startsWith("AT+UART0_TX_GPIO="))
    {
        config.uart0_tx_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+UART0_RX_GPIO?")
        return String(config.uart0_rx_gpio);
    else if (cmd.startsWith("AT+UART0_RX_GPIO="))
    {
        config.uart0_rx_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+UART0_RTS_GPIO?")
        return String(config.uart0_rts_gpio);
    else if (cmd.startsWith("AT+UART0_RTS_GPIO="))
    {
        config.uart0_rts_gpio = cmd.substring(18).toInt();
        return "OK";
    }

    if (cmd == "AT+UART1_ENABLE?")
        return String(config.uart1_enable ? "1" : "0");
    else if (cmd == "AT+UART1_ENABLE=1")
    {
        config.uart1_enable = true;
        return "OK";
    }
    else if (cmd == "AT+UART1_ENABLE=0")
    {
        config.uart1_enable = false;
        return "OK";
    }

    if (cmd == "AT+UART1_TX_GPIO?")
        return String(config.uart1_tx_gpio);
    else if (cmd.startsWith("AT+UART1_TX_GPIO="))
    {
        config.uart1_tx_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+UART1_RX_GPIO?")
        return String(config.uart1_rx_gpio);
    else if (cmd.startsWith("AT+UART1_RX_GPIO="))
    {
        config.uart1_rx_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+UART1_RTS_GPIO?")
        return String(config.uart1_rts_gpio);
    else if (cmd.startsWith("AT+UART1_RTS_GPIO="))
    {
        config.uart1_rts_gpio = cmd.substring(18).toInt();
        return "OK";
    }

    if (cmd == "AT+MODBUS_ENABLE?")
        return String(config.modbus_enable ? "1" : "0");
    else if (cmd == "AT+MODBUS_ENABLE=1")
    {
        config.modbus_enable = true;
        return "OK";
    }
    else if (cmd == "AT+MODBUS_ENABLE=0")
    {
        config.modbus_enable = false;
        return "OK";
    }

    if (cmd == "AT+MODBUS_ADDRESS?")
        return String(config.modbus_address);
    else if (cmd.startsWith("AT+MODBUS_ADDRESS="))
    {
        config.modbus_address = cmd.substring(18).toInt();
        return "OK";
    }

    if (cmd == "AT+MODBUS_CHANNEL?")
        return String(config.modbus_channel);
    else if (cmd.startsWith("AT+MODBUS_CHANNEL="))
    {
        config.modbus_channel = cmd.substring(18).toInt();
        return "OK";
    }

    if (cmd == "AT+MODBUS_DE_GPIO?")
        return String(config.modbus_de_gpio);
    else if (cmd.startsWith("AT+MODBUS_DE_GPIO="))
    {
        config.modbus_de_gpio = cmd.substring(18).toInt();
        return "OK";
    }

    if (cmd == "AT+COUNTER0_ENABLE?")
        return String(config.counter0_enable ? "1" : "0");
    else if (cmd == "AT+COUNTER0_ENABLE=1")
    {
        config.counter0_enable = true;
        return "OK";
    }
    else if (cmd == "AT+COUNTER0_ENABLE=0")
    {
        config.counter0_enable = false;
        return "OK";
    }

    if (cmd == "AT+COUNTER0_ACTIVE?")
        return String(config.counter0_active ? "1" : "0");
    else if (cmd == "AT+COUNTER0_ACTIVE=1")
    {
        config.counter0_active = true;
        return "OK";
    }
    else if (cmd == "AT+COUNTER0_ACTIVE=0")
    {
        config.counter0_active = false;
        return "OK";
    }

    if (cmd == "AT+COUNTER0_GPIO?")
        return String(config.counter0_gpio);
    else if (cmd.startsWith("AT+COUNTER0_GPIO="))
    {
        config.counter0_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+COUNTER1_ENABLE?")
        return String(config.counter1_enable ? "1" : "0");
    else if (cmd == "AT+COUNTER1_ENABLE=1")
    {
        config.counter1_enable = true;
        return "OK";
    }
    else if (cmd == "AT+COUNTER1_ENABLE=0")
    {
        config.counter1_enable = false;
        return "OK";
    }

    if (cmd == "AT+COUNTER1_ACTIVE?")
        return String(config.counter1_active ? "1" : "0");
    else if (cmd == "AT+COUNTER1_ACTIVE=1")
    {
        config.counter1_active = true;
        return "OK";
    }
    else if (cmd == "AT+COUNTER1_ACTIVE=0")
    {
        config.counter1_active = false;
        return "OK";
    }

    if (cmd == "AT+COUNTER1_GPIO?")
        return String(config.counter1_gpio);
    else if (cmd.startsWith("AT+COUNTER1_GPIO="))
    {
        config.counter1_gpio = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+EXT_TNC_ENABLE?")
        return String(config.ext_tnc_enable ? "1" : "0");
    else if (cmd == "AT+EXT_TNC_ENABLE=1")
    {
        config.ext_tnc_enable = true;
        return "OK";
    }
    else if (cmd == "AT+EXT_TNC_ENABLE=0")
    {
        config.ext_tnc_enable = false;
        return "OK";
    }

    if (cmd == "AT+EXT_TNC_CHANNEL?")
        return String(config.ext_tnc_channel);
    else if (cmd.startsWith("AT+EXT_TNC_CHANNEL="))
    {
        config.ext_tnc_channel = cmd.substring(19).toInt();
        return "OK";
    }

    if (cmd == "AT+EXT_TNC_MODE?")
        return String(config.ext_tnc_mode);
    else if (cmd.startsWith("AT+EXT_TNC_MODE="))
    {
        config.ext_tnc_mode = cmd.substring(16).toInt();
        return "OK";
    }

    // AT+PWR_EN / PWR_MODE / PWR_SLEEP_INTERVAL / PWR_SLEEP_ACTIVATE /
    // PWR_GPIO / PWR_ACTIVE removed (Set/2026, LU6JMF): dead "Power Save
    // Mode" feature, nothing in the firmware acted on these. AT+PWR_STANBY_
    // DELAY stays below - it's genuinely used (OLED standby timeout).

    if (cmd == "AT+PWR_STANBY_DELAY?")
        return String(config.pwr_stanby_delay);
    else if (cmd.startsWith("AT+PWR_STANBY_DELAY="))
    {
        config.pwr_stanby_delay = cmd.substring(20).toInt();
        return "OK";
    }

    if (cmd == "AT+LOG?")
        return String(config.log);
    else if (cmd.startsWith("AT+LOG="))
    {
        config.log = cmd.substring(7).toInt();
        return "OK";
    }

    // if (cmd == "AT+TRK_TLM_AVG?")
    //     return String(config.trk_tlm_avg ? "1" : "0");
    // else if (cmd == "AT+TRK_TLM_AVG=1")
    // {
    //     config.trk_tlm_avg = true;
    //     return "OK";
    // }
    // else if (cmd == "AT+TRK_TLM_AVG=0")
    // {
    //     config.trk_tlm_avg = false;
    //     return "OK";
    // }

    // if (cmd == "AT+TRK_TLM_SENSOR?")
    //     return String(config.trk_tlm_sensor);
    // else if (cmd.startsWith("AT+TRK_TLM_SENSOR="))
    // {
    //     config.trk_tlm_sensor = cmd.substring(18).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+TRK_TLM_PRECISION?")
    //     return String(config.trk_tlm_precision);
    // else if (cmd.startsWith("AT+TRK_TLM_PRECISION="))
    // {
    //     config.trk_tlm_precision = cmd.substring(21).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+TRK_TLM_OFFSET?")
    //     return String(config.trk_tlm_offset, 6);
    // else if (cmd.startsWith("AT+TRK_TLM_OFFSET="))
    // {
    //     config.trk_tlm_offset = cmd.substring(18).toFloat();
    //     return "OK";
    // }

    // if (cmd == "AT+DIGI_TLM_AVG?")
    //     return String(config.digi_tlm_avg ? "1" : "0");
    // else if (cmd == "AT+DIGI_TLM_AVG=1")
    // {
    //     config.digi_tlm_avg = true;
    //     return "OK";
    // }
    // else if (cmd == "AT+DIGI_TLM_AVG=0")
    // {
    //     config.digi_tlm_avg = false;
    //     return "OK";
    // }

    // if (cmd == "AT+DIGI_TLM_SENSOR?")
    //     return String(config.digi_tlm_sensor);
    // else if (cmd.startsWith("AT+DIGI_TLM_SENSOR="))
    // {
    //     config.digi_tlm_sensor = cmd.substring(19).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+DIGI_TLM_PRECISION?")
    //     return String(config.digi_tlm_precision);
    // else if (cmd.startsWith("AT+DIGI_TLM_PRECISION="))
    // {
    //     config.digi_tlm_precision = cmd.substring(22).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+DIGI_TLM_OFFSET?")
    //     return String(config.digi_tlm_offset, 6);
    // else if (cmd.startsWith("AT+DIGI_TLM_OFFSET="))
    // {
    //     config.digi_tlm_offset = cmd.substring(19).toFloat();
    //     return "OK";
    // }

    // if (cmd == "AT+IGATE_TLM_AVG?")
    //     return String(config.igate_tlm_avg ? "1" : "0");
    // else if (cmd == "AT+IGATE_TLM_AVG=1")
    // {
    //     config.igate_tlm_avg = true;
    //     return "OK";
    // }
    // else if (cmd == "AT+IGATE_TLM_AVG=0")
    // {
    //     config.igate_tlm_avg = false;
    //     return "OK";
    // }

    // if (cmd == "AT+IGATE_TLM_SENSOR?")
    //     return String(config.igate_tlm_sensor);
    // else if (cmd.startsWith("AT+IGATE_TLM_SENSOR="))
    // {
    //     config.igate_tlm_sensor = cmd.substring(20).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+IGATE_TLM_PRECISION?")
    //     return String(config.igate_tlm_precision);
    // else if (cmd.startsWith("AT+IGATE_TLM_PRECISION="))
    // {
    //     config.igate_tlm_precision = cmd.substring(23).toInt();
    //     return "OK";
    // }

    // if (cmd == "AT+IGATE_TLM_OFFSET?")
    //     return String(config.igate_tlm_offset, 6);
    // else if (cmd.startsWith("AT+IGATE_TLM_OFFSET="))
    // {
    //     config.igate_tlm_offset = cmd.substring(20).toFloat();
    //     return "OK";
    // }

    // if (cmd == "AT+WX_SENSOR_ENABLE?")
    //     return String(config.wx_sensor_enable ? "1" : "0");
    // else if (cmd == "AT+WX_SENSOR_ENABLE=1")
    // {
    //     config.wx_sensor_enable = true;
    //     return "OK";
    // }
    // else if (cmd == "AT+WX_SENSOR_ENABLE=0")
    // {
    //     config.wx_sensor_enable = false;
    //     return "OK";
    // }

    // if (cmd == "AT+WX_SENSOR_AVG?")
    //     return String(config.wx_sensor_avg ? "1" : "0");
    // else if (cmd == "AT+WX_SENSOR_AVG=1")
    // {
    //     config.wx_sensor_avg = true;
    //     return "OK";
    // }
    // else if (cmd == "AT+WX_SENSOR_AVG=0")
    // {
    //     config.wx_sensor_avg = false;
    //     return "OK";
    // }

    // if (cmd == "AT+WX_SENSOR_CH?")
    //     return String(config.wx_sensor_ch);
    // else if (cmd.startsWith("AT+WX_SENSOR_CH="))
    // {
    //     config.wx_sensor_ch = cmd.substring(16).toInt();
    //     return "OK";
    // }

    if (cmd == "AT+PPP_ENABLE?")
        return String(config.ppp_enable ? "1" : "0");
    else if (cmd == "AT+PPP_ENABLE=1")
    {
        config.ppp_enable = true;
        return "OK";
    }
    else if (cmd == "AT+PPP_ENABLE=0")
    {
        config.ppp_enable = false;
        return "OK";
    }

    if (cmd == "AT+PPP_RST_GPIO?")
        return String(config.ppp_rst_gpio);
    else if (cmd.startsWith("AT+PPP_RST_GPIO="))
    {
        config.ppp_rst_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_TX_GPIO?")
        return String(config.ppp_tx_gpio);
    else if (cmd.startsWith("AT+PPP_TX_GPIO="))
    {
        config.ppp_tx_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_RX_GPIO?")
        return String(config.ppp_rx_gpio);
    else if (cmd.startsWith("AT+PPP_RX_GPIO="))
    {
        config.ppp_rx_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_RTS_GPIO?")
        return String(config.ppp_rts_gpio);
    else if (cmd.startsWith("AT+PPP_RTS_GPIO="))
    {
        config.ppp_rts_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_CTS_GPIO?")
        return String(config.ppp_cts_gpio);
    else if (cmd.startsWith("AT+PPP_CTS_GPIO="))
    {
        config.ppp_cts_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_DTR_GPIO?")
        return String(config.ppp_dtr_gpio);
    else if (cmd.startsWith("AT+PPP_DTR_GPIO="))
    {
        config.ppp_dtr_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_RI_GPIO?")
        return String(config.ppp_ri_gpio);
    else if (cmd.startsWith("AT+PPP_RI_GPIO="))
    {
        config.ppp_ri_gpio = cmd.substring(15).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_RST_ACTIVE?")
        return String(config.ppp_rst_active ? "1" : "0");
    else if (cmd == "AT+PPP_RST_ACTIVE=1")
    {
        config.ppp_rst_active = true;
        return "OK";
    }
    else if (cmd == "AT+PPP_RST_ACTIVE=0")
    {
        config.ppp_rst_active = false;
        return "OK";
    }

    if (cmd == "AT+PPP_RST_DELAY?")
        return String(config.ppp_rst_delay);
    else if (cmd.startsWith("AT+PPP_RST_DELAY="))
    {
        config.ppp_rst_delay = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_PWR_GPIO?")
        return String(config.ppp_pwr_gpio);
    else if (cmd.startsWith("AT+PPP_PWR_GPIO="))
    {
        config.ppp_pwr_gpio = cmd.substring(16).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_PWR_ACTIVE?")
        return String(config.ppp_pwr_active ? "1" : "0");
    else if (cmd == "AT+PPP_PWR_ACTIVE=1")
    {
        config.ppp_pwr_active = true;
        return "OK";
    }
    else if (cmd == "AT+PPP_PWR_ACTIVE=0")
    {
        config.ppp_pwr_active = false;
        return "OK";
    }

    if (cmd == "AT+PPP_SERIAL?")
        return String(config.ppp_serial);
    else if (cmd.startsWith("AT+PPP_SERIAL="))
    {
        config.ppp_serial = cmd.substring(14).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_MODEL?")
        return String(config.ppp_model);
    else if (cmd.startsWith("AT+PPP_MODEL="))
    {
        config.ppp_model = cmd.substring(13).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_FLOW_CTRL?")
        return String(config.ppp_flow_ctrl);
    else if (cmd.startsWith("AT+PPP_FLOW_CTRL="))
    {
        config.ppp_flow_ctrl = cmd.substring(17).toInt();
        return "OK";
    }

    if (cmd == "AT+PPP_GNSS?")
        return String(config.ppp_gnss ? "1" : "0");
    else if (cmd == "AT+PPP_GNSS=1")
    {
        config.ppp_gnss = true;
        return "OK";
    }
    else if (cmd == "AT+PPP_GNSS=0")
    {
        config.ppp_gnss = false;
        return "OK";
    }

#ifdef MQTT
#endif
    return "ERR";
}
