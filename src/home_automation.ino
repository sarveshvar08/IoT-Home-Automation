/*
  ============================================================================
  6-Channel Smart Home Switch Controller
  ============================================================================
  Board target : ESP32 (DevKit / WROOM style boards)
  Cloud        : Arduino IoT Cloud
  Relays       : 6x, active-LOW modules (configurable)
  Switches     : 6x physical wall switches, wired to GPIO w/ INPUT_PULLUP

  DESIGN SUMMARY
  ----------------------------------------------------------------------------
  - relayState[] is the single source of truth for every channel.
  - A change can be requested from two places:
      1) Arduino IoT Cloud   -> onLightXChange() callback  -> updateRelay()
      2) A physical switch   -> handlePhysicalSwitches()   -> updateRelay()
  - updateRelay() is the ONLY function allowed to touch a relay GPIO. It also
    keeps the matching Cloud property in sync, so no matter which side
    initiated the change, the relay, the Cloud dashboard and the internal
    state array always agree.
  - Physical switches are read and debounced entirely in local memory —
    they never wait on Wi-Fi or the Cloud, so they keep working even if the
    network or Arduino IoT Cloud is completely unreachable.
  - On (re)connect, syncCloudStates() pushes the real relay states up to the
    Cloud so the dashboard never shows stale/incorrect values after an
    outage.

  ESP8266 PORTING NOTE
  ----------------------------------------------------------------------------
  This sketch is ESP32-first. To port to ESP8266:
    - Swap the pin numbers in relayPins[] / switchPins[] for valid ESP8266
      GPIOs (D1-D8 style, avoid D3/D4/D8 boot-strapping pins).
    - ESP8266 boards only have one usable ADC/analog pin and fewer free
      GPIOs, so you may need an I/O expander (e.g. PCF8574) for 6 switches
      + 6 relays. The logic below (updateRelay, handlePhysicalSwitches,
      syncCloudStates) does not need to change at all.
  ============================================================================
*/
#define WIFI_LED 2

unsigned long lastBlink = 0;
bool ledState = false;

#include "thingProperties.h"

// ============================== CONFIGURATION ==============================

#define NUM_CHANNELS 6          // Change this (and the pin arrays) to scale
                                 // up to 8/16 channels.

// Most cheap relay boards trigger the relay when the input pin is pulled
// LOW. Set to false if you have active-HIGH relay modules/solid state
// relays.
#define RELAY_ACTIVE_LOW true

// Debounce window in milliseconds (30-50ms recommended for mechanical
// switches).
#define DEBOUNCE_DELAY 40

// How often (ms) to retry Wi-Fi when disconnected.
#define WIFI_RETRY_INTERVAL 10000UL

// ---------------------------- GPIO ASSIGNMENT -------------------------------
// See README.md for the full safe-pin rationale. These avoid ESP32 boot
// strapping pins (0, 2, 12, 15), flash pins (6-11) and input-only pins
// (34-39) that cannot drive an output.

const uint8_t relayPins[NUM_CHANNELS]  = { 23, 22, 21, 19, 18, 5  };
const uint8_t switchPins[NUM_CHANNELS] = { 32, 33, 25, 26, 27, 14 };

// ============================== STATE ARRAYS ================================

bool relayState[NUM_CHANNELS] = { false, false, false, false, false, false };

bool          lastRawSwitchReading[NUM_CHANNELS];
bool          debouncedSwitchState[NUM_CHANNELS];
unsigned long lastDebounceTime[NUM_CHANNELS] = { 0 };

// Pointers into the Cloud-generated CloudLight properties (declared in
// thingProperties.h) so we can loop over all 6 channels generically instead
// of writing six near-identical if/else blocks. CloudLight supports direct
// assignment from / implicit conversion to bool, so it drops into the same
// pattern used for a plain bool property.
CloudLight* cloudLightPtr[NUM_CHANNELS] = {
  &light1, &light2, &light3, &light4, &light5, &light6
};

bool          wifiWasConnected = false;
unsigned long lastWiFiRetryAttempt = 0;

// ============================== FUNCTION PROTOTYPES =========================

void initGPIO();
void writeRelayPin(uint8_t idx, bool on);
void updateRelay(uint8_t idx, bool newState);
void handlePhysicalSwitches();
void syncCloudStates();
void reconnectWiFi();

// ================================== SETUP ====================================

void setup() {
  Serial.begin(115200);
  // A brief, bounded wait for the serial monitor is fine here (setup-only,
  // not part of the runtime loop) — no delay() is used in loop().
  unsigned long serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 2000)) { /* wait */ }

  Serial.println(F("\n[BOOT] 6-Channel Smart Home Switch Controller"));

  initGPIO();
  pinMode(WIFI_LED, OUTPUT);
  digitalWrite(WIFI_LED, LOW);   // LED OFF initially

  // Arduino IoT Cloud setup (generated helper from thingProperties.h)
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();

  // Push the real (post-GPIO-init) relay states to the cloud properties so
  // the dashboard reflects reality the moment a connection is established.
  syncCloudStates();

  Serial.println(F("[BOOT] Setup complete."));
}

// =================================== LOOP ====================================

void loop() {
  ArduinoCloud.update();     // Non-blocking; handles Cloud + Wi-Fi state machine
  handlePhysicalSwitches();  // Always runs, regardless of connectivity
  reconnectWiFi();           // Tracks reconnect events to trigger re-sync
}

