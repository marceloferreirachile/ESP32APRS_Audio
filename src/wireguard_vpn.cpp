//==============================================================================
// Wireguard VPN Client demo for LwIP/ESP32     
//==============================================================================

//==============================================================================
//  Includes
//==============================================================================
#include <Arduino.h>
#include "main.h"
#include <esp_wireguard.h>
#include <WiFi.h>
#include "lwip/dns.h"
#include "lwip/ip_addr.h"


//ใช้ตัวแปรโกลบอลในไฟล์ main.cpp
extern Configuration config;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#include "wireguardif.h"
#include "wireguard.h"
#include "wireguard-platform.h"

#include "wireguard_vpn.h"
#include <lwip/tcpip.h>
//==============================================================================
//  Defines
//==============================================================================
#define CMP_NAME "WG_VPN"

#if !defined(WG_CLIENT_PRIVATE_KEY) || !defined(WG_PEER_PUBLIC_KEY)
#error "Please update configuratiuon with your VPN-specific keys!"
#endif

bool wireguard_up()
{
esp_err_t err = ESP_FAIL;

wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();

ip_addr_t ipaddr = WG_LOCAL_ADDRESS;
    ip_addr_t netmask = WG_LOCAL_NETMASK;
    ip_addr_t gateway = WG_GATEWAY_ADDRESS;
    ip_addr_t peer_address = WG_PEER_ADDRESS;

    ipaddr_aton(config.wg_local_address,&ipaddr);
    ipaddr_aton(config.wg_netmask_address,&netmask);
    ipaddr_aton(config.wg_gw_address,&gateway);
    //ipaddr_aton(config.wg_peer_address,&peer_address);
    IPAddress hostIp;
    if (WiFi.hostByName(config.wg_peer_address, hostIp)) {
        ipaddr_aton(hostIp.toString().c_str(), &peer_address);
    }else{
        ipaddr_aton(config.wg_peer_address,&peer_address);
    }

wg_config.address = config.wg_local_address;
wg_config.private_key = config.wg_private_key;;
wg_config.listen_port = config.wg_port+1;
wg_config.public_key = config.wg_public_key;
//wg_config.netmask = config.wg_netmask_address;
char netmask2[20];
sprintf(netmask2,"255.255.255.255");
wg_config.netmask = netmask2;

wg_config.endpoint=config.wg_peer_address;
//ip_addr_set(&wg_config.endpoint, &peer_address);
    //peer.endport_port = WG_PEER_PORT;
wg_config.port = config.wg_port;
wg_config.preshared_key = NULL;


/* If the device is behind NAT or stateful firewall, set persistent_keepalive.
   persistent_keepalive is disabled by default */
// wg_config.persistent_keepalive = 10;

wireguard_ctx_t ctx = {0};
err = esp_wireguard_init(&wg_config, &ctx);

/* start establishing the link. after this call, esp_wireguard start
   establishing connection. */
err = esp_wireguard_connect(&ctx);

/* after some time, see if the link is up. note that it takes some time to
   establish the link */
err = esp_wireguardif_peer_is_up(&ctx);
if (err == ESP_OK) {
    /* the link is up */
    return true;
}
    /* the link is not up */
return false;
}

//==============================================================================
//  Local types
//==============================================================================

//==============================================================================
//  Local data
//==============================================================================
static struct netif wg_netif_struct = {0};
static struct netif *wg_netif = NULL;
static uint8_t wireguard_peer_index_local = WIREGUARDIF_INVALID_INDEX;

//==============================================================================
//  Exported data
//==============================================================================

//==============================================================================
//  Local functions
//==============================================================================

//==============================================================================
//  Exported functions
//==============================================================================
void wireguard_change_device()
{
    if(wg_netif==NULL) return;
    LOCK_TCPIP_CORE();
        wireguardif_changedevice(wg_netif);
    UNLOCK_TCPIP_CORE();
}

bool wireguard_active()
{
    if(wg_netif!=NULL) return true;
    return false;
}

void wireguard_remove()
{

 if(wg_netif!=NULL){
    LOCK_TCPIP_CORE();
        wireguardif_disconnect(wg_netif, wireguard_peer_index_local);
        wireguardif_remove_peer(wg_netif, wireguard_peer_index_local);
     netif_set_down(wg_netif);
     netif_remove(&wg_netif_struct);
     UNLOCK_TCPIP_CORE();
 }
}


