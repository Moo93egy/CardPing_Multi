#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

// Basic RGB565 colors used by the UI.
constexpr uint16_t COLOR_BLACK = 0x0000;
constexpr uint16_t COLOR_WHITE = 0xFFFF;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_NET = 0x7BEF;

constexpr uint8_t HID_KEY_ARROW_UP = 0x52;
constexpr uint8_t HID_KEY_ARROW_DOWN = 0x51;
constexpr uint8_t HID_KEY_ENTER = 0x28;
constexpr uint8_t HID_KEY_ESCAPE = 0x29;
constexpr char ASCII_ESC = 0x1B;

constexpr size_t PLAYER_NAME_MAX_LEN = 16;

// -----------------------------------------------------------------------------
// Wi-Fi configuration --------------------------------------------------------

constexpr uint16_t UDP_PORT = 41000;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

// -----------------------------------------------------------------------------
// Gameplay configuration -----------------------------------------------------

constexpr int SCREEN_WIDTH = 240;
constexpr int SCREEN_HEIGHT = 135;

constexpr float PADDLE_WIDTH = 8.0f;
constexpr float PADDLE_HEIGHT = 34.0f;
constexpr float PADDLE_HALF_HEIGHT = PADDLE_HEIGHT * 0.5f;
constexpr float HOST_PADDLE_X = 16.0f;
constexpr float CLIENT_PADDLE_X = SCREEN_WIDTH - HOST_PADDLE_X - PADDLE_WIDTH;
constexpr float PADDLE_SPEED = 170.0f;  // pixels per second

constexpr float BALL_RADIUS = 5.0f;
constexpr float BALL_SPEED_INITIAL = 170.0f;
constexpr float BALL_SPEED_GROWTH = 1.06f;
constexpr uint8_t MAX_SCORE = 7;

constexpr uint32_t SERVE_DELAY_MS = 1300;
constexpr uint32_t STATE_SEND_INTERVAL_MS = 32;     // ~30 FPS broadcast
constexpr uint32_t PADDLE_SEND_INTERVAL_MS = 45;    // client paddle updates
constexpr uint32_t JOIN_BROADCAST_INTERVAL_MS = 800;
constexpr uint32_t CONNECTION_TIMEOUT_MS = 4000;
constexpr int WIFI_MENU_VISIBLE_ROWS = 4;

// -----------------------------------------------------------------------------
// Button mapping -------------------------------------------------------------
// Host:    W (up) / S (down)   |  Space = serve/rematch   |  Q = quit lobby
// Client:  I (up) / K (down)   |  Q = leave lobby

// -----------------------------------------------------------------------------

enum class Role : uint8_t {
  None,
  Host,
  Client,
};

enum class Screen : uint8_t {
  WifiSelect,
  WifiPassword,
  NameEntry,
  RoleSelect,
  HostWaiting,
  ClientSearching,
  Lobby,
  Playing,
  GameOver,
  Error,
};

enum class PacketType : uint8_t {
  Join = 1,
  JoinAck = 2,
  State = 3,
  Paddle = 4,
  Start = 5,
};

constexpr uint8_t FLAG_MATCH_ACTIVE = 0x01;
constexpr uint8_t FLAG_WAITING_SERVE = 0x02;
constexpr uint8_t FLAG_GAME_OVER = 0x04;
constexpr uint8_t FLAG_PAUSED = 0x08;

#pragma pack(push, 1)
struct JoinPacket {
  uint8_t type;
  char name[PLAYER_NAME_MAX_LEN];
};

struct JoinAckPacket {
  uint8_t type;
  char name[PLAYER_NAME_MAX_LEN];
};

struct StartPacket {
  uint8_t type;
  uint32_t seed;
};

struct PaddlePacket {
  uint8_t type;
  float paddleY;
};

struct StatePacket {
  uint8_t type;
  uint8_t flags;
  uint8_t hostScore;
  uint8_t clientScore;
  uint32_t frameId;
  float ballX;
  float ballY;
  float ballVX;
  float ballVY;
  float hostPaddleY;
  float clientPaddleY;
};
#pragma pack(pop)

static_assert(sizeof(StatePacket) <= 64, "StatePacket stays under UDP buffer");

// -----------------------------------------------------------------------------
// Globals --------------------------------------------------------------------

Role g_role = Role::None;
Screen g_screen = Screen::WifiSelect;
bool g_screenDirty = true;

struct WifiNetworkInfo {
  String ssid;
  int32_t rssi;
  wifi_auth_mode_t authMode;
  bool isManual = false;
};

std::vector<WifiNetworkInfo> g_wifiNetworks;
int g_wifiSelectedIndex = 0;
bool g_wifiPasswordVisible = false;
String g_wifiSSID;
String g_wifiPassword;

String g_localPlayerName = "Player";
String g_remotePlayerName = "Opponent";

WiFiUDP g_udp;
IPAddress g_peerIp;
uint16_t g_peerPort = UDP_PORT;
bool g_hasPeer = false;

float g_hostPaddleY = SCREEN_HEIGHT * 0.5f;
float g_clientPaddleY = SCREEN_HEIGHT * 0.5f;
float g_ballX = SCREEN_WIDTH * 0.5f;
float g_ballY = SCREEN_HEIGHT * 0.5f;
float g_ballVX = 0.0f;
float g_ballVY = 0.0f;

uint8_t g_hostScore = 0;
uint8_t g_clientScore = 0;
bool g_matchActive = false;
bool g_waitingForServe = false;
bool g_gameOver = false;
int g_serveDirection = 1;  // +1 -> to client, -1 -> to host

unsigned long g_lastStateSent = 0;
unsigned long g_lastPaddleSent = 0;
unsigned long g_lastJoinBroadcast = 0;
unsigned long g_lastStateReceived = 0;
unsigned long g_lastFrameTick = 0;
unsigned long g_serveRequestTs = 0;
uint32_t g_frameCounter = 0;

String g_errorMessage;

Preferences g_preferences;
bool g_gamePaused = false;

constexpr size_t KEY_LATCH_SIZE = 512;
std::array<bool, KEY_LATCH_SIZE> g_keyLatch{};

struct MenuStar {
  float x = 0.0f;
  float y = 0.0f;
  float speed = 0.0f;
};

std::array<MenuStar, 24> g_menuStars{};
bool g_menuStarsInitialized = false;

struct ConfettiPiece {
  float x = 0.0f;
  float y = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  uint16_t color = COLOR_WHITE;
};

std::array<ConfettiPiece, 40> g_confetti{};
bool g_confettiActive = false;

uint8_t g_frameDelayMs = 5;

void onScreenEnter(Screen screen);

// -----------------------------------------------------------------------------
// Helpers --------------------------------------------------------------------

