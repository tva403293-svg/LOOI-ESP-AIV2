#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <rom/rtc.h>
#include <HTTPClient.h>

// ── Server ──────────────────────────────────────────────────────────
const char* WS_HOST = "f0fd1c53-240c-4836-a0a2-4bf6391eb499-00-1zxt1e2huj9to.sisko.replit.dev";
const int   WS_PORT = 443;
const char* WS_PATH = "/ws/esp32";

// ── Hardware pins ───────────────────────────────────────────────────
#define MOTOR_A1       16
#define MOTOR_A2       17
#define MOTOR_B1       18
#define MOTOR_B2       8
#define SERVO_PIN      15
#define NEO_PIN        48
#define NEO_COUNT      1
// Direct speaker/amp output: GPIO 10 is driven by a high-frequency PWM
// carrier. The amplifier/speaker input turns the changing duty cycle into
// the PCM waveform, so no external I2S DAC is needed.
#define AUDIO_GPIO     10
#define AUDIO_PWM_FREQ 78125
#define AUDIO_PWM_RES  8
#define AUDIO_PWM_CH   4
#define MIC_I2S_PORT   I2S_NUM_1
#define MIC_BCLK_PIN   16
#define MIC_WS_PIN     17
#define MIC_SD_PIN     18

// LEDC PWM para sa motors
#define CH_A1  0
#define CH_A2  1
#define CH_B1  2
#define CH_B2  3
#define PWM_FREQ  5000
#define PWM_RES   8

Adafruit_NeoPixel pixels(NEO_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
WebSocketsClient webSocket;
Preferences prefs;

bool is_recording  = false;
bool isPlaying      = false;
bool isWSConnected  = false;
float currentVolume = 0.32f;
volatile float audioLevel = 0.0f;

#define MIC_RATE     16000
#define AUDIO_RATE   24000
#define MIC_CHUNK_SAMPLES 512
#define MAX_CHUNK_SIZE 16384
uint8_t tempBuffer[MAX_CHUNK_SIZE];
uint8_t b64DecodeBuf[MAX_CHUNK_SIZE];

const int START_THRESHOLD = 260;
int speech_frames = 0;
const int SPEECH_CONFIRM = 4;
const int SILENCE_CONFIRM = 12;       // ~384 ms at 512 samples / 16 kHz
const unsigned long MIN_STREAM_MS = 180;
int silence_frames = 0;
unsigned long micStreamStartedAt = 0;
unsigned long micFramesSent = 0;
unsigned long micBytesSent = 0;

unsigned long moveStopAt = 0;
bool motorsActive = false;
const unsigned long MOVE_PULSE_MS = 700;

// ── Network resilience ─────────────────────────────────────────────
int consecutiveFailures = 0;
const int MAX_FAILURES_BEFORE_RESTART = 30;

// Manual keepalive (para sa Globe idle timeout)
unsigned long lastKeepalive = 0;
const unsigned long KEEPALIVE_INTERVAL = 15000; // Keep proxy/NAT mappings warm

const IPAddress GOOGLE_DNS(8, 8, 8, 8);
const IPAddress CLOUDFLARE_DNS(1, 1, 1, 1);

// Debug mode: i-set to true para i-disable ang audio sending (test connection stability)
const bool AUDIO_TEST_MODE = false;
// Temporary hardware diagnostic. Set to false after confirming the 440 Hz
// tone is audible from the amplifier input.
const bool DIRECT_AUDIO_TONE_TEST = true;

// --------------------
// Debug helpers
// --------------------

void setColor(uint32_t color) {
  pixels.setPixelColor(0, color);
  pixels.show();
}

void printBootReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("[BOOT] Reset reason: ");
  switch (reason) {
    case ESP_RST_POWERON: Serial.println("Power-on"); break;
    case ESP_RST_SW: Serial.println("Software restart"); break;
    case ESP_RST_PANIC: Serial.println("Exception/panic"); break;
    case ESP_RST_BROWNOUT: Serial.println("Brownout (mahinang power!)"); break;
    case ESP_RST_WDT: Serial.println("Watchdog timeout"); break;
    default: Serial.println((int)reason); break;
  }
}

