# CardPing_Multi
Ping Pong Multiplayer game for M5stack Cardputer

CardPing Multi README

Multiplayer Pong for two M5 Cardputers sharing a Wi-Fi network. One hosts, the other joins; state sync rides over UDP so both displays stay in lockstep.
Menus: Wi-Fi scan → enter/remember password → pick player name → choose Host/Join. Preferences persist SSID/password so reconnect is quick.
Host flow: press H on Role Select, wait in lobby, Space serves/starts rounds, Esc toggles pause overlay, Q backs out to menus.
Client flow: press J, device broadcasts join requests, host auto-acknowledges, lobby shows both names. Use ; and . (semicolon/dot) for paddle movement once match starts.
Gameplay: first to 7 points wins. Ball accelerates slightly on every paddle hit. Host sim runs authoritative physics; client mirrors via state packets and sends paddle updates.
Pause & game over: host Esc pauses and shares state so client sees overlay. On victory screen host can Space for rematch, both can Q to return to main menu.
Networking: UDP port 41000 on local subnet, broadcasts when searching for hosts. Connection timeout (~4 s) drops back to error screen if packets stop.
Controls summary: ; up / . down everywhere, Enter to confirm, Q (or Fn+Q in some menus) backs out, Fn+Tab toggles password mask, R rescans Wi-Fi, Space serves/rematches, Esc pause (host during play).
Build/flash: pio run --environment m5stack-cardputer then pio run --target upload. Ensure both devices flashed with same firmware before hosting/joining.