template <typename T>
T clampValue(T value, T minValue, T maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

size_t asciiLatchIndex(char key) {
  return static_cast<size_t>(static_cast<uint8_t>(key));
}

size_t hidLatchIndex(uint8_t code) {
  return static_cast<size_t>(256 + code);
}

bool asciiKeyPressed(char key) {
  return M5Cardputer.Keyboard.isKeyPressed(key);
}

bool hidKeyPressed(uint8_t code) {
  const auto &hidKeys = M5Cardputer.Keyboard.keysState().hid_keys;
  return std::find(hidKeys.begin(), hidKeys.end(), code) != hidKeys.end();
}

bool cardKeyPressedVectors(const std::vector<char> &asciiKeys, const std::vector<uint8_t> &hidKeys) {
  for (char key : asciiKeys) {
    if (asciiKeyPressed(key)) {
      return true;
    }
  }
  for (uint8_t code : hidKeys) {
    if (hidKeyPressed(code)) {
      return true;
    }
  }
  return false;
}

bool cardKeyJustPressedVectors(const std::vector<char> &asciiKeys, const std::vector<uint8_t> &hidKeys) {
  bool anyJustPressed = false;

  for (char key : asciiKeys) {
    size_t idx = asciiLatchIndex(key) % KEY_LATCH_SIZE;
    bool pressed = asciiKeyPressed(key);
    if (pressed && !g_keyLatch[idx]) {
      anyJustPressed = true;
    }
    g_keyLatch[idx] = pressed;
  }

  for (uint8_t code : hidKeys) {
    size_t idx = hidLatchIndex(code) % KEY_LATCH_SIZE;
    bool pressed = hidKeyPressed(code);
    if (pressed && !g_keyLatch[idx]) {
      anyJustPressed = true;
    }
    g_keyLatch[idx] = pressed;
  }

  return anyJustPressed;
}

bool cardKeyPressedAny(std::initializer_list<char> asciiKeys, std::initializer_list<uint8_t> hidKeys = {}) {
  return cardKeyPressedVectors(std::vector<char>(asciiKeys), std::vector<uint8_t>(hidKeys));
}

bool cardKeyJustPressedAny(std::initializer_list<char> asciiKeys, std::initializer_list<uint8_t> hidKeys = {}) {
  return cardKeyJustPressedVectors(std::vector<char>(asciiKeys), std::vector<uint8_t>(hidKeys));
}

bool cardKeyPressed(char key) {
  if (std::isalpha(static_cast<unsigned char>(key))) {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(key)));
    if (lower == upper) {
      return cardKeyPressedAny({lower});
    }
    if (key == lower) {
      return cardKeyPressedAny({lower, upper});
    }
    if (key == upper) {
      return cardKeyPressedAny({upper, lower});
    }
    return cardKeyPressedAny({key, lower, upper});
  }
  return cardKeyPressedAny({key});
}

bool cardKeyJustPressed(char key) {
  if (std::isalpha(static_cast<unsigned char>(key))) {
    char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
    char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(key)));
    if (lower == upper) {
      return cardKeyJustPressedAny({lower});
    }
    if (key == lower) {
      return cardKeyJustPressedAny({lower, upper});
    }
    if (key == upper) {
      return cardKeyJustPressedAny({upper, lower});
    }
    return cardKeyJustPressedAny({key, lower, upper});
  }
  return cardKeyJustPressedAny({key});
}

void resetKeyLatch() {
  g_keyLatch.fill(false);
  const auto &state = M5Cardputer.Keyboard.keysState();
  for (char c : state.word) {
    size_t idx = asciiLatchIndex(c) % KEY_LATCH_SIZE;
    g_keyLatch[idx] = true;
  }
  for (uint8_t hid : state.hid_keys) {
    size_t idx = hidLatchIndex(hid) % KEY_LATCH_SIZE;
    g_keyLatch[idx] = true;
  }
  for (uint8_t mod : state.modifier_keys) {
    size_t idx = hidLatchIndex(mod) % KEY_LATCH_SIZE;
    g_keyLatch[idx] = true;
  }
}

void setScreen(Screen next) {
  extern void onScreenEnter(Screen screen);
  if (g_screen != next) {
    g_screen = next;
    g_screenDirty = true;
    onScreenEnter(next);
  }
}

void resetMatchState();
void hostStartMatch(uint32_t seed);
void clientStartMatch(uint32_t seed);
void drawStaticScreen();
void drawGameFrame();
void processNetwork();
void sendJoinBroadcast();
void sendJoinAck();
void sendStartPacket(uint32_t seed);
void sendStatePacket();
void sendPaddlePacket();
void updateHostGameplay(float dtSeconds);
void updateClientGameplay(float dtSeconds);
void handleConnectionTimeout();
bool connectToWiFi();
void resetToMainMenu();
void resetToWifiSetup();
void scanAvailableNetworks();
void initMenuStars();
void drawRoleSelectFrame(float dtSeconds);
void drawWifiSelectScreen();
void drawWifiPasswordScreen();
void drawNameEntryScreen();
void initConfetti();
void drawGameOverFrameAnimated(float dtSeconds);
void handleTextInput(String &buffer, size_t maxLength, bool allowSpaces = true);
void setRemotePlayerName(const char *name);
void drawPauseOverlay();
void saveWifiCredentials();
void loadWifiCredentials();

uint32_t nextRandomSeed() {
  return millis() ^ (micros() << 8);
}

void centerBallStationary() {
  g_ballX = SCREEN_WIDTH * 0.5f;
  g_ballY = SCREEN_HEIGHT * 0.5f;
  g_ballVX = 0.0f;
  g_ballVY = 0.0f;
}

void prepareServe(int direction) {
  g_serveDirection = direction;
  g_waitingForServe = true;
  g_matchActive = true;
  g_serveRequestTs = millis();
  centerBallStationary();
}

void setRemotePlayerName(const char *name) {
  if (name == nullptr || name[0] == '\0') {
    g_remotePlayerName = (g_role == Role::Host) ? String("Challenger") : String("Host");
    return;
  }
  String candidate = String(name);
  candidate.trim();
  if (candidate.isEmpty()) {
    g_remotePlayerName = (g_role == Role::Host) ? String("Challenger") : String("Host");
  } else {
    g_remotePlayerName = candidate;
  }
}

void handleTextInput(String &buffer, size_t maxLength, bool allowSpaces) {
  if (!M5Cardputer.Keyboard.isChange()) {
    return;
  }
  if (!M5Cardputer.Keyboard.isPressed()) {
    return;
  }

  const auto &state = M5Cardputer.Keyboard.keysState();

  if (state.del && !buffer.isEmpty()) {
    buffer.remove(buffer.length() - 1);
  }

  for (char raw : state.word) {
    char c = raw;
    if (c < 32) {
      continue;
    }
    if (!allowSpaces && c == ' ') {
      continue;
    }
    if (buffer.length() >= maxLength) {
      break;
    }
    buffer += c;
  }
}

