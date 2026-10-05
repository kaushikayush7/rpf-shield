// FIRMWARE 2 (reconstructed) - ESP32-CAM surveillance node
// Board: AI Thinker ESP32 Wrover/ESP32-CAM, PSRAM enabled.
// Port 81 / : MJPEG live stream
// Port 80 /capture : single JPEG /alert?type=NITRO : upload evidence to ImgBB

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_http_server.h"
#include "mbedtls/base64.h"
#include <time.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASS";
const char* IMGBB_KEY = "YOUR_IMGBB_API_KEY";
const char* LOCATION_TAG = "PF-4_Coach-S5"; // set per deployment (no GPS in the BOM)

// AI-Thinker pin map
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

httpd_handle_t httpMain = NULL, httpStream = NULL;
volatile bool uploadBusy = false;
char alertType[24] = "ALERT";

static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t r = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return r;
}

static esp_err_t stream_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char part[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    size_t hl = snprintf(part, sizeof(part), "\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    esp_err_t r = httpd_resp_send_chunk(req, part, hl);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (r != ESP_OK) break; // client disconnected
  }
  return ESP_OK;
}

// ---- ImgBB upload runs in its own task so the live stream is never blocked ----
void uploadTask(void*) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb) {
    size_t b64len = 4 * ((fb->len + 2) / 3) + 4, outLen = 0;
    unsigned char* b64 = (unsigned char*)ps_malloc(b64len);
    if (b64 && mbedtls_base64_encode(b64, b64len, &outLen, fb->buf, fb->len) == 0) {
      WiFiClientSecure c;
      c.setInsecure(); // TODO: pin ImgBB root CA for production
      if (c.connect("api.imgbb.com", 443)) {
        time_t now; time(&now);
        String name = String("RPF_") + alertType + "_" + LOCATION_TAG + "_" + String((unsigned long)now);
        String bnd = "----rpfshield";
        String head = "--" + bnd + "\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\n" + name +
                      "\r\n--" + bnd + "\r\nContent-Disposition: form-data; name=\"image\"\r\n\r\n";
        String tail = "\r\n--" + bnd + "--\r\n";
        size_t total = head.length() + outLen + tail.length();
        c.printf("POST /1/upload?key=%s HTTP/1.1\r\nHost: api.imgbb.com\r\n", IMGBB_KEY);
        c.printf("Content-Type: multipart/form-data; boundary=%s\r\n", bnd.c_str());
        c.printf("Content-Length: %u\r\nConnection: close\r\n\r\n", (unsigned)total);
        c.print(head);
        for (size_t i = 0; i < outLen; i += 1024) c.write(b64 + i, min((size_t)1024, outLen - i));
        c.print(tail);
        uint32_t t0 = millis();
        while (c.connected() && millis() - t0 < 8000) { while (c.available()) c.read(); delay(10); }
        c.stop();
      }
    }
    if (b64) free(b64);
    esp_camera_fb_return(fb);
  }
  uploadBusy = false;
  vTaskDelete(NULL);
}

static esp_err_t alert_handler(httpd_req_t* req) {
  char q[48];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) 
    httpd_query_key_value(q, "type", alertType, sizeof(alertType));
  if (!uploadBusy) { uploadBusy = true; xTaskCreate(uploadTask, "upl", 8192, NULL, 1, NULL); }
  return httpd_resp_send(req, "QUEUED", 6);
}

void startServers() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  httpd_uri_t cap = { "/capture", HTTP_GET, capture_handler, NULL };
  httpd_uri_t alr = { "/alert", HTTP_GET, alert_handler, NULL };
  if (httpd_start(&httpMain, &cfg) == ESP_OK) {
    httpd_register_uri_handler(httpMain, &cap);
    httpd_register_uri_handler(httpMain, &alr);
  }
  cfg.server_port = 81; cfg.ctrl_port += 1;
  httpd_uri_t str = { "/", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&httpStream, &cfg) == ESP_OK) httpd_register_uri_handler(httpStream, &str);
}

void setup() {
  Serial.begin(115200);
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sscb_sda = SIOD_GPIO_NUM; c.pin_sscb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000; c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 12; c.fb_count = 2;
  c.grab_mode = CAMERA_GRAB_LATEST; c.fb_location = CAMERA_FB_IN_PSRAM;
  if (esp_camera_init(&c) != ESP_OK) { Serial.println("Camera init failed"); delay(3000); ESP.restart(); }
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  configTime(19800, 0, "pool.ntp.org"); // IST = UTC+5:30, for evidence timestamps
  startServers();
  Serial.println(WiFi.localIP());
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(2000); }
  delay(1000);
}
