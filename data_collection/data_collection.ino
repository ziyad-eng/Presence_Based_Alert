#include <esp32cam.h>
#include <WiFi.h>
#include <HTTPClient.h> 

static const char* WIFI_SSID = "<wifi-name>";
static const char* WIFI_PASS = "<wifi password>";
const char* serverUrl = "http://<ip-address>/<port>";

esp32cam::Resolution initialResolution;
HTTPClient http;

void setup() {
  Serial.begin(115200);
  Serial.println();
  delay(1000);
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

  Serial.println("camera starting");
  Serial.print(serverUrl);
  
}

void loop(){

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi failure %d\n", WiFi.status());
    return;
  }

  auto frame = esp32cam::capture();
  if (frame == nullptr) {
    Serial.println("capture() failure");
    return;
  }
  Serial.printf("capture() success: %dx%d %zub\n", frame->getWidth(), frame->getHeight(),
                frame->size());

  http.begin(serverUrl);
  http.addHeader("content-type", "image/jpeg");
  Serial.println("Sending image to Flask...");
  int httpResponseCode = http.POST(frame->data(), frame->size());

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.printf("Server Response Code: %d\n", httpResponseCode);
      if(httpResponseCode == 200) {
        Serial.println("Image Posted!");
      }
  } else {
    Serial.printf("Error code: %d\n", httpResponseCode);
  }

  http.end();
  delay(500);

}
