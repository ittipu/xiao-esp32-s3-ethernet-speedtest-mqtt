/*
 * ============================================================================
 *  IoT Bhai — Part 1: XIAO ESP32-S3 PoE -> Ethernet -> Internet -> MQTT
 *  Ethernet only (W5500 via native ETH.h). No WiFi in this part.
 * ----------------------------------------------------------------------------
 *  Demo flow (matches the video):
 *    1. Plug the Ethernet cable (PoE or USB-powered) and open Serial Monitor
 *    2. Board gets an IP over DHCP            -> printed (IP, gateway, DNS, MAC)
 *    3. Internet check: DNS lookup + HTTP request to prove real internet access
 *    4. Connect to mqtt.iotbhai.io:1883
 *    5. Publish JSON every 2 s -> watch it live in the MQTT Bhai app
 *    6. SPEED TEST: publish  speed_test  to  iotbhai/cmd
 *       -> board downloads a 100 MB file over Ethernet and replies on
 *          iotbhai/speed  with the measured speed (Mbps)
 *
 *  Libraries: PubSubClient.  Board: XIAO_ESP32S3 (arduino-esp32 core 3.1.0+)
 *  W5500 pins (Seeed wiki): CS=D1 SCK=D8 MISO=D9 MOSI=D10
 * ============================================================================
 */

#include <SPI.h>
#include <ETH.h>
#include <WiFi.h>            // only for hostByName(); WiFi is never started in Part 1
#include <NetworkClient.h>
#include <PubSubClient.h>

// ============================== USER CONFIG =================================
const char* MQTT_HOST   = "mqtt.iotbhai.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER   = "";                 // "" if broker is open
const char* MQTT_PASS   = "";
const char* MQTT_TOPIC  = "iotbhai/node";
const char* MQTT_CLIENT = "xiao-s3-part1";

const uint32_t PUBLISH_INTERVAL_MS = 2000;

// --- Speed test (MQTT command) ---
const char* CMD_TOPIC    = "iotbhai/cmd";     // send: speed_test
const char* RESULT_TOPIC = "iotbhai/speed";   // reply: JSON
// Plain HTTP on purpose (no TLS overhead = measures the link, not the CPU).
const char* SPEED_HOST   = "speedtest.tele2.net";
const char* SPEED_PATH   = "/100MB.zip";
const uint32_t SPEED_BYTES_MAX = 100UL * 1024UL * 1024UL;   // stop after 100 MB
const uint32_t SPEED_TIMEOUT_MS = 180000;                   // give up after 3 min

// --- W5500 pins (confirmed from Seeed wiki demo) ---
#define ETH_PHY_TYPE  ETH_PHY_W5500
#define ETH_PHY_ADDR  1
#define ETH_PHY_CS    D1
#define ETH_PHY_IRQ   -1
#define ETH_PHY_RST   -1
#define ETH_SPI_SCK   D8
#define ETH_SPI_MISO  D9
#define ETH_SPI_MOSI  D10
// ===========================================================================

NetworkClient netClient;
PubSubClient  mqtt(netClient);

volatile bool eth_up = false;
uint32_t pubCount = 0;
volatile bool speedTestRequested = false;   // set by the MQTT callback, run from loop()

void onNetEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:        ETH.setHostname("iotbhai-part1"); break;
    case ARDUINO_EVENT_ETH_CONNECTED:    Serial.println("[eth] cable connected, link up"); break;
    case ARDUINO_EVENT_ETH_GOT_IP:       eth_up = true; break;
    case ARDUINO_EVENT_ETH_LOST_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED: Serial.println("[eth] link down"); eth_up = false; break;
    case ARDUINO_EVENT_ETH_STOP:         eth_up = false; break;
    default: break;
  }
}

// Returns whatever you measure. Placeholder until the sensor is chosen.
float readSensor() {
  return (float)(millis() / 1000);
}

// Proof of real internet access: DNS resolve + plain HTTP request.
bool internetCheck() {
  IPAddress ip;
  Serial.print("[net] DNS lookup example.com ... ");
  if (!WiFi.hostByName("example.com", ip)) {   // resolves via the active lwIP interface (ETH)
    Serial.println("FAILED");
    return false;
  }
  Serial.println(ip);

  NetworkClient c;
  c.setTimeout(5000);
  Serial.print("[net] HTTP GET example.com ... ");
  if (!c.connect(ip, 80)) { Serial.println("connect FAILED"); return false; }
  c.print("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
  String status = c.readStringUntil('\n');
  c.stop();
  status.trim();
  Serial.println(status);                       // expect "HTTP/1.1 200 OK"
  return status.indexOf("200") > 0;
}

bool mqttConnect() {
  if (mqtt.connected()) return true;
  bool ok = (strlen(MQTT_USER) > 0)
              ? mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASS)
              : mqtt.connect(MQTT_CLIENT);
  Serial.printf("[mqtt] connect %s:%u : %s\n", MQTT_HOST, MQTT_PORT, ok ? "OK" : "fail");
  if (ok) mqtt.subscribe(CMD_TOPIC);
  return ok;
}