float calculateRMS(int16_t* buffer, size_t samples) {
  float sum = 0;
  for (size_t i = 0; i < samples; i++) { float s = buffer[i]; sum += s * s; }
  return sqrt(sum / samples);
}

// --------------------
// Motors (LEDC PWM)
// --------------------

void setupMotors() {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(MOTOR_A1, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_A2, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_B1, PWM_FREQ, PWM_RES);
    ledcAttach(MOTOR_B2, PWM_FREQ, PWM_RES);
  #else
    ledcSetup(CH_A1, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_A1, CH_A1);
    ledcSetup(CH_A2, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_A2, CH_A2);
    ledcSetup(CH_B1, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_B1, CH_B1);
    ledcSetup(CH_B2, PWM_FREQ, PWM_RES); ledcAttachPin(MOTOR_B2, CH_B2);
  #endif
}

void motorWrite(int pin, int duty) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(pin, duty);
  #else
    int ch = (pin == MOTOR_A1) ? CH_A1 : (pin == MOTOR_A2) ? CH_A2 :
             (pin == MOTOR_B1) ? CH_B1 : CH_B2;
    ledcWrite(ch, duty);
  #endif
}

void stopMotors() {
  motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, 0);
  motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, 0);
  motorsActive = false;
}

void driveMotors(const String& move, uint8_t speed) {
  if (speed == 0) speed = 128;
  stopMotors();
  delay(5);
  if (move == "FORWARD") {
    motorWrite(MOTOR_A1, speed); motorWrite(MOTOR_A2, 0);
    motorWrite(MOTOR_B1, speed); motorWrite(MOTOR_B2, 0);
  } else if (move == "BACKWARD") {
    motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, speed);
    motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, speed);
  } else if (move == "LEFT") {
    motorWrite(MOTOR_A1, 0); motorWrite(MOTOR_A2, speed);
    motorWrite(MOTOR_B1, speed); motorWrite(MOTOR_B2, 0);
  } else if (move == "RIGHT") {
    motorWrite(MOTOR_A1, speed); motorWrite(MOTOR_A2, 0);
    motorWrite(MOTOR_B1, 0); motorWrite(MOTOR_B2, speed);
  } else {
    stopMotors(); return;
  }
  motorsActive = true;
  moveStopAt = millis() + MOVE_PULSE_MS;
}

void applyLed(const String& led) {
  if (led == "LED_RED")        setColor(pixels.Color(255, 0, 0));
  else if (led == "LED_GREEN") setColor(pixels.Color(0, 255, 0));
  else if (led == "LED_BLUE")  setColor(pixels.Color(0, 0, 255));
  else if (led == "LED_WHITE") setColor(pixels.Color(255, 255, 255));
  else if (led == "LED_CYAN")  setColor(pixels.Color(0, 255, 255));
  else if (led == "LED_PURPLE")setColor(pixels.Color(150, 0, 255));
  else if (led == "LED_ORANGE")setColor(pixels.Color(255, 100, 0));
  else if (led == "LED_YELLOW")setColor(pixels.Color(255, 200, 0));
  else if (led == "LED_PINK")  setColor(pixels.Color(255, 0, 150));
  else if (led == "LED_ON")    setColor(pixels.Color(255, 255, 255));
  else if (led == "LED_OFF")   setColor(pixels.Color(0, 0, 0));
}

void handleRobotAction(const String& move, const String& led, int speed) {
  if (led.length()) applyLed(led);
  if (move.length() && move != "NONE") driveMotors(move, (uint8_t)speed);
}

// --------------------
// Base64 decode
// --------------------

int b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