void launchBall() {
  g_waitingForServe = false;
  float speed = BALL_SPEED_INITIAL;
  g_ballVX = speed * static_cast<float>(g_serveDirection);
  float arc = static_cast<float>(random(-60, 61)) / 100.0f;  // -0.60 .. 0.60
  g_ballVY = speed * 0.6f * arc;
}

void resetPaddles() {
  g_hostPaddleY = SCREEN_HEIGHT * 0.5f;
  g_clientPaddleY = SCREEN_HEIGHT * 0.5f;
}

void resetMatchState() {
  g_hostScore = 0;
  g_clientScore = 0;
  g_gameOver = false;
  g_matchActive = false;
  g_waitingForServe = false;
  g_gamePaused = false;
  g_frameCounter = 0;
  resetPaddles();
  centerBallStationary();
}

void markGameOver() {
  g_gameOver = true;
  g_matchActive = false;
  g_waitingForServe = false;
  centerBallStationary();
  if (g_role == Role::Host) {
    sendStatePacket();
  }
  setScreen(Screen::GameOver);
}

// -----------------------------------------------------------------------------
// Rendering helpers ----------------------------------------------------------

void drawCenteredText(const String &text, int16_t y, uint8_t size = 2) {
  auto &display = M5.Display;
  display.setTextSize(size);
  int16_t x = (SCREEN_WIDTH - (text.length() * 6 * size)) / 2;
  if (x < 0) {
    x = 0;
  }
  display.setCursor(x, y);
  display.print(text);
}

String truncatedName(const String &name, size_t maxChars) {
  if (name.length() <= maxChars) {
    return name;
  }
  if (maxChars <= 3) {
    return name.substring(0, maxChars);
  }
  return name.substring(0, maxChars - 3) + "...";
}

String hostNameForDisplay() {
  if (g_role == Role::Host) {
    return g_localPlayerName;
  }
  if (g_role == Role::Client) {
    return g_remotePlayerName;
  }
  return g_localPlayerName;
}

String clientNameForDisplay() {
  if (g_role == Role::Host) {
    return g_remotePlayerName;
  }
  if (g_role == Role::Client) {
    return g_localPlayerName;
  }
  return g_remotePlayerName;
}

void drawRoleSelect() {
  drawRoleSelectFrame(0.0f);
}

void drawHostWaiting() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  drawCenteredText("Hosting Lobby", 18, 2);

  display.setTextSize(1);
  display.setCursor(12, 50);
  display.print("Player: ");
  display.print(g_localPlayerName);
  display.setCursor(12, 66);
  display.setCursor(12, 126);
  display.print(WiFi.localIP());
  display.setCursor(12, 82);
  display.print("Waiting for opponent...");
  display.setCursor(12, 98);
  display.print("Space = serve when ready");
  display.setCursor(12, 122);
  display.print("Q = back");
}

void drawClientSearching() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  drawCenteredText("Searching...", 18, 2);

  display.setTextSize(1);
  display.setCursor(12, 56);
  display.print("Looking on: ");
  display.print(g_wifiSSID);
  display.setCursor(12, 72);
  display.print("Host must be waiting");
  display.setCursor(12, 114);
  display.print("Q = back");
  display.setCursor(12, 126);
  display.print("; up  . dn move");
}

void drawWifiSelectScreen() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);

  drawCenteredText("Select WiFi", 14, 2);
  display.setTextSize(1);

  if (g_wifiNetworks.empty()) {
    display.setCursor(12, 56);
    display.print("No networks found. Press R to rescan.");
  } else {
    int visibleCount = std::min<int>(WIFI_MENU_VISIBLE_ROWS, static_cast<int>(g_wifiNetworks.size()));
    int firstIndex = g_wifiSelectedIndex - visibleCount / 2;
    if (firstIndex < 0) {
      firstIndex = 0;
    }
    if (firstIndex + visibleCount > static_cast<int>(g_wifiNetworks.size())) {
      firstIndex = std::max(0, static_cast<int>(g_wifiNetworks.size()) - visibleCount);
    }

    for (int i = 0; i < visibleCount; ++i) {
      int idx = firstIndex + i;
      int y = 48 + (i * 16);
      bool selected = idx == g_wifiSelectedIndex;
      if (selected) {
        display.fillRoundRect(10, y - 3, SCREEN_WIDTH - 20, 14, 2, COLOR_NET);
        display.setTextColor(COLOR_BLACK, COLOR_NET);
      } else {
        display.setTextColor(COLOR_WHITE, COLOR_BLACK);
      }
      const auto &info = g_wifiNetworks[idx];
      display.setCursor(14, y);
      String ssid = info.ssid;
      if (ssid.isEmpty()) {
        ssid = "<Hidden>";
      }
      display.print(truncatedName(ssid, 16));
      display.setCursor(SCREEN_WIDTH - 70, y);
      display.printf("%ddBm", info.rssi);
      if (info.authMode != WIFI_AUTH_OPEN) {
        display.setCursor(SCREEN_WIDTH - 110, y);
        display.print("Sec");
      }
    }
    display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  }

  if (!g_errorMessage.isEmpty()) {
    display.setTextColor(COLOR_RED, COLOR_BLACK);
    display.setCursor(12, SCREEN_HEIGHT - 46);
    display.print(g_errorMessage);
    display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  }

  display.setCursor(12, SCREEN_HEIGHT - 28);
  display.print("; up  . dn  Enter=go");
  display.setCursor(12, SCREEN_HEIGHT - 14);
  display.print("R=rescan  Q=back");
}

void drawWifiPasswordScreen() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);

  drawCenteredText("WiFi Password", 16, 2);
  display.setTextSize(1);
  display.setCursor(12, 52);
  display.print("SSID:");
  display.setCursor(12, 64);
  display.print(truncatedName(g_wifiSSID, 18));

  String shown = g_wifiPassword;
  if (!g_wifiPasswordVisible) {
    shown = "";
    shown.reserve(g_wifiPassword.length());
    for (size_t i = 0; i < static_cast<size_t>(g_wifiPassword.length()); ++i) {
      shown += '*';
    }
  }

  display.setCursor(12, 82);
  display.print("Pass:");
  display.setCursor(12, 94);
  display.print(truncatedName(shown, 18));

  display.setTextSize(1);
  int infoY = 110;
  if (!g_errorMessage.isEmpty()) {
    display.setTextColor(COLOR_RED, COLOR_BLACK);
    display.setCursor(12, infoY);
    display.print(g_errorMessage);
    display.setTextColor(COLOR_WHITE, COLOR_BLACK);
    infoY += 12;
  }
  display.setCursor(12, infoY);
  display.print("Enter=join  Bksp=del");
  display.setCursor(12, infoY + 12);
  display.print("Fn+Tab mask  Q=back");
}

