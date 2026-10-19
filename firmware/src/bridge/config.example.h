// duco-rf-bridge configuration.
// Copy this file to config.h (same folder) and fill it in. config.h is
// git-ignored, so your WiFi/MQTT passwords and network ID stay out of git.
//
// The Duco values (DUCO_NET, DUCO_NODES, DUCO_TX_FREQ_MHZ) come from the
// sniffer firmware: run it with your OLD DucoBox powered on, wait for the
// "[suggested config]" block it prints, and paste it in here.
#pragma once

// WiFi
#define WIFI_SSID      "your-wifi"
#define WIFI_PASS      "your-wifi-password"

// MQTT broker (the one Home Assistant uses, e.g. the Mosquitto add-on)
#define MQTT_HOST      "192.168.1.10"
#define MQTT_PORT      1883
#define MQTT_USER      ""                  // "" = no login
#define MQTT_PASS      ""
#define MQTT_CLIENT_ID "duco-rf-bridge"

// Your Duco RF network ID = your DucoBox's device ID (8 hex digits).
// 0 = not set: the bridge refuses to transmit until you set it.
#define DUCO_NET       0x00000000UL

// The nodes (sensors / switches) on your network: { address, sensor, id, name }.
//   address  2..31, as the sniffer lists them
//   sensor   'c' = CO2 (reg 0x4A), 'h' = humidity (reg 0x4B), 'b' = both.
//            Not sure? Use 'b' first: a sensor the node doesn't have reads 0.
//   id       MQTT topic duco/<id> and HA unique ids (lowercase, no spaces)
//   name     what Home Assistant shows
#define DUCO_NODES  { {2, 'b', "node_2", "Node 2"}, \
                      {3, 'b', "node_3", "Node 3"} }

// Your box's transmit frequency in MHz. Nominal is 868.2943; real boxes sit a
// few kHz off. The sniffer measures yours (AFC) and suggests the value.
#define DUCO_TX_FREQ_MHZ   868.2943f
// +14 dBm = 25 mW, the legal limit in this 868 MHz sub-band (docs/hardware.md).
#define DUCO_TX_POWER_DBM  14
