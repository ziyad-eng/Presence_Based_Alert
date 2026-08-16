#include <driver/i2s.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <stdio.h>

#define I2S_NUM         I2S_NUM_0
#define I2S_SCK         2
#define I2S_WS          15
#define I2S_SD          4

#define I2S_NUM2         I2S_NUM_1
#define I2S_SCK2         19
#define I2S_WS2          5
#define DIN              18
#define SAMPLE_RATE      16000
#define DURATION_SEC     30                 // fixed recording length
#define CHUNK_SAMPLES    512     
#define BUTTON_PIN       21

static const char* WIFI_SSID = "<wifi-name>";
static const char* WIFI_PASS = "<wifi password>";
const char* serverHost = "<ip adress>";
const uint16_t serverPort = <port>;
const uint32_t TOTAL_SAMPLES = SAMPLE_RATE * DURATION_SEC;
const int CONTENT_LENGTH = TOTAL_SAMPLES * 2;  // 16-bit mono, bytes
unsigned long lastPollMs = 0;
const unsigned long POLL_INTERVAL_MS = 3000; 

int current_button_state = HIGH;
int last_button_state = HIGH;

int32_t raw_buf[CHUNK_SAMPLES];   // raw 32-bit I2S samples
int16_t pcm_buf[CHUNK_SAMPLES];  


void i2s_mic_init() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256
  };
  i2s_pin_config_t pins = {
    .mck_io_num  = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_SCK,
    .ws_io_num    = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_SD,
  };
  i2s_driver_install(I2S_NUM, &config, 0, NULL);
  i2s_set_pin(I2S_NUM, &pins);
}

void i2s_speaker_init() {

  i2s_config_t config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = 16000,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,   // was ONLY_RIGHT — must match the
                                                        // manually-interleaved stereo buffer
                                                        // built below in play_wav_from_url()
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 256
  };

  i2s_pin_config_t pins = {
    .mck_io_num  = I2S_PIN_NO_CHANGE,
    .bck_io_num   = I2S_SCK2,
    .ws_io_num    = I2S_WS2,
    .data_out_num = DIN,
    .data_in_num = I2S_PIN_NO_CHANGE,
  };

  i2s_driver_install(I2S_NUM2, &config, 0, NULL);
  i2s_set_pin(I2S_NUM2, &pins);

}


esp_err_t play_wav_from_url() {
  esp_http_client_config_t config = {};
  config.host = serverHost;
  config.port = serverPort;
  config.path = "/wav_send";
  config.method = HTTP_METHOD_GET;
  config.event_handler = _http_event_handler;
  config.timeout_ms = 2000;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
      printf("connection failed: %s\n", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return err;
  }

  int content_length = esp_http_client_fetch_headers(client);
  int http_status = esp_http_client_get_status_code(client);
  if (http_status != 200) {
      // 204 = nothing queued yet, 404 = name not found, etc. — nothing to play.
      // Without this check, a 204/404 body would get parsed as a WAV header
      // below and potentially pushed to the speaker as noise.
      esp_http_client_cleanup(client);
      return ESP_OK;
  }
  printf("content length: %d\n", content_length);
  // --- Parse WAV header from the stream itself (can't seek back) ---
  static uint8_t hdr[44];
  int total_read = 0;
  while (total_read < 44) {
      int r = esp_http_client_read(client, (char*)hdr + total_read, 44 - total_read);
      if (r <= 0) { esp_http_client_cleanup(client); return ESP_FAIL; }
      total_read += r;
  }
  Serial.println(uxTaskGetStackHighWaterMark(NULL));

  uint32_t sample_rate    = *(uint32_t*)(hdr + 24);
  uint16_t num_channels   = *(uint16_t*)(hdr + 22);
  uint32_t data_size      = *(uint32_t*)(hdr + 40);

  if (memcmp(hdr + 36, "data", 4) != 0) {
      printf("non-canonical header, extra chunks present — not handled here\n");
  }

  static uint8_t buf[1024];
  static uint8_t stereo_buf[2048];
  int remaining = data_size;
  size_t bytes_written;
  while (remaining > 0) {
      int to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
      int r = esp_http_client_read(client, (char*)buf, to_read);
      if (r <= 0) break; // connection closed / error
      remaining -= r;
      if (num_channels == 1) {
          int16_t *mono = (int16_t*)buf;
          int16_t *stereo = (int16_t*)stereo_buf;
          int n = r / 2;
          for (int i = 0; i < n; i++) { stereo[2*i] = mono[i]; stereo[2*i+1] = mono[i]; }
          i2s_write(I2S_NUM2, stereo_buf, n * 4, &bytes_written, portMAX_DELAY);
      } else {
          i2s_write(I2S_NUM2, buf, r, &bytes_written, portMAX_DELAY);
      }
  }
  i2s_zero_dma_buffer(I2S_NUM2);
  Serial.println(uxTaskGetStackHighWaterMark(NULL));
  esp_http_client_cleanup(client);
  return ESP_OK;
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  return ESP_OK;
}