// ================================ CORE LOGIC =================================

/**
 * Configure relay outputs and switch inputs, and seed the debounce state
 * arrays with the switches' power-up reading so we don't get a false
 * trigger on boot.
 */
void initGPIO() {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    pinMode(relayPins[i], OUTPUT);
    writeRelayPin(i, relayState[i]);   // all relays OFF at boot

    pinMode(switchPins[i], INPUT_PULLUP);
    bool raw = digitalRead(switchPins[i]);
    lastRawSwitchReading[i]  = raw;
    debouncedSwitchState[i]  = raw;
  }
}

/**
 * Drives the physical relay GPIO, honoring RELAY_ACTIVE_LOW.
 * This is the only function that ever calls digitalWrite() on a relay pin.
 */
void writeRelayPin(uint8_t idx, bool on) {
  digitalWrite(relayPins[idx], RELAY_ACTIVE_LOW ? !on : on);
}

/**
 * Single choke-point for changing a channel's state, no matter the source
 * (Cloud callback or physical switch). Keeps relay hardware, the internal
 * relayState[] truth table, and the Cloud property in lock-step.
 */
void updateRelay(uint8_t idx, bool newState) {
  if (idx >= NUM_CHANNELS) return;

  relayState[idx] = newState;
  writeRelayPin(idx, newState);

  // Only touch the Cloud property if it actually differs — avoids
  // redundant sync traffic when a Cloud-originated change calls back in
  // here with the value it already has.
  if ((bool)(*cloudLightPtr[idx]) != newState) {
    *cloudLightPtr[idx] = newState;
  }

  Serial.print(F("[RELAY] Channel "));
  Serial.print(idx + 1);
  Serial.println(newState ? F(" -> ON") : F(" -> OFF"));
}

/**
 * Reads all 6 switches with per-channel software debouncing and toggles the
 * matching relay on a validated state change. Toggling (rather than
 * "follow switch position") works correctly whether the wall switch is a
 * momentary push-button or a maintained toggle switch, and it correctly
 * handles the case where the Cloud has already changed the relay state
 * since the switch was last touched.
 *
 * This function never touches Wi-Fi or the Cloud API directly — it only
 * calls updateRelay(), so it works fully offline.
 */
void handlePhysicalSwitches() {
  unsigned long now = millis();

  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    bool raw = digitalRead(switchPins[i]);

    // Any change on the raw pin resets the debounce timer for that channel.
    if (raw != lastRawSwitchReading[i]) {
      lastDebounceTime[i] = now;
      lastRawSwitchReading[i] = raw;
    }

    // Reading has been stable for DEBOUNCE_DELAY -> accept it.
    if ((now - lastDebounceTime[i]) > DEBOUNCE_DELAY) {
      if (raw != debouncedSwitchState[i]) {
        debouncedSwitchState[i] = raw;

        // INPUT_PULLUP: pin reads LOW when the switch is actuated/pressed.
        // Trigger the toggle only on that actuation edge.
        if (raw == LOW) {
          updateRelay(i, !relayState[i]);
        }
      }
    }
  }
}

/**
 * Pushes the current (ground-truth) relayState[] array up to every Cloud
 * property. Called at boot and immediately after a Wi-Fi/Cloud reconnect so
 * the dashboard can never show a stale value after an outage.
 */
void syncCloudStates() {
  for (uint8_t i = 0; i < NUM_CHANNELS; i++) {
    *cloudLightPtr[i] = relayState[i];
  }
  Serial.println(F("[SYNC] Cloud properties synchronized with relay states."));
}

/**
 * ArduinoCloud.begin()/update() already manage the underlying Wi-Fi
 * connection state machine and automatic reconnection internally. This
 * function's job is simply to detect the disconnected->connected edge and
 * fire a state re-sync at that moment, and to log retries for visibility.
 */
void reconnectWiFi() {

  bool nowConnected = (ArduinoCloud.connected() == 1);

  if (nowConnected && !wifiWasConnected) {
    Serial.println(F("[WIFI] Connection (re)established."));
    syncCloudStates();
  }

  if (!nowConnected && wifiWasConnected) {
    Serial.println(F("[WIFI] Connection lost."));
  }

  // -------- LED STATUS --------
  if (nowConnected) {
    // Blink every 500 ms
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(WIFI_LED, ledState);
    }
  } else {
    // Glow continuously when disconnected
    digitalWrite(WIFI_LED, HIGH);
  }

  if (!nowConnected) {
    unsigned long now = millis();
    if (now - lastWiFiRetryAttempt >= WIFI_RETRY_INTERVAL) {
      lastWiFiRetryAttempt = now;
      Serial.println(F("[WIFI] Still disconnected..."));
    }
  }

  wifiWasConnected = nowConnected;
}

// ============================ CLOUD CHANGE CALLBACKS ==========================
// Arduino IoT Cloud requires one distinctly-named callback per property.
// Each one simply forwards to the shared updateRelay() function — no logic
// is duplicated six times.

void onLight1Change() { updateRelay(0, light1); }
void onLight2Change() { updateRelay(1, light2); }
void onLight3Change() { updateRelay(2, light3); }
void onLight4Change() { updateRelay(3, light4); }
void onLight5Change() { updateRelay(4, light5); }
void onLight6Change() { updateRelay(5, light6); }