// LU6JMF (Oct/2026): a WireGuard key is 32 bytes in base64 = exactly 44 chars ending in '='.
// An empty, truncated or mistyped key makes wireguardif_init() fail, netif_add() then
// returns NULL and the original code dereferenced it (netif_set_up(NULL)) -> PANIC at
// every boot as soon as NTP synced = boot loop. Validate first, never touch a NULL netif.
static bool wg_key_valid(const char *k)
{
    if (k == NULL || strlen(k) != 44 || k[43] != '=')
        return false;
    for (int i = 0; i < 43; i++)
    {
        char c = k[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (!ok)
            return false;
    }
    return true;
}

// Short text for the VPN status line / logs: why the tunnel was not started ("" = ok).
const char *wireguard_last_error = "";

void wireguard_setup()
{
    wireguard_last_error = "";
    if (!wg_key_valid(config.wg_private_key))
    {
        wireguard_last_error = "Invalid private key (must be 44 chars, base64, ends with =)";
        log_d("WireGuard NOT started: %s", wireguard_last_error);
        return;
    }
    if (!wg_key_valid(config.wg_public_key))
    {
        wireguard_last_error = "Invalid public key (must be 44 chars, base64, ends with =)";
        log_d("WireGuard NOT started: %s", wireguard_last_error);
        return;
    }
    if (config.wg_peer_address[0] == '\0')
    {
        wireguard_last_error = "Server address is empty";
        log_d("WireGuard NOT started: %s", wireguard_last_error);
        return;
    }
    // LU6JMF: start WireGuard's random generator once. esp_wireguard_init() does it, but this
    // function calls netif_add() directly, so it never ran: on ESP32-S3 / ESP-IDF 5 the first
    // random number crashed the board (null entropy function -> reboot loop); on the classic
    // ESP32 the generator ran without a seed.
    static bool wgRandomReady = false;
    if (!wgRandomReady)
    {
        if (wireguard_platform_init() != ESP_OK)
        {
            wireguard_last_error = "WireGuard random generator could not start";
            log_d("WireGuard NOT started: %s", wireguard_last_error);
            return;
        }
        wgRandomReady = true;
    }
    struct wireguardif_init_data wg;
    struct wireguardif_peer peer;
    ip_addr_t ipaddr;
    ip_addr_t netmask;
    ip_addr_t gateway;
    ip_addr_t peer_address;

    ipaddr_aton(config.wg_local_address,&ipaddr);
    ipaddr_aton(config.wg_netmask_address,&netmask);
    ipaddr_aton(config.wg_gw_address,&gateway);
    //ipaddr_aton(config.wg_peer_address,&peer_address);
    //dns_gethostbyname(config.wg_peer_address, &peer_address, NULL, NULL);

    IPAddress hostIp;
    if (WiFi.hostByName(config.wg_peer_address, hostIp)) {
        ipaddr_aton(hostIp.toString().c_str(), &peer_address);
    }else{
        ipaddr_aton(config.wg_peer_address,&peer_address);
    }
    // Setup the WireGuard device structure
    // wg.private_key = WG_CLIENT_PRIVATE_KEY;
    // wg.listen_port = WG_CLIENT_PORT;
    wg.private_key = config.wg_private_key;
    wg.listen_port = config.wg_port+1;
    //log_d("WireGuard ppp netif: %p",ppp_netif);
    //log_d("PPP Gateway: %s",IPAddress(&ppp_netif->gw).toString().c_str());
    wg.bind_netif = NULL; // NB! not working on ESP32 even if set!

    if(wg_netif==NULL){
        // Register the new WireGuard network interface with lwIP
        LOCK_TCPIP_CORE();        
        wg_netif = netif_add(&wg_netif_struct, ip_2_ip4(&ipaddr), ip_2_ip4(&netmask), ip_2_ip4(&gateway), &wg, &wireguardif_init, &ip_input);
        if (wg_netif != NULL)
        {
            // Mark the interface as administratively up, link up flag is set automatically when peer connects
            netif_set_up(wg_netif);
        }
        UNLOCK_TCPIP_CORE();
    }

    // LU6JMF: netif_add can fail (bad key, UDP port in use, no memory). Never continue with NULL.
    if (wg_netif == NULL)
    {
        wireguard_last_error = "WireGuard interface could not be created (key, port or memory)";
        log_d("WireGuard NOT started: %s", wireguard_last_error);
        return;
    }

    // Initialise the first WireGuard peer structure
    wireguardif_peer_init(&peer);
    // peer.public_key = WG_PEER_PUBLIC_KEY;
    peer.public_key = config.wg_public_key;
    peer.preshared_key = NULL;
    // Allow all IPs through tunnel
    //peer.allowed_ip = IPADDR4_INIT_BYTES(0, 0, 0, 0);
    IP_ADDR4(&peer.allowed_ip, 0, 0, 0, 0);
    IP_ADDR4(&peer.allowed_mask, 0, 0, 0, 0);

    // If we know the endpoint's address can add here
    ip_addr_set(&peer.endpoint_ip, &peer_address);
    //peer.endport_port = WG_PEER_PORT;
    peer.endport_port = config.wg_port;

    // Register the new WireGuard peer with the netwok interface
    // LU6JMF: these lwIP calls (UDP pcb, pbuf, timers) must run with the TCPIP core locked,
    // like the netif_add above; from this task unlocked they can corrupt lwIP state.
    LOCK_TCPIP_CORE();
    wireguardif_add_peer(wg_netif, &peer, &wireguard_peer_index_local);

    if ((wireguard_peer_index_local != WIREGUARDIF_INVALID_INDEX) && !ip_addr_isany(&peer.endpoint_ip))
    {
        // Start outbound connection to peer
        wireguardif_connect(wg_netif, wireguard_peer_index_local);
    }
    else
    {
        wireguard_last_error = "Peer not added (check public key and that the server address resolves)";
    }
    UNLOCK_TCPIP_CORE();
}

#ifdef __cplusplus
}
#endif // __cplusplus