size_t base64Decode(const char* in, size_t len, uint8_t* out, size_t outCap) {
  size_t o = 0;
  int val = 0, bits = -8;
  for (size_t i = 0; i < len; i++) {
    char c = in[i];
    if (c == '=' || c == '\0') break;
    int d = b64Val(c);
    if (d < 0) continue;
    val = (val << 6) + d;
    bits += 6;
    if (bits >= 0) {
      if (o >= outCap) break;
      out[o++] = (uint8_t)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return o;
}

// --------------------
// Tiny JSON helpers
// --------------------

String jsonGetString(const String& src, const char* key, int fromIndex = 0) {
  String needle = String("\"") + key + "\":\"";
  int i = src.indexOf(needle, fromIndex);
  if (i < 0) return "";
  int start = i + needle.length();
  int end = src.indexOf('"', start);
  while (end > 0 && src.charAt(end - 1) == '\\') end = src.indexOf('"', end + 1);
  if (end < 0) return "";
  return src.substring(start, end);
}

int jsonGetInt(const String& src, const char* key, int def = 0) {
  String needle = String("\"") + key + "\":";
  int i = src.indexOf(needle);
  if (i < 0) return def;
  int start = i + needle.length();
  int end = start;
  while (end < (int)src.length() && (isDigit(src.charAt(end)) || src.charAt(end) == '-')) end++;
  if (end == start) return def;
  return src.substring(start, end).toInt();
}

bool jsonHas(const String& src, const char* literal) {
  return src.indexOf(literal) >= 0;
}

// --------------------
// Audio
// --------------------

void streamMicChunk(int16_t* buf, size_t bytes) {
  webSocket.sendBIN((uint8_t*)buf, bytes);
}

void sendStreamEvent(const char* eventName) {
  String event = "{\"event\":\"";
  event += eventName;
  event += "\"}";
  webSocket.sendTXT(event);
}

void applyVolume(uint8_t* data, size_t len, float vol) {
  int16_t* samples = (int16_t*)data;
  for (size_t i = 0; i < len / 2; i++) {
    int32_t scaled = (int32_t)((float)samples[i] * vol);
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    samples[i] = (int16_t)scaled;
  }
}

float computeAudioLevel(uint8_t* data, size_t len) {
  const int count = len / sizeof(int16_t);
  float sum = 0;
  for (int i = 0; i < count; i++) {
    int16_t sample = 0;
    memcpy(&sample, data + (i * sizeof(int16_t)), sizeof(sample));
    float s = sample;
    sum += s * s;
  }
  return count ? sqrt(sum / count) : 0;
}

void audioPwmWrite(uint8_t duty) {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(AUDIO_GPIO, duty);
  #else
    ledcWrite(AUDIO_PWM_CH, duty);
  #endif
}

void setupDirectAudio() {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    if (!ledcAttach(AUDIO_GPIO, AUDIO_PWM_FREQ, AUDIO_PWM_RES)) {
      Serial.println("[ERROR] Direct audio PWM attach failed");
      return;
    }
  #else
    ledcSetup(AUDIO_PWM_CH, AUDIO_PWM_FREQ, AUDIO_PWM_RES);
    ledcAttachPin(AUDIO_GPIO, AUDIO_PWM_CH);
  #endif

  // 50% duty is the silent midpoint for signed PCM.
  audioPwmWrite(128);
  Serial.printf("[INIT] Direct GPIO audio OK (GPIO %d, PWM %dHz / %dbit, PCM %dHz)\n",
                AUDIO_GPIO, AUDIO_PWM_FREQ, AUDIO_PWM_RES, AUDIO_RATE);
}

void playDirectToneTest() {
  Serial.println("[AUDIO TEST] GPIO 10: 440Hz tone for 1 second");
  const uint16_t halfPeriodUs = 1136; // approximately 440 Hz
  for (int i = 0; i < 440; i++) {
    audioPwmWrite(255);
    delayMicroseconds(halfPeriodUs);
    audioPwmWrite(0);
    delayMicroseconds(halfPeriodUs);
  }
  audioPwmWrite(128);
  Serial.println("[AUDIO TEST] Tone ended");
}

void writePcmToGpio(const uint8_t* data, size_t len) {
  // Gemini sends little-endian signed 16-bit PCM at 24 kHz. Each sample is
  // represented by the duty cycle of a much faster PWM carrier. This is a
  // DAC-less output path: connect GPIO 10 through a small series capacitor
  // (and preferably a 1k resistor) to the amplifier's audio input.
  const size_t sampleCount = len / sizeof(int16_t);
  if (sampleCount == 0) return;

  const uint32_t samplePeriodUs = 1000000UL / AUDIO_RATE;
  uint32_t nextSampleAt = micros();
  const int16_t* samples = reinterpret_cast<const int16_t*>(data);

  for (size_t i = 0; i < sampleCount; i++) {
    // Convert signed 16-bit PCM [-32768, 32767] to unsigned 8-bit duty.
    const int32_t unsignedSample = (int32_t)samples[i] + 32768;
    audioPwmWrite((uint8_t)(unsignedSample >> 8));

    nextSampleAt += samplePeriodUs;
    const int32_t waitUs = (int32_t)(nextSampleAt - micros());
    if (waitUs > 0) {
      delayMicroseconds((uint32_t)waitUs);
    } else {
      // If a callback briefly overruns, restart the schedule instead of
      // accumulating delay and making the next frames increasingly late.
      nextSampleAt = micros();
    }
  }
}

// --------------------
// WebSocket
// --------------------

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      isWSConnected = false;
      consecutiveFailures++;
      if (is_recording) {
        Serial.printf("[MIC] STREAM_ABORTED: WebSocket disconnected after %lu frame(s)\n",
                      micFramesSent);
        is_recording = false;
        speech_frames = 0;
        silence_frames = 0;
      }
      Serial.printf("[WS] Disconnected (failure #%d, library retry interval active)\n",
                    consecutiveFailures);
      setColor(pixels.Color(100, 0, 0));

      if (consecutiveFailures >= MAX_FAILURES_BEFORE_RESTART) {
        Serial.println("[NET] Too many failures, restarting ESP32...");
        delay(1000);
        ESP.restart();
      }
      break;

    case WStype_CONNECTED: {
      isWSConnected = true;
      consecutiveFailures = 0;
      lastKeepalive = millis(); // Reset keepalive timer
      Serial.println("[WS] Connected ✓");
      setColor(pixels.Color(0, 0, 100));
      
      // Send device hello
      webSocket.sendTXT("{\"deviceHello\":{\"device\":\"alexatron-esp32s3\"}}");
      break;
    }

    case WStype_TEXT: {
      String msg((char*)payload, length);
      Serial.println("[WS] TXT: " + msg.substring(0, min((int)msg.length(), 150)));
      
      // Ignore serverHello/keepalive responses
      if (jsonHas(msg, "\"serverHello\"") || jsonHas(msg, "\"pong\"")) {
        Serial.println("[WS] Server hello/keepalive ack");
        break;
      }
      
      if (jsonHas(msg, "\"robotAction\"")) {
        String move = jsonGetString(msg, "move");
        String led  = jsonGetString(msg, "led");
        int speed    = jsonGetInt(msg, "speed", 128);
        handleRobotAction(move, led, speed);
        break;
      }
      if (jsonHas(msg, "\"error\"")) {
        Serial.println("[Server error] " + jsonGetString(msg, "error"));
        setColor(pixels.Color(100, 0, 0));
        // Huwag mag-disconnect agad — hintayin kung magre-retry ang server
        break;
      }
      if (jsonHas(msg, "\"interrupted\":true")) {
        isPlaying = false;
        Serial.println("[AUDIO] PLAYBACK_INTERRUPTED");
        setColor(pixels.Color(0, 0, 100));
        break;
      }
      if (jsonHas(msg, "\"inlineData\"")) {
        isPlaying = true;
        setColor(pixels.Color(200, 0, 200));
        String b64 = jsonGetString(msg, "data");
        if (b64.length()) {
          size_t decoded = base64Decode(b64.c_str(), b64.length(), b64DecodeBuf, MAX_CHUNK_SIZE);
          if (decoded > 0) {
            uint8_t* p = b64DecodeBuf;
            if (currentVolume != 1.0f && decoded <= MAX_CHUNK_SIZE) {
              memcpy(tempBuffer, b64DecodeBuf, decoded);
              applyVolume(tempBuffer, decoded, currentVolume);
              p = tempBuffer;
            }
            audioLevel = computeAudioLevel(p, decoded);
            writePcmToGpio(p, decoded);
          }
        }
      }
      if (jsonHas(msg, "\"turnComplete\":true")) {
        isPlaying = false;
        Serial.println("[AUDIO] PLAYBACK_END (Gemini turn complete)");
        setColor(pixels.Color(0, 0, 100));
      }
      break;
    }

    case WStype_BIN: {
      static unsigned long audioFramesReceived = 0;
      if (length == 0) break;

      isPlaying = true;
      setColor(pixels.Color(200, 0, 200));
      audioFramesReceived++;
      if (audioFramesReceived <= 3 || audioFramesReceived % 10 == 0) {
        Serial.printf("[WS] AI audio frame #%lu (%u bytes, RMS=%.0f)\n",
                      audioFramesReceived, (unsigned)length,
                      computeAudioLevel(payload, length));
      }

      writePcmToGpio(payload, length);
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WS] Error event: %s\n", payload ? (char*)payload : "unknown");
      break;
  }
}

