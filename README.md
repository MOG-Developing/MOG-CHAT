# MOG-CHAT V1.3
---
**MOG-CHAT is a lightweight chatserver accessible via browser running on ESP32. You connect via Wi-Fi, send messages and chat in real time without connecting to the internet. V1.3 adds ESP-NOW mesh networking. Multiple ESP32s can link together and sync messages across all of them.**

![Stars](https://img.shields.io/github/stars/MOG-Developing/MOG-CHAT?style=social) ![Forks](https://img.shields.io/github/forks/MOG-Developing/MOG-CHAT?style=social) ![Contributors](https://img.shields.io/github/contributors/MOG-Developing/MOG-CHAT) ![Issues](https://img.shields.io/github/issues/MOG-Developing/MOG-CHAT) ![Downloads](https://img.shields.io/github/downloads/MOG-Developing/MOG-CHAT/total) ![License](https://img.shields.io/github/license/MOG-Developing/MOG-CHAT)

---
<img src="https://raw.githubusercontent.com/MOG-Developing/MOG-CHAT/web/assets/newlogo.png" width="300">


## Why use MOG-CHAT?
MOG-CHAT runs on ESP32 hardware and creates its own WiFi AP. No internet, no cloud, no cell towers. V1.3 lets you link multiple ESP32s so the coverage area is larger. Messages sent on one ESP appear on all paired ESPs.

## MOG-CHAT V1.3
MOG-CHAT turns the ESP32 into a WiFi AP with a web server. Connect any device to its WiFi, open `http://192.168.4.1`, and chat. V1.3 adds ESP-NOW peer-to-peer networking: ESPs discover each other, pair via challenge-response, and sync messages across the mesh. Each ESP still hosts its own clients. Features full configuration via web UI, admin panel, peer management, and security (configurable creds, brute-force lockout, CSRF, SHA-256 derived keys).
[WEBSITE](https://mog-developing.github.io/MOG-CHAT/)

---

## How It Works

```
                          ┌───────────────────────────┐
                          │       ESP-NOW MESH        │
                          │  (peer-to-peer, no infra) │
                          └───────────────────────────┘
                                   │
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
     ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
     │   ESP32 Node A   │  │   ESP32 Node B   │  │   ESP32 Node C   │
     │                  │  │                  │  │                  │
     │ AP: NoWifiHere1  │  │ AP: NoWifiHere2  │  │ AP: NoWifiHere3  │
     │  IP: 192.168.4.1 │  │  IP: 192.168.4.1 │  │  IP: 192.168.4.1 │
     │  Ch: 1           │◄─┤  Ch: 1           │◄─┤  Ch: 1           │
     └───────┬──────────┘  └───────┬──────────┘  └───────┬──────────┘
             │                     │                     │
             │               ┌─────┴───────┐             │
             │               │   Clients   │             │
             │               │ ┌─────────┐ │             │
       ┌─────┴──────┐        │ │ Phone   │ │       ┌─────┴──────┐
       │  Phone     │        │ └─────────┘ │       │  Laptop    │
       └────────────┘        │ ┌─────────┐ │       └────────────┘
                             │ │ Tablet  │ │
                             │ └─────────┘ │
                             └─────────────┘
```

Each ESP runs AP mode for browser clients + ESP-NOW for ESP-to-ESP. Clients connect to any ESP and chat at `http://192.168.4.1`. Messages sync across all paired ESPs automatically.

---

## Features

- **Browser chat**: connect via WiFi, no internet needed
- **ESP-NOW mesh**: link multiple ESP32s, messages sync across all
- **Peer discovery**: ESPs auto-discover each other in range
- **Pairing system**: Pair/Accept/Reject via web UI with challenge-response auth
- **Full config**: Node ID, SSID, password, channel, TX power, etc. at `/settings`
- **Admin panel**: system info, connected clients, paired peers, RSSI, message count, reboot
- **Security**: configurable admin creds, brute-force lockout, CSRF, SHA-256 derived keys
- **Per-user rate limiting**: 1 msg/sec per username
- **Navigation bar**: quick links to Chat, Settings, Peers, Admin

---

## Pages

| URL | Description | Auth Required |
|-----|-------------|---------------|
| `/` | Chat interface | No |
| `/messages` | Messages AJAX endpoint | No |
| `/send` | Send message AJAX endpoint | No |
| `/settings` | Full configuration | Yes |
| `/admin` | System info & management | Yes |
| `/peers` | Discover, pair, and manage ESPs | Yes |
| `/clear` | Clear all messages | Yes |
| `/reboot` | Reboot the ESP32 | Yes |

---

## Getting Started

### Requirements
- ESP32-WROOM-32U (or any compatible ESP32)
- Arduino IDE with ESP32 board support
- Micro USB cable
(tested with two ESP32WROOM32U's)

### Setup
1. Add ESP32 board URL to Arduino IDE: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Install ESP32 2.0.14 via Boards Manager
3. Select **ESP32 Dev Module**, set Upload Speed 115200
4. Open `MOG-CHAT_V1.3.ino`, select COM port, upload

### First Use
1. Power the ESP32
2. Connect to WiFi `NoWifiHere` (password: `mogchat123`)
3. Open `http://192.168.4.1`
4. Start chatting!

### Linking Multiple ESPs
1. Upload same firmware to each ESP
2. Set all to the **same WiFi channel** (`/settings`)
3. Go to `/peers` on any ESP
4. Click **Pair** next to a discovered node
5. On the target ESP, go to `/peers` and click **Accept**
6. Messages now sync between them!

---

## Changelog V1.3

**New:**
- ESP-NOW mesh networking: link multiple ESP32s
- Peer discovery, pairing, and challenge-response auth
- Message sync across paired ESPs
- Web-based peer management (Pair/Accept/Reject/Unpair)
- Full settings page (`/settings`)
- Admin panel (`/admin`) with system info, RSSI, distance estimate, clear messages, reboot
- Navigation bar on all pages
- Source node tags on synced messages
- Live peer count in chat UI
- Ping/pong keepalive for peer status
- Configurable admin credentials (stored in NVS)
- Brute-force lockout (5 attempts, 30s, persisted across reboots)
- CSRF protection on all admin POST endpoints
- Per-user rate limiting on `/send` (1 msg/sec)
- SHA-256 derived LMK/PMK with 5000-iteration key stretching
- Configurable antenna GPIO (IPEX/PCB switching)
- Chunked transfer encoding to prevent heap fragmentation
- ISR-based FreeRTOS packet queue for ESP-NOW receive

**Changed from V1.2:**
- Message buffer: 32 to 64 messages
- Synced messages limited to 180 chars (local still 512) with live char counter
- Default SSID: `CHAT` to `NoWifiHere`
- Default admin user: `admin` to `admin1`
- Default pairing key set for out-of-box compatibility
- Chat UI redesigned with responsive dark theme
- Messages stored raw, sanitized at render time
- Rate limiter per-user instead of global

---

## Technical Details

| Setting | Default | Description |
|---------|---------|-------------|
| Node ID | MOG-XXXX | Unique identifier |
| AP SSID | NoWifiHere | Wi-Fi network name |
| AP Password | mogchat123 | Wi-Fi password |
| Channel | 1 | WiFi channel (must match all mesh ESPs) |
| TX Power | 13 dBm | Transmission power |
| Discovery Interval | 30s | Broadcast interval |
| Admin Username | admin1 | Login for settings/admin |
| Admin Password | admin | Login for settings/admin |
| Pairing Key | ElectricBlanket1376891376SKFRHPZJE | ESP-to-ESP auth (63 chars max) |

- Max 6 encrypted peers per ESP
- Max 64 messages per ESP (circular buffer)
- ESP-NOW payload: 250 bytes
- Settings stored in NVS
- 5000-iteration SHA-256 key derivation

---

## License

This project is licensed under the GNU General Public License v3.0. See the [LICENSE](https://raw.githubusercontent.com/MOG-Developing/MOG-CHAT/refs/heads/main/LICENSE) file.
