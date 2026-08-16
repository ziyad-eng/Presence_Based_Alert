#include <esp32cam.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <Ultrasonic.h>

static const char* WIFI_SSID = "<wifi-name>";
static const char* WIFI_PASS = "<wifi password>";
const char* serverHost = "<ip-address>";
const uint16_t serverPort = <port>;
const char* serverPath = "/burst";
const unsigned long SENSOR_INTERVAL = 60;
unsigned long lastSensorRead = 0;
int readIndex = 0;
long total = 0;
const int WINDOW_SIZE = 3;
int readings[WINDOW_SIZE];

const int BURST_COUNT = 5;
const char* boundary = "----ESP32Boundary7MA4YWxk";

int distance;


esp32cam::Resolution initialResolution;
Ultrasonic ultrasonic(13, 14);

void setup() {
  Serial.begin(115200);
  Serial.println();
  delay(1000);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    delay(5000);
    ESP.restart();
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.printf("WiFi failure %d\n", WiFi.status());
    delay(5000);
    ESP.restart();
  }
  Serial.println("WiFi connected");
  delay(1000);

  {
    using namespace esp32cam;
    initialResolution = Resolution::find(1024, 768);
    Config cfg;
    cfg.setPins(pins::AiThinker);
    cfg.setResolution(initialResolution);
    cfg.setJpeg(80);

    bool ok = Camera.begin(cfg);
    if (!ok) {
      Serial.println("camera initialize failure");
      delay(5000);
      ESP.restart();
    }
    Serial.println("camera initialize success");
  }
}

// Capture BURST_COUNT images and write each one to SPIFFS as /img_0.jpg .. /img_4.jpg
bool captureBurstToSpiffs() {
  for (int i = 0; i < BURST_COUNT; i++) {
    auto frame = esp32cam::capture();
    if (frame == nullptr) {
      Serial.printf("capture() failure on image %d\n", i);
      return false;
    }

    String path = "/img_" + String(i) + ".jpg";
    File f = SPIFFS.open(path, FILE_WRITE);
    if (!f) {
      Serial.printf("failed to open %s for writing\n", path.c_str());
      return false;
    }

    f.write(frame->data(), frame->size());
    f.close();

    Serial.printf("saved %s (%zu bytes)\n", path.c_str(), frame->size());
    delay(200); // small gap between captures
  }
  return true;
}

// we need a way to calculate the exact multipart body size in advance, because our HTTP needs Content-Length information before sending
size_t computeBodySize() {
  size_t total = 0;
  for (int i = 0; i < BURST_COUNT; i++) {
    String path = "/img_" + String(i) + ".jpg";
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) continue;
    size_t fileSize = f.size();
    f.close();

    String header = "--" + String(boundary) + "\r\n";
    header += "Content-Disposition: form-data; name=\"images\"; filename=\"img_" + String(i) + ".jpg\"\r\n";
    header += "Content-Type: image/jpeg\r\n\r\n";

    total += header.length();
    total += fileSize;
    total += 2; // trailing \r\n after each file's bytes
  }
  String closing = "--" + String(boundary) + "--\r\n";
  total += closing.length();
  return total;
}

// this will help us to build and send our multiparts with headers to our flask
bool sendBurstMultipart() {
  WiFiClient client;
  if (!client.connect(serverHost, serverPort)) {
    Serial.println("connection to server failed");
    return false;
  }

  size_t contentLength = computeBodySize();

  // instead of using HTTPClient library, we are customizing our own HTTP Post requests so this is the headers

  client.print(String("POST ") + serverPath + " HTTP/1.1\r\n");
  client.print(String("Host: ") + serverHost + "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + String(boundary) + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n"); 

  // this is the burst multipart for each image
  for (int i = 0; i < BURST_COUNT; i++) {
    String path = "/img_" + String(i) + ".jpg";
    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
      Serial.printf("failed to open %s for reading\n", path.c_str());
      continue;
    }

    // text part: boundary + headers (safe as String/print)
    client.print("--" + String(boundary) + "\r\n");
    client.print("Content-Disposition: form-data; name=\"images\"; filename=\"img_" + String(i) + ".jpg\"\r\n");
    client.print("Content-Type: image/jpeg\r\n\r\n");

    // binary part: raw bytes, streamed in chunks, never through String
    uint8_t buffer[512];
    while (f.available()) {
      size_t bytesRead = f.read(buffer, sizeof(buffer));
      client.write(buffer, bytesRead);
    }
    f.close();

    client.print("\r\n"); // end this part
  }

  // closing boundary
  client.print("--" + String(boundary) + "--\r\n");

  // --- read the server's response ---
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 10000) {
      Serial.println("response timeout");
      client.stop();
      return false;
    }
  }

  while (client.available()) {
    String line = client.readStringUntil('\n');
    Serial.println(line);
  }

  client.stop();
  return true;
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi failure %d\n", WiFi.status());
    delay(2000);
    return;
  }

  unsigned long now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL) {

    lastSensorRead = now;
    distance = ultrasonic.read();
    total -= readings[readIndex];
    readings[readIndex] = distance;
    total += readings[readIndex];
    readIndex = (readIndex + 1) % WINDOW_SIZE;
    distance = total / WINDOW_SIZE;

    if (distance < 60 && distance > 50) {
      Serial.println("capturing burst...");
      if (!captureBurstToSpiffs()) {
        Serial.println("burst capture failed, aborting send");
        delay(5000);
        return;
      }

      Serial.println("sending burst...");
      sendBurstMultipart();
    
    }
  }
}