// Incoming command. Runs inside mqtt.loop(), so only set a flag here.
void onMqttMessage(char* topic, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len && i < 32; i++) cmd += (char)payload[i];
  cmd.trim();
  cmd.toLowerCase();
  Serial.printf("[cmd] %s\n", cmd.c_str());
  if (cmd == "speed_test") speedTestRequested = true;
}

void publishResult(const char* json) {
  mqtt.publish(RESULT_TOPIC, json);
  Serial.printf("[speed] %s\n", json);
}

// Downloads up to 100 MB over HTTP, discards the data, reports the speed.
// mqtt.loop() is called while downloading so the broker connection stays alive.
void runSpeedTest() {
  publishResult("{\"status\":\"running\",\"link\":\"ETH\"}");

  NetworkClient c;
  c.setTimeout(5000);
  if (!c.connect(SPEED_HOST, 80)) {
    publishResult("{\"status\":\"error\",\"msg\":\"connect failed\"}");
    return;
  }
  c.printf("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", SPEED_PATH, SPEED_HOST);

  String status = c.readStringUntil('\n');            // e.g. "HTTP/1.1 200 OK"
  if (status.indexOf(" 200") < 0) {
    status.trim();
    char err[96];
    snprintf(err, sizeof(err), "{\"status\":\"error\",\"msg\":\"%.40s\"}", status.c_str());
    publishResult(err);
    c.stop();
    return;
  }
  while (c.connected()) {                             // skip the remaining headers
    String h = c.readStringUntil('\n');
    if (h == "\r" || h.length() == 0) break;
  }

  static uint8_t buf[4096];
  uint32_t total = 0;
  uint32_t t0 = millis(), lastLoop = t0, lastPrint = t0;

  while ((c.connected() || c.available()) && total < SPEED_BYTES_MAX &&
         millis() - t0 < SPEED_TIMEOUT_MS) {
    int n = c.read(buf, sizeof(buf));
    if (n > 0) total += n; else delay(1);
    if (millis() - lastLoop > 1000) { lastLoop = millis(); mqtt.loop(); }
    if (millis() - lastPrint > 2000) {
      lastPrint = millis();
      Serial.printf("[speed] %.1f MB ...\n", total / 1048576.0);
    }
  }
  uint32_t ms = millis() - t0;
  c.stop();

  float mbps = (ms > 0) ? (total * 8.0f) / (ms * 1000.0f) : 0;   // megabits per second
  char out[200];
  snprintf(out, sizeof(out),
    "{\"status\":\"done\",\"link\":\"ETH\",\"mbps\":%.2f,\"mb_per_s\":%.2f,\"bytes\":%lu,\"ms\":%lu}",
    mbps, mbps / 8.0f, (unsigned long)total, (unsigned long)ms);
  publishResult(out);
}

void publish(float value) {
  char payload[200];
  snprintf(payload, sizeof(payload),
    "{\"link\":\"ETH\",\"sensor\":%.1f,\"count\":%lu,\"wifi\":0,\"eth\":%d}",
    value, (unsigned long)(pubCount + 1), eth_up ? 1 : 0);
  if (mqtt.publish(MQTT_TOPIC, payload)) {
    pubCount++;
    Serial.printf("[mqtt] %s\n", payload);
  } else {
    Serial.println("[mqtt] publish failed");
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== IoT Bhai - Part 1: Ethernet -> MQTT ===");

  Network.onEvent(onNetEvent);
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_CS, ETH_PHY_IRQ, ETH_PHY_RST, SPI);

  Serial.print("waiting for Ethernet (plug the cable in)");
  while (!eth_up) { delay(300); Serial.print("."); }

  Serial.println();
  Serial.print("MAC     : "); Serial.println(ETH.macAddress());
  Serial.print("IP      : "); Serial.println(ETH.localIP());
  Serial.print("Gateway : "); Serial.println(ETH.gatewayIP());
  Serial.print("DNS     : "); Serial.println(ETH.dnsIP());
  Serial.printf("Speed   : %u Mbps, %s duplex\n", ETH.linkSpeed(), ETH.fullDuplex() ? "full" : "half");

  while (!internetCheck()) {                    // keep trying until internet is confirmed
    Serial.println("[net] no internet yet, retrying in 3 s");
    delay(3000);
  }
  Serial.println("[net] internet OK");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(10);
  mqtt.setCallback(onMqttMessage);
}

void loop() {
  static uint32_t last = 0;
  mqtt.loop();

  if (speedTestRequested) {                     // runs here, not inside the callback
    speedTestRequested = false;
    if (eth_up && mqtt.connected()) runSpeedTest();
    last = millis();                            // restart the publish timer after the test
  }

  if (millis() - last >= PUBLISH_INTERVAL_MS) {
    last = millis();
    if (!eth_up) { Serial.println("[eth] no link, waiting..."); return; }
    if (mqttConnect()) publish(readSensor());
  }
}