void drawNameEntryScreen() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);

  drawCenteredText("Player Name", 14, 2);
  display.setTextSize(1);
  display.setCursor(12, 56);
  display.print("Enter the name to show opponents:");

  display.setCursor(12, 76);
  display.print(g_localPlayerName);

  display.setCursor(12, 118);
  display.print("Enter=continue  Backspace=erase  Fn+Q=WiFi");
}

void initMenuStars() {
  for (auto &star : g_menuStars) {
    star.x = static_cast<float>(random(SCREEN_WIDTH));
    star.y = static_cast<float>(random(SCREEN_HEIGHT));
    star.speed = 20.0f + static_cast<float>(random(20, 90));
  }
  g_menuStarsInitialized = true;
}

void drawRoleSelectFrame(float dtSeconds) {
  if (!g_menuStarsInitialized) {
    initMenuStars();
  }

  auto &display = M5.Display;
  display.startWrite();
  display.fillScreen(COLOR_BLACK);

  for (auto &star : g_menuStars) {
    star.y += star.speed * dtSeconds;
    if (star.y >= SCREEN_HEIGHT) {
      star.y = static_cast<float>(random(0, 12));
      star.x = static_cast<float>(random(SCREEN_WIDTH));
      star.speed = 20.0f + static_cast<float>(random(20, 90));
    }
    int x = static_cast<int>(star.x);
    int y = static_cast<int>(star.y);
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
      display.drawPixel(x, y, COLOR_NET);
      if (y + 1 < SCREEN_HEIGHT) {
        display.drawPixel(x, y + 1, COLOR_WHITE);
      }
    }
  }

  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  drawCenteredText("CardPing_Multi", 14, 2);

  display.setTextSize(1);
  display.setCursor(12, 48);
  display.print("Player: ");
  display.print(truncatedName(g_localPlayerName, 16));

  display.setCursor(12, 64);
  display.print("WiFi: ");
  if (WiFi.status() == WL_CONNECTED) {
    display.print(truncatedName(g_wifiSSID, 18));
  } else {
    display.print("not connected");
  }

  display.setCursor(12, 80);
  display.print("IP: ");
  display.print(WiFi.localIP());

  display.setCursor(12, 100);
  display.print("[H] Host match    [J] Join match");
  display.setCursor(12, 114);
  display.print("; up  . down  |  Space serve");
  display.setCursor(12, 126);
  display.print("Fn+Q reconfigure WiFi");
  display.endWrite();
}

void initConfetti() {
  static const uint16_t COLORS[] = {0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F};
  const long colorCount = static_cast<long>(sizeof(COLORS) / sizeof(COLORS[0]));
  for (auto &piece : g_confetti) {
    piece.x = static_cast<float>(random(SCREEN_WIDTH));
    piece.y = static_cast<float>(-random(SCREEN_HEIGHT));
    piece.vx = static_cast<float>(random(-30, 31)) * 0.6f;
    piece.vy = static_cast<float>(random(50, 121)) * 0.6f;
    piece.color = COLORS[random(colorCount)];
  }
  g_confettiActive = true;
}

void drawGameOverFrameAnimated(float dtSeconds) {
  if (!g_confettiActive) {
    initConfetti();
  }

  auto &display = M5.Display;
  display.startWrite();
  display.fillScreen(COLOR_BLACK);

  for (auto &piece : g_confetti) {
    piece.x += piece.vx * dtSeconds;
    piece.y += piece.vy * dtSeconds;
    piece.vy += 20.0f * dtSeconds;

    if (piece.x < 0.0f) {
      piece.x += SCREEN_WIDTH;
    } else if (piece.x >= SCREEN_WIDTH) {
      piece.x -= SCREEN_WIDTH;
    }

    if (piece.y >= SCREEN_HEIGHT) {
      piece.y = static_cast<float>(-random(10, 60));
      piece.x = static_cast<float>(random(SCREEN_WIDTH));
      piece.vx = static_cast<float>(random(-30, 31)) * 0.6f;
      piece.vy = static_cast<float>(random(60, 130)) * 0.6f;
    }

    int px = static_cast<int>(piece.x);
    int py = static_cast<int>(piece.y);
    if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
      display.drawPixel(px, py, piece.color);
    }
  }

  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  String hostName = hostNameForDisplay();
  String clientName = clientNameForDisplay();

  bool tie = g_hostScore == g_clientScore;
  String winnerLine;
  if (tie) {
    winnerLine = "Draw Game";
  } else if (g_hostScore > g_clientScore) {
    winnerLine = truncatedName(hostName, 16) + " wins!";
  } else {
    winnerLine = truncatedName(clientName, 16) + " wins!";
  }

  drawCenteredText(winnerLine, 16, 2);

  display.setTextSize(2);
  display.setCursor(60, 56);
  display.printf("%u", g_hostScore);
  display.setCursor(SCREEN_WIDTH - 60, 56);
  display.printf("%u", g_clientScore);

  display.setTextSize(1);
  display.setCursor(12, 84);
  display.print("Left: ");
  display.print(truncatedName(hostName, 14));
  display.setCursor(12, 100);
  display.print("Right: ");
  display.print(truncatedName(clientName, 14));

  display.setCursor(12, 118);
  if (g_role == Role::Host) {
    display.print("Space = rematch    Q = main menu");
  } else {
    display.print("Waiting for host. Q = main menu");
  }
  display.endWrite();
}

void drawLobby() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  drawCenteredText("Opponent Linked", 16, 2);

  display.setTextSize(1);
  display.setCursor(12, 48);
  display.print("You: ");
  display.print(truncatedName(g_localPlayerName, 18));
  display.setCursor(12, 64);
  display.print("Opponent: ");
  display.print(truncatedName(g_remotePlayerName, 18));

  if (g_role == Role::Host) {
    display.setCursor(12, 96);
    display.print("Space to serve the first ball.");
    display.setCursor(12, 112);
    display.print("; up    . down to move.");
  } else {
    display.setCursor(12, 96);
    display.print("Waiting for host to serve...");
    display.setCursor(12, 112);
    display.print("; up    . down to move.");
  }
  display.setCursor(12, 126);
  if (g_role == Role::Host) {
    display.print("Esc pause   Q leave lobby");
  } else {
    display.print("Press Q to leave lobby");
  }
}

void drawGameOver() {
  drawGameOverFrameAnimated(0.0f);
}

void drawErrorScreen() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_RED, COLOR_BLACK);
  drawCenteredText("Connection Error", 18, 2);
  display.setTextSize(1);
  display.setCursor(12, 62);
  display.print(g_errorMessage);
  display.setCursor(12, 90);
  display.print("Press Q to reconfigure");
}