// --------------------
// Network Diagnostics
// --------------------

bool checkInternetConnectivity() {
  Serial.println("[NET] Testing HTTP connectivity...");
  HTTPClient http;
  http.setTimeout(5000);
  // Use HTTPS para same path as WS
  http.begin(String("https://") + WS_HOST + "/health");
  int httpCode = http.GET();
  http.end();
  
  if (httpCode == 200) {
    Serial.println("[NET] HTTP test ✓ (Server reachable)");
    return true;
  } else {
    Serial.printf("[NET] HTTP test ✗ (code: %d)\n", httpCode);
    return false;
  }
}

bool resolveHost() {
  IPAddress resolvedIP;
  Serial.printf("[NET] Resolving %s...\n", WS_HOST);
  
  if (WiFi.hostByName(WS_HOST, resolvedIP)) {
    Serial.printf("[NET] Resolved to: %s\n", resolvedIP.toString().c_str());
    return true;
  } else {
    Serial.println("[NET] DNS resolution FAILED");
    return false;
  }
}

// --------------------
// Setup
// --------------------

void setup() {
  Serial.begin(115200);
  unsigned long serialTimeout = millis();
  while (!Serial && (millis() - serialTimeout < 3000)) { delay(10); }
  delay(500);

  Serial.println("\n\n═══════════════════════════════════════");
  Serial.println("  ALEXATRON BOOT");
  Serial.println("═══════════════════════════════════════");
  printBootReason();

  pixels.begin();
  setColor(pixels.Color(50, 50, 0));
  Serial.println("[INIT] NeoPixel OK");

  prefs.begin("alexatron", false);
  currentVolume = prefs.getFloat("volume", 0.32f);
  Serial.printf("[INIT] Volume: %.2f\n", currentVolume);

  setupMotors();
  stopMotors();
  Serial.println("[INIT] Motors OK (LEDC)");

  Serial.println("[INIT] Skipping servo (debug mode)");
  
  Serial.println("[INIT] Creating WiFiManager...");
  WiFiManager wm;
  // wm.resetSettings();
  
  wm.setConfigPortalTimeout(180);
  Serial.println("[INIT] Starting autoConnect...");
  
  if (!wm.autoConnect("Alexatron")) {
    Serial.println("[INIT] WiFi failed, restarting...");
    delay(2000);
    ESP.restart();
  }
  
  Serial.println("[INIT] WiFi connected: " + WiFi.localIP().toString());
  
  Serial.println("\n[NET] === Network Diagnostics ===");
  
  WiFi.setDNS(GOOGLE_DNS, CLOUDFLARE_DNS);
  Serial.println("[NET] DNS set to 8.8.8.8, 1.1.1.1");
  
  if (!resolveHost()) {
    Serial.println("[NET] WARNING: Cannot resolve server hostname!");
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }
  
  if (!checkInternetConnectivity()) {
    Serial.println("[NET] WARNING: Server HTTPS not reachable!");
    setColor(pixels.Color(255, 50, 0));
    delay(3000);
  }
  
  Serial.println("[NET] =============================\n");

  Serial.println("[INIT] Installing Mic I2S...");
  i2s_config_t mic_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = MIC_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256
  };
  i2s_pin_config_t mic_p = {
    .bck_io_num = MIC_BCLK_PIN,
    .ws_io_num = MIC_WS_PIN,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = MIC_SD_PIN
  };
  esp_err_t err = i2s_driver_install(MIC_I2S_PORT, &mic_cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Mic I2S install failed: %d\n", err);
    setColor(pixels.Color(255, 0, 0));
    while (true) { delay(500); }
  }
  i2s_set_pin(MIC_I2S_PORT, &mic_p);
  Serial.println("[INIT] Mic I2S OK");

  setupDirectAudio();
  if (DIRECT_AUDIO_TONE_TEST) playDirectToneTest();

  Serial.print("[INIT] Free heap before WS: ");
  Serial.println(ESP.getFreeHeap());
  
  // REMOVED: enableHeartbeat — buggy sa lumang WebSockets library
  // REMOVED: setInsecure — not supported
  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  // WebSocketsClient owns reconnect attempts. Do not call beginSSL()/disconnect()
  // again from loop(), otherwise two reconnect state machines race each other.
  webSocket.setReconnectInterval(3000);

  setColor(pixels.Color(0, 0, 100));
  Serial.println("[INIT] Setup complete!");
  Serial.printf("[INIT] AUDIO_TEST_MODE = %s\n\n", AUDIO_TEST_MODE ? "ON (no audio)" : "OFF");
}