void setup() {

  Serial.begin(115200);
  delay(1000);

  i2s_mic_init();
  Serial.println("INMP441 Microphone Initialized");
  i2s_speaker_init();
  Serial.println("Speaker Initialized");
  play_test_tone();
  i2s_zero_dma_buffer(I2S_NUM2);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  last_button_state = digitalRead(BUTTON_PIN);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi Connected! IP: " + WiFi.localIP().toString());
}


void loop() {

  unsigned long now = millis();
  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    play_wav_from_url();
  }

  int current_button_state = digitalRead(BUTTON_PIN);

  if (current_button_state == LOW && last_button_state == HIGH) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("\n[Button] Pressed — starting fixed-duration recording+stream");
      recordAndStream();
    }
  }

  last_button_state = current_button_state;
  delay(10);
}








void recordAndStream() {
  Serial.println("[Stream] Opening connection...");

  esp_http_client_config_t config = {};
  config.host = serverHost;
  config.port = serverPort;
  config.path = "/transcribe";
  config.method = HTTP_METHOD_POST;
  config.event_handler = _http_event_handler;
  config.timeout_ms = 45000;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

  // Declares the total body size up front so the server can use a normal
  // Content-Length read instead of needing chunked transfer encoding.
  esp_err_t open_err = esp_http_client_open(client, CONTENT_LENGTH);
  if (open_err != ESP_OK) {
    Serial.printf("[Stream] ERROR: open failed: %s\n", esp_err_to_name(open_err));
    esp_http_client_cleanup(client);
    return;
  }
  Serial.printf("[Stream] Connection open. Declared Content-Length: %d bytes (%d s)\n",
                CONTENT_LENGTH, DURATION_SEC);

  uint32_t samples_sent = 0;
  int bytes_written_total = 0;
  uint32_t chunk_count = 0;

  while (samples_sent < TOTAL_SAMPLES) {
    uint32_t samples_this_read = min((uint32_t)CHUNK_SAMPLES, TOTAL_SAMPLES - samples_sent);
    size_t bytes_read = 0;

    i2s_read(I2S_NUM, raw_buf, samples_this_read * sizeof(int32_t), &bytes_read, portMAX_DELAY);
    int samples_read = bytes_read / sizeof(int32_t);

    if (samples_read == 0) {
      Serial.println("[Stream] WARNING: i2s_read returned 0 samples, retrying...");
      continue;
    }

    // NOTE: verify this shift against a real recorded waveform — the correct
    // amount depends on how the INMP441 aligns its 24-bit output in the 32-bit
    // I2S word. >>14 was a working starting point in earlier testing; don't
    // trust it blindly, confirm it isn't clipping or too quiet.
    for (int i = 0; i < samples_read; i++) {
      pcm_buf[i] = (int16_t)(raw_buf[i] >> 14);
    }

    int chunk_bytes = samples_read * sizeof(int16_t);
    int written = esp_http_client_write(client, (const char*)pcm_buf, chunk_bytes);

    if (written < 0) {
      Serial.printf("[Stream] ERROR: write failed at chunk %u (samples_sent=%u)\n",
                    chunk_count, samples_sent);
      break;
    } else if (written != chunk_bytes) {
      Serial.printf("[Stream] WARNING: short write — expected %d, wrote %d\n",
                    chunk_bytes, written);
    }

    bytes_written_total += written;
    samples_sent += samples_read;
    chunk_count++;

    if (chunk_count % 10 == 0) {
      Serial.printf("[Stream] Progress: %u / %u samples (%d bytes sent)\n",
                    samples_sent, TOTAL_SAMPLES, bytes_written_total);
    }
  }

  Serial.printf("[Stream] Done writing. Total: %d bytes across %u chunks (expected %d bytes)\n",
                bytes_written_total, chunk_count, CONTENT_LENGTH);

  if (bytes_written_total != CONTENT_LENGTH) {
    Serial.println("[Stream] WARNING: bytes written does not match declared Content-Length — "
                    "server read will likely hang or truncate.");
  }

  // Response
  int status = esp_http_client_fetch_headers(client);
  int http_status = esp_http_client_get_status_code(client);
  Serial.printf("[Stream] Server response status: %d\n", http_status);

  char resp_buf[512];
  int resp_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
  if (resp_len > 0) {
    resp_buf[resp_len] = '\0';
    Serial.printf("[Stream] Server response body: %s\n", resp_buf);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  Serial.println("[Stream] Connection closed.");
}



void play_test_tone() {
  const int freq = 440;
  const int seconds = 2;
  const int num_samples = SAMPLE_RATE * seconds;
  int16_t sample_buf[256];
  size_t bytes_written;

  for (int i = 0; i < num_samples; i += 128) {
    int16_t stereo[256]; // 128 frames, L+R interleaved
    for (int j = 0; j < 128 && (i + j) < num_samples; j++) {
      int16_t s = (int16_t)(10000 * sin(2 * PI * freq * (i + j) / (float)SAMPLE_RATE));
      stereo[2*j] = s;
      stereo[2*j+1] = s;
    }
    i2s_write(I2S_NUM2, stereo, 512, &bytes_written, portMAX_DELAY);
  }
}