void drawStaticScreen() {
  switch (g_screen) {
    case Screen::WifiSelect:
      drawWifiSelectScreen();
      break;
    case Screen::WifiPassword:
      drawWifiPasswordScreen();
      break;
    case Screen::NameEntry:
      drawNameEntryScreen();
      break;
    case Screen::RoleSelect:
      drawRoleSelect();
      break;
    case Screen::HostWaiting:
      drawHostWaiting();
      break;
    case Screen::ClientSearching:
      drawClientSearching();
      break;
    case Screen::Lobby:
      drawLobby();
      break;
    case Screen::GameOver:
      drawGameOver();
      break;
    case Screen::Error:
      drawErrorScreen();
      break;
    default:
      break;
  }
  g_screenDirty = false;
}

void onScreenEnter(Screen screen) {
  switch (screen) {
    case Screen::WifiSelect:
      g_errorMessage.clear();
      if (g_wifiNetworks.empty()) {
        scanAvailableNetworks();
      }
      break;
    case Screen::WifiPassword:
      g_errorMessage.clear();
      g_wifiPasswordVisible = false;
      break;
    case Screen::NameEntry:
      g_errorMessage.clear();
      break;
    case Screen::RoleSelect:
      g_menuStarsInitialized = false;
      initMenuStars();
      break;
    case Screen::GameOver:
      g_confettiActive = false;
      break;
    default:
      break;
  }
}

void drawGameFrame() {
  auto &display = M5.Display;
  display.startWrite();
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);

  // Midline net
  for (int y = 0; y < SCREEN_HEIGHT; y += 12) {
    display.drawFastVLine((SCREEN_WIDTH / 2) - 1, y, 6, COLOR_NET);
  }

  display.setTextSize(1);
  String hostName = truncatedName(hostNameForDisplay(), 12);
  String clientName = truncatedName(clientNameForDisplay(), 12);
  display.setCursor(12, 6);
  display.print(hostName);
  int clientWidth = static_cast<int>(clientName.length()) * 6;
  display.setCursor(SCREEN_WIDTH - clientWidth - 12, 6);
  display.print(clientName);

  // Scores
  display.setTextSize(2);
  display.setCursor(60, 8);
  display.printf("%u", g_hostScore);
  display.setCursor(SCREEN_WIDTH - 60, 8);
  display.printf("%u", g_clientScore);

  display.setTextSize(1);
  if (g_waitingForServe) {
    drawCenteredText("Serve ready...", 28, 1);
  }

  // Paddles
  int hostY = static_cast<int>(roundf(g_hostPaddleY - PADDLE_HALF_HEIGHT));
  int clientY = static_cast<int>(roundf(g_clientPaddleY - PADDLE_HALF_HEIGHT));
  display.fillRect(static_cast<int>(HOST_PADDLE_X), hostY, static_cast<int>(PADDLE_WIDTH), static_cast<int>(PADDLE_HEIGHT), COLOR_WHITE);
  display.fillRect(static_cast<int>(CLIENT_PADDLE_X), clientY, static_cast<int>(PADDLE_WIDTH), static_cast<int>(PADDLE_HEIGHT), COLOR_WHITE);

  // Ball
  int ballX = static_cast<int>(roundf(g_ballX - BALL_RADIUS));
  int ballY = static_cast<int>(roundf(g_ballY - BALL_RADIUS));
  display.fillCircle(ballX + static_cast<int>(BALL_RADIUS), ballY + static_cast<int>(BALL_RADIUS), static_cast<int>(BALL_RADIUS), COLOR_WHITE);

  display.endWrite();
}

void drawPauseOverlay() {
  auto &display = M5.Display;
  display.fillRoundRect(24, 40, SCREEN_WIDTH - 48, 56, 6, COLOR_BLACK);
  display.drawRoundRect(24, 40, SCREEN_WIDTH - 48, 56, 6, COLOR_WHITE);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  display.setTextSize(1);
  display.setCursor(38, 56);
  display.print("Game Paused");
  display.setCursor(30, 72);
  display.print("Esc resume   Q menu");
}

// -----------------------------------------------------------------------------
// Networking -----------------------------------------------------------------

void sendJoinBroadcast() {
  JoinPacket packet{};
  packet.type = static_cast<uint8_t>(PacketType::Join);
  memset(packet.name, 0, sizeof(packet.name));
  g_localPlayerName.toCharArray(packet.name, PLAYER_NAME_MAX_LEN);
  g_udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_PORT);
  g_udp.write(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  g_udp.endPacket();
}