// --------------------
// Loop
// --------------------

void loop() {
  webSocket.loop();

  if (motorsActive && millis() > moveStopAt) stopMotors();

  // ── Manual keepalive (para sa Globe idle timeout) ──
  if (isWSConnected && (millis() - lastKeepalive >= KEEPALIVE_INTERVAL)) {
    lastKeepalive = millis();
    // Send lightweight keepalive — para hindi ma-idle timeout ng router
    webSocket.sendTXT("{\"ping\":1}");
    Serial.println("[NET] Keepalive sent");
  }

  if (!isWSConnected) return;
  if (isPlaying) return;
  
  // ── TEST MODE: Skip audio sending ──
  if (AUDIO_TEST_MODE) {
    // Just keep connection alive, don't send audio
    return;
  }

  int16_t sample_buffer[MIC_CHUNK_SAMPLES];
  size_t bytes_read = 0;
  i2s_read(MIC_I2S_PORT, sample_buffer, sizeof(sample_buffer), &bytes_read, 10);
  if (bytes_read == 0) return;

  float rms = calculateRMS(sample_buffer, bytes_read / 2);

  if (!is_recording) {
    if (rms > START_THRESHOLD) speech_frames++;
    else speech_frames = 0;
    if (speech_frames >= SPEECH_CONFIRM) {
      is_recording = true;
      speech_frames = 0;
      silence_frames = 0;
      micStreamStartedAt = millis();
      micFramesSent = 0;
      micBytesSent = 0;
      Serial.printf("[MIC] START_STREAM rms=%.0f threshold=%d\n", rms, START_THRESHOLD);
      sendStreamEvent("start_stream");
      setColor(pixels.Color(0, 255, 255));
    }
  }

  // Only send audio inside a detected speech turn. Gemini needs the explicit
  // end_stream event below to know when it can generate a response.
  if (is_recording) {
    streamMicChunk(sample_buffer, bytes_read);
    micFramesSent++;
    micBytesSent += bytes_read;

    if (rms < START_THRESHOLD * 0.6f) {
      silence_frames++;
    } else {
      silence_frames = 0;
    }

    if (silence_frames >= SILENCE_CONFIRM &&
        millis() - micStreamStartedAt >= MIN_STREAM_MS) {
      is_recording = false;
      Serial.printf("[MIC] END_STREAM rms=%.0f silence_frames=%d frames=%lu bytes=%lu duration=%lums\n",
                    rms, silence_frames, micFramesSent, micBytesSent,
                    millis() - micStreamStartedAt);
      sendStreamEvent("end_stream");
      setColor(pixels.Color(0, 0, 100));
    }
  }
}