void sendJoinAck() {
  if (!g_hasPeer) {
    return;
  }
  JoinAckPacket packet{};
  packet.type = static_cast<uint8_t>(PacketType::JoinAck);
  memset(packet.name, 0, sizeof(packet.name));
  g_localPlayerName.toCharArray(packet.name, PLAYER_NAME_MAX_LEN);
  g_udp.beginPacket(g_peerIp, g_peerPort);
  g_udp.write(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  g_udp.endPacket();
}

void sendStartPacket(uint32_t seed) {
  if (!g_hasPeer) {
    return;
  }
  StartPacket packet{static_cast<uint8_t>(PacketType::Start), seed};
  g_udp.beginPacket(g_peerIp, g_peerPort);
  g_udp.write(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  g_udp.endPacket();
}

void sendStatePacket() {
  if (!g_hasPeer || g_role != Role::Host) {
    return;
  }

  StatePacket packet{};
  packet.type = static_cast<uint8_t>(PacketType::State);
  packet.flags = 0;
  if (g_matchActive) {
    packet.flags |= FLAG_MATCH_ACTIVE;
  }
  if (g_waitingForServe) {
    packet.flags |= FLAG_WAITING_SERVE;
  }
  if (g_gameOver) {
    packet.flags |= FLAG_GAME_OVER;
  }
  if (g_gamePaused) {
    packet.flags |= FLAG_PAUSED;
  }
  packet.hostScore = g_hostScore;
  packet.clientScore = g_clientScore;
  packet.frameId = ++g_frameCounter;
  packet.ballX = g_ballX;
  packet.ballY = g_ballY;
  packet.ballVX = g_ballVX;
  packet.ballVY = g_ballVY;
  packet.hostPaddleY = g_hostPaddleY;
  packet.clientPaddleY = g_clientPaddleY;

  g_udp.beginPacket(g_peerIp, g_peerPort);
  g_udp.write(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  g_udp.endPacket();
  g_lastStateSent = millis();
}

void sendPaddlePacket() {
  if (!g_hasPeer || g_role != Role::Client) {
    return;
  }
  PaddlePacket packet{};
  packet.type = static_cast<uint8_t>(PacketType::Paddle);
  packet.paddleY = g_clientPaddleY;
  g_udp.beginPacket(g_peerIp, g_peerPort);
  g_udp.write(reinterpret_cast<uint8_t *>(&packet), sizeof(packet));
  g_udp.endPacket();
  g_lastPaddleSent = millis();
}

void processStatePacket(const StatePacket &packet) {
  g_hostScore = packet.hostScore;
  g_clientScore = packet.clientScore;
  g_ballX = packet.ballX;
  g_ballY = packet.ballY;
  g_ballVX = packet.ballVX;
  g_ballVY = packet.ballVY;
  g_hostPaddleY = packet.hostPaddleY;
  g_clientPaddleY = packet.clientPaddleY;

  bool wasGameOver = g_gameOver;

  g_gameOver = (packet.flags & FLAG_GAME_OVER) != 0;
  g_waitingForServe = (packet.flags & FLAG_WAITING_SERVE) != 0;
  g_gamePaused = (packet.flags & FLAG_PAUSED) != 0;
  g_matchActive = (packet.flags & FLAG_MATCH_ACTIVE) != 0 || g_waitingForServe;

  if (g_gameOver && !wasGameOver) {
    setScreen(Screen::GameOver);
  } else if (!g_gameOver && g_matchActive && g_screen != Screen::Playing) {
    setScreen(Screen::Playing);
  }
  g_lastStateReceived = millis();
}

void processNetwork() {
  int packetSize;
  while ((packetSize = g_udp.parsePacket()) > 0) {
    if (packetSize > 128) {
      while (g_udp.available()) {
        g_udp.read();
      }
      continue;
    }

    uint8_t buffer[128];
    int len = g_udp.read(buffer, packetSize);
    if (len <= 0) {
      continue;
    }

    PacketType type = static_cast<PacketType>(buffer[0]);
    switch (type) {
      case PacketType::Join:
        if (static_cast<size_t>(len) >= sizeof(JoinPacket) && g_role == Role::Host && g_screen == Screen::HostWaiting) {
          JoinPacket pkt;
          memcpy(&pkt, buffer, sizeof(JoinPacket));
          pkt.name[PLAYER_NAME_MAX_LEN - 1] = '\0';
          setRemotePlayerName(pkt.name);
          g_peerIp = g_udp.remoteIP();
          g_peerPort = g_udp.remotePort();
          g_hasPeer = true;
          sendJoinAck();
          resetMatchState();
          setScreen(Screen::Lobby);
        }
        break;
      case PacketType::JoinAck:
        if (static_cast<size_t>(len) >= sizeof(JoinAckPacket) && g_role == Role::Client && g_screen == Screen::ClientSearching) {
          JoinAckPacket pkt;
          memcpy(&pkt, buffer, sizeof(JoinAckPacket));
          pkt.name[PLAYER_NAME_MAX_LEN - 1] = '\0';
          setRemotePlayerName(pkt.name);
          g_peerIp = g_udp.remoteIP();
          g_peerPort = g_udp.remotePort();
          g_hasPeer = true;
          resetMatchState();
          setScreen(Screen::Lobby);
        }
        break;
      case PacketType::Start: {
        if (static_cast<size_t>(len) >= sizeof(StartPacket)) {
          StartPacket pkt;
          memcpy(&pkt, buffer, sizeof(StartPacket));
          if (g_role == Role::Host) {
            hostStartMatch(pkt.seed);
          } else {
            clientStartMatch(pkt.seed);
          }
        }
        break;
      }
      case PacketType::State:
        if (static_cast<size_t>(len) >= sizeof(StatePacket) && g_role == Role::Client) {
          StatePacket pkt;
          memcpy(&pkt, buffer, sizeof(StatePacket));
          processStatePacket(pkt);
        }
        break;
      case PacketType::Paddle:
        if (static_cast<size_t>(len) >= sizeof(PaddlePacket) && g_role == Role::Host && g_hasPeer) {
          PaddlePacket pkt;
          memcpy(&pkt, buffer, sizeof(PaddlePacket));
          g_clientPaddleY = clampValue(pkt.paddleY, PADDLE_HALF_HEIGHT, SCREEN_HEIGHT - PADDLE_HALF_HEIGHT);
        }
        break;
      default:
        break;
    }
  }
}

// -----------------------------------------------------------------------------
// Game logic -----------------------------------------------------------------

void handleConnectionTimeout() {
  if (!g_hasPeer) {
    return;
  }
  unsigned long now = millis();
  if (g_role == Role::Client && g_screen >= Screen::Playing) {
    if (now - g_lastStateReceived > CONNECTION_TIMEOUT_MS) {
      g_errorMessage = "Lost connection to host.";
      setScreen(Screen::Error);
      g_hasPeer = false;
    }
  }
  if (g_role == Role::Host && g_screen >= Screen::Lobby) {
    // Basic heartbeat: if we haven't heard a paddle update in a while, assume drop.
    // parsePacket already refreshed remote even if nothing arrives, so rely on state send difference.
    // For host we allow longer, as state packets go one-way.
    (void)now;
  }
}

void checkForServeLaunch() {
  if (g_waitingForServe) {
    if (millis() - g_serveRequestTs >= SERVE_DELAY_MS) {
      launchBall();
    }
  }
}

void updateHostGameplay(float dtSeconds) {
  if (!g_matchActive && !g_waitingForServe) {
    return;
  }

  if (cardKeyPressed(';')) {
    g_hostPaddleY -= PADDLE_SPEED * dtSeconds;
  }
  if (cardKeyPressed('.')) {
    g_hostPaddleY += PADDLE_SPEED * dtSeconds;
  }
  g_hostPaddleY = clampValue(g_hostPaddleY, PADDLE_HALF_HEIGHT, SCREEN_HEIGHT - PADDLE_HALF_HEIGHT);

  checkForServeLaunch();

  if (g_waitingForServe) {
    return;
  }

  g_ballX += g_ballVX * dtSeconds;
  g_ballY += g_ballVY * dtSeconds;

  if (g_ballY - BALL_RADIUS <= 0) {
    g_ballY = BALL_RADIUS;
    g_ballVY = -g_ballVY;
  }
  if (g_ballY + BALL_RADIUS >= SCREEN_HEIGHT) {
    g_ballY = SCREEN_HEIGHT - BALL_RADIUS;
    g_ballVY = -g_ballVY;
  }

  // Host paddle collision
  if (g_ballVX < 0) {
    float paddleLeft = HOST_PADDLE_X;
    float paddleRight = HOST_PADDLE_X + PADDLE_WIDTH;
    if (g_ballX - BALL_RADIUS <= paddleRight && g_ballX - BALL_RADIUS >= paddleLeft) {
      if (g_ballY >= g_hostPaddleY - PADDLE_HALF_HEIGHT && g_ballY <= g_hostPaddleY + PADDLE_HALF_HEIGHT) {
        g_ballX = paddleRight + BALL_RADIUS;
        g_ballVX = fabsf(g_ballVX) * BALL_SPEED_GROWTH;
        float offset = (g_ballY - g_hostPaddleY) / PADDLE_HALF_HEIGHT;
        g_ballVY += offset * 45.0f;
      }
    }
  }

  // Client paddle collision
  if (g_ballVX > 0) {
    float paddleLeft = CLIENT_PADDLE_X;
    float paddleRight = CLIENT_PADDLE_X + PADDLE_WIDTH;
    if (g_ballX + BALL_RADIUS >= paddleLeft && g_ballX + BALL_RADIUS <= paddleRight) {
      if (g_ballY >= g_clientPaddleY - PADDLE_HALF_HEIGHT && g_ballY <= g_clientPaddleY + PADDLE_HALF_HEIGHT) {
        g_ballX = paddleLeft - BALL_RADIUS;
        g_ballVX = -fabsf(g_ballVX) * BALL_SPEED_GROWTH;
        float offset = (g_ballY - g_clientPaddleY) / PADDLE_HALF_HEIGHT;
        g_ballVY += offset * 45.0f;
      }
    }
  }

  // Scoring
  if (g_ballX + BALL_RADIUS < 0) {
    ++g_clientScore;
    if (g_clientScore >= MAX_SCORE) {
      markGameOver();
      return;
    }
    prepareServe(1);
  } else if (g_ballX - BALL_RADIUS > SCREEN_WIDTH) {
    ++g_hostScore;
    if (g_hostScore >= MAX_SCORE) {
      markGameOver();
      return;
    }
    prepareServe(-1);
  }
}

void updateClientGameplay(float dtSeconds) {
  if (!g_hasPeer) {
    return;
  }

  bool moved = false;
  if (cardKeyPressed(';')) {
    g_clientPaddleY -= PADDLE_SPEED * dtSeconds;
    moved = true;
  }
  if (cardKeyPressed('.')) {
    g_clientPaddleY += PADDLE_SPEED * dtSeconds;
    moved = true;
  }
  g_clientPaddleY = clampValue(g_clientPaddleY, PADDLE_HALF_HEIGHT, SCREEN_HEIGHT - PADDLE_HALF_HEIGHT);

  unsigned long now = millis();
  if (moved || (now - g_lastPaddleSent) > PADDLE_SEND_INTERVAL_MS) {
    sendPaddlePacket();
  }
}

// -----------------------------------------------------------------------------
// Wi-Fi and session setup ----------------------------------------------------

void saveWifiCredentials() {
  if (g_wifiSSID.isEmpty()) {
    return;
  }
  if (!g_preferences.begin("cpong", false)) {
    return;
  }
  g_preferences.putString("ssid", g_wifiSSID);
  g_preferences.putString("pass", g_wifiPassword);
  g_preferences.end();
}

void loadWifiCredentials() {
  if (!g_preferences.begin("cpong", true)) {
    return;
  }
  String storedSsid = g_preferences.getString("ssid", "");
  String storedPass = g_preferences.getString("pass", "");
  g_preferences.end();
  if (!storedSsid.isEmpty()) {
    g_wifiSSID = storedSsid;
    g_wifiPassword = storedPass;
  }
}

void scanAvailableNetworks() {
  g_wifiNetworks.clear();
  g_wifiSelectedIndex = 0;

  int16_t count = WiFi.scanNetworks();
  if (count > 0) {
    g_wifiNetworks.reserve(static_cast<size_t>(count));
    for (int16_t i = 0; i < count; ++i) {
      WifiNetworkInfo info;
      info.ssid = WiFi.SSID(i);
      info.rssi = WiFi.RSSI(i);
      info.authMode = WiFi.encryptionType(i);
      g_wifiNetworks.push_back(info);
    }
    std::sort(g_wifiNetworks.begin(), g_wifiNetworks.end(), [](const WifiNetworkInfo &lhs, const WifiNetworkInfo &rhs) {
      return lhs.rssi > rhs.rssi;
    });
  } else {
    WifiNetworkInfo info;
    info.ssid = "(no networks)";
    info.rssi = -100;
    info.authMode = WIFI_AUTH_OPEN;
    info.isManual = true;
    g_wifiNetworks.push_back(info);
  }

  WiFi.scanDelete();
  g_screenDirty = true;
}

bool connectToWiFi() {
  auto &display = M5.Display;
  display.fillScreen(COLOR_BLACK);
  display.setTextColor(COLOR_WHITE, COLOR_BLACK);
  drawCenteredText("Connecting WiFi", 32, 2);
  display.setTextSize(1);
  display.setCursor(12, 70);
  display.print("SSID: ");
  display.print(g_wifiSSID);

  g_errorMessage.clear();

  if (g_wifiSSID.isEmpty()) {
    g_errorMessage = "No SSID selected.";
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  if (g_wifiPassword.isEmpty()) {
    WiFi.begin(g_wifiSSID.c_str());
  } else {
    WiFi.begin(g_wifiSSID.c_str(), g_wifiPassword.c_str());
  }
  WiFi.setSleep(false);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(120);
    M5.update();
    M5Cardputer.update();
    display.fillRect(12, 90, SCREEN_WIDTH - 24, 16, COLOR_BLACK);
    display.setCursor(12, 90);
    display.print("Status: ");
    display.print(WiFi.status());
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      g_errorMessage = "WiFi connect timeout.";
      return false;
    }
  }

  display.fillRect(12, 90, SCREEN_WIDTH - 24, 16, COLOR_BLACK);
  display.setCursor(12, 90);
  display.print("Connected!");
  delay(500);
  saveWifiCredentials();
  return true;
}

bool resetUdp() {
  g_udp.stop();
  if (!g_udp.begin(UDP_PORT)) {
    g_errorMessage = "UDP bind failed.";
    setScreen(Screen::Error);
    drawErrorScreen();
    return false;
  }
  return true;
}

void resetToMainMenu() {
  g_hasPeer = false;
  g_peerIp = IPAddress();
  g_peerPort = UDP_PORT;
  g_role = Role::None;
  resetMatchState();
  resetKeyLatch();
  g_remotePlayerName = "Opponent";
  setScreen(Screen::RoleSelect);
}

void resetToWifiSetup() {
  g_hasPeer = false;
  g_peerIp = IPAddress();
  g_peerPort = UDP_PORT;
  g_role = Role::None;
  resetMatchState();
  resetKeyLatch();
  g_remotePlayerName = "Opponent";
  scanAvailableNetworks();
  setScreen(Screen::WifiSelect);
}

void startHosting() {
  if (!resetUdp()) {
    return;
  }
  g_role = Role::Host;
  g_hasPeer = false;
  resetMatchState();
  g_remotePlayerName = "Opponent";
  setScreen(Screen::HostWaiting);
}

void startJoining() {
  if (!resetUdp()) {
    return;
  }
  g_role = Role::Client;
  g_hasPeer = false;
  resetMatchState();
  g_lastJoinBroadcast = 0;
  g_remotePlayerName = "Host";
  setScreen(Screen::ClientSearching);
}

void hostStartMatch(uint32_t seed) {
  (void)seed;
  randomSeed(seed);
  resetMatchState();
  prepareServe(1);
  setScreen(Screen::Playing);
  sendStatePacket();
}

void clientStartMatch(uint32_t seed) {
  (void)seed;
  randomSeed(seed);
  resetMatchState();
  g_waitingForServe = true;
  g_matchActive = true;
  g_serveDirection = 1;
  setScreen(Screen::Playing);
}

// -----------------------------------------------------------------------------
// Arduino main loop ----------------------------------------------------------

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5Cardputer.begin();
  M5.Display.setRotation(1);
  M5.Display.setTextColor(COLOR_WHITE, COLOR_BLACK);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  loadWifiCredentials();

  g_screenDirty = true;
  onScreenEnter(g_screen);
  if (!g_wifiSSID.isEmpty()) {
    if (connectToWiFi()) {
      g_errorMessage.clear();
      setScreen(Screen::NameEntry);
    } else {
      g_errorMessage = "Auto-connect failed.";
      g_wifiPassword.clear();
      WiFi.disconnect(true);
      setScreen(Screen::WifiSelect);
    }
  }
  drawStaticScreen();

  g_lastFrameTick = millis();
}

void loop() {
  M5.update();
  M5Cardputer.update();
  processNetwork();
  handleConnectionTimeout();

  unsigned long now = millis();
  float dt = (now - g_lastFrameTick) / 1000.0f;
  g_lastFrameTick = now;

  const auto &keysState = M5Cardputer.Keyboard.keysState();

  switch (g_screen) {
    case Screen::WifiSelect: {
      if (!g_wifiNetworks.empty()) {
        if (cardKeyJustPressed(';')) {
          if (g_wifiSelectedIndex > 0) {
            --g_wifiSelectedIndex;
            g_screenDirty = true;
            g_errorMessage.clear();
          }
        }
        if (cardKeyJustPressed('.')) {
          int maxIndex = static_cast<int>(g_wifiNetworks.size()) - 1;
          if (g_wifiSelectedIndex < maxIndex) {
            ++g_wifiSelectedIndex;
            g_screenDirty = true;
            g_errorMessage.clear();
          }
        }
      }

      if (cardKeyJustPressed('R')) {
        g_errorMessage.clear();
        scanAvailableNetworks();
      }

      if (cardKeyJustPressed('Q')) {
        if (WiFi.status() == WL_CONNECTED) {
          g_errorMessage.clear();
          setScreen(Screen::NameEntry);
        } else {
          g_errorMessage = "Connect to WiFi first.";
          g_screenDirty = true;
        }
      }

      if (cardKeyJustPressedAny({}, {HID_KEY_ENTER})) {
        if (!g_wifiNetworks.empty()) {
          int safeIndex = g_wifiSelectedIndex;
          safeIndex = clampValue(safeIndex, 0, static_cast<int>(g_wifiNetworks.size()) - 1);
          const auto &selected = g_wifiNetworks[static_cast<size_t>(safeIndex)];
          if (selected.isManual) {
            g_errorMessage = "No WiFi networks found.";
            g_screenDirty = true;
          } else {
            g_wifiSSID = selected.ssid;
            g_wifiPassword.clear();
            g_errorMessage.clear();
            setScreen(Screen::WifiPassword);
          }
        }
      }
      break;
    }
    case Screen::WifiPassword: {
      String previousPassword = g_wifiPassword;
      handleTextInput(g_wifiPassword, 63);
      if (previousPassword != g_wifiPassword) {
        g_screenDirty = true;
      }

      if (cardKeyJustPressed(static_cast<char>(KEY_TAB)) && keysState.fn) {
        g_wifiPasswordVisible = !g_wifiPasswordVisible;
        g_screenDirty = true;
      }

      if (cardKeyJustPressed('Q')) {
        setScreen(Screen::WifiSelect);
      } else if (cardKeyJustPressedAny({}, {HID_KEY_ENTER})) {
        if (connectToWiFi()) {
          g_errorMessage.clear();
          setScreen(Screen::NameEntry);
        } else {
          g_screenDirty = true;
        }
      }
      break;
    }
    case Screen::NameEntry: {
      String previousName = g_localPlayerName;
      handleTextInput(g_localPlayerName, PLAYER_NAME_MAX_LEN);
      if (previousName != g_localPlayerName) {
        g_screenDirty = true;
      }

      if (cardKeyJustPressed('Q') && keysState.fn) {
        resetToWifiSetup();
      } else if (cardKeyJustPressedAny({}, {HID_KEY_ENTER})) {
        String trimmed = g_localPlayerName;
        trimmed.trim();
        if (trimmed.isEmpty()) {
          trimmed = "Player";
        }
        if (trimmed != g_localPlayerName) {
          g_localPlayerName = trimmed;
        }
        setScreen(Screen::RoleSelect);
      }
      break;
    }
    case Screen::RoleSelect:
      drawRoleSelectFrame(dt);
      if (cardKeyJustPressed('H')) {
        startHosting();
      } else if (cardKeyJustPressed('J')) {
        startJoining();
      } else if (cardKeyJustPressed('Q') && keysState.fn) {
        resetToWifiSetup();
      }
      break;
    case Screen::HostWaiting:
      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
      }
      break;
    case Screen::ClientSearching: {
      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
        break;
      }
      if (now - g_lastJoinBroadcast > JOIN_BROADCAST_INTERVAL_MS) {
        sendJoinBroadcast();
        g_lastJoinBroadcast = now;
      }
      break;
    }
    case Screen::Lobby:
      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
      } else if (g_role == Role::Host && cardKeyJustPressed(' ')) {
        uint32_t seed = nextRandomSeed();
        sendStartPacket(seed);
        hostStartMatch(seed);
      }
      break;
    case Screen::Playing: {
      bool escJustPressed = cardKeyJustPressed(ASCII_ESC);
      if (escJustPressed && g_role == Role::Host) {
        g_gamePaused = !g_gamePaused;
      }

      if (g_role == Role::Host) {
        if (!g_gamePaused) {
          updateHostGameplay(dt);
        }
        if (escJustPressed || (now - g_lastStateSent > STATE_SEND_INTERVAL_MS)) {
          sendStatePacket();
        }
      } else {
        if (!g_gamePaused) {
          updateClientGameplay(dt);
        }
      }

      drawGameFrame();
      if (g_gamePaused) {
        drawPauseOverlay();
      }

      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
      }
      break;
    }
    case Screen::GameOver:
      drawGameOverFrameAnimated(dt);
      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
      } else if (g_role == Role::Host && cardKeyJustPressed(' ')) {
        uint32_t seed = nextRandomSeed();
        sendStartPacket(seed);
        hostStartMatch(seed);
      }
      break;
    case Screen::Error:
      if (cardKeyJustPressed('Q')) {
        resetToMainMenu();
      }
      break;
    default:
      break;
  }

  if (g_screen != Screen::Playing && g_screenDirty) {
    drawStaticScreen();
  }

  delay(g_frameDelayMs);
}