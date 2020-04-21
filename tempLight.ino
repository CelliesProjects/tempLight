#include <Arduino.h>
#include<WiFi.h>
#include<Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "index_htm.h"
#include "lightState.h"
#define NUMBER_OF_CHANNELS 3

#define SET_STATIC_IP false                              /* If SET_STATIC_IP is set to true then STATIC_IP, GATEWAY, SUBNET and PRIMARY_DNS have to be set to some sane values */
const char * SSID = "huiskamer";
const char * PSK = "0987654321";

const IPAddress STATIC_IP(192, 168, 0, 60);              /* This should be outside your router dhcp range! */
const IPAddress GATEWAY(192, 168, 0, 1);                 /* Set to your gateway ip address */
const IPAddress SUBNET(255, 255, 255, 0);                /* Usually 255,255,255,0 but check in your router or pc connected to the same network */
const IPAddress PRIMARY_DNS(192, 168, 0, 30);            /* Check in your router */
const IPAddress SECONDARY_DNS(192, 168, 0, 50);          /* Check in your router */

const uint8_t pin[NUMBER_OF_CHANNELS] {22, 21, 17};

struct ledChannel_t {
  uint32_t val;
};

#define COLOR_STR_LENGTH 8    /* example: "#ff0000" for red */

struct color_t {
  char color[COLOR_STR_LENGTH];
} color;

struct strobo_t {
  double freq{5};
  char color[COLOR_STR_LENGTH]; /* example: "#ff0000" for red */
} stroboscope;

Preferences preferences;
ledChannel_t led[NUMBER_OF_CHANNELS];

#define DEFAULT_PRIORITY 5

AsyncWebServer http(80);
AsyncWebSocket ws("/ws");
lightState light;

double setLedc(uint32_t frequency, uint8_t numberOfBits) {
  double retfreq{0};
  for (auto ch = 0; ch < NUMBER_OF_CHANNELS; ch++) {
    ledcAttachPin(pin[ch], ch);
    retfreq = ledcSetup(ch, frequency, numberOfBits);
    if (!retfreq) {
      ESP_LOGE(TAG, "ERROR setting ledc");
      return false;
    }
  }
  return retfreq;
}


void writeHexToLedc(const char * color) {
  uint32_t tmp = strtol(&color[1], NULL, 16);
  uint32_t redVal = (tmp & 0xFF0000) >> 16;
  uint32_t greenVal = (tmp & 0x00FF00) >> 8;
  uint32_t blueVal = tmp & 0x0000FF;
  ledcWrite(0, map(redVal, 0, 255, 0, 65535));
  ledcWrite(1, map(greenVal, 0, 255, 0, 65535));
  ledcWrite(2, map(blueVal, 0, 255, 0, 65535));
}

void setup() {
  ESP_LOGI(TAG, "tempLight v0.0");
  preferences.begin("tempLight", false);
  preferences.getBytes("lastColor", color.color, sizeof(color.color));
  if (SET_STATIC_IP && !WiFi.config(STATIC_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) ESP_LOGE(TAG, "Setting static IP failed");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(SSID, PSK);
  ESP_LOGI(TAG, "Connecting to %s", SSID);
  WiFi.waitForConnectResult();
  if (WiFi.status() != WL_CONNECTED) {
    ESP_LOGE(TAG, "Could not connect to %s.\nAdjust SSID and/or PSK.\nSystem halted.", SSID);
    while (true) delay(1000);
  }
  ESP_LOGI(TAG, "Connected as IP: %s", WiFi.localIP().toString().c_str());

  ws.onEvent(onEvent);
  http.addHandler(&ws);

  http.on("/", HTTP_GET, [] (AsyncWebServerRequest * request) {
    AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", index_htm, index_htm_len);
    request->send(response);
  });

  http.onNotFound([](AsyncWebServerRequest * request) {
    ESP_LOGI(TAG, "Not found http://%s%s\n", request->host().c_str(), request->url().c_str());
    request->send(404);
  });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  http.begin();
  ESP_LOGI(TAG, "HTTP server started.");

  /* set pwm to some sane values */
  if (!setLedc(1300, 16)) {
    ESP_LOGE(TAG, "Invalid frequency/bitdepth");
  }
}

void loop() {
  switch (light.state()) {
    case LS_OFF: {
        ESP_LOGI(TAG, "Switching to off");
        ledcWrite(0, 0);
        ledcWrite(1, 0);
        ledcWrite(2, 0);
        while (light.state() == LS_OFF) {
          delay(1);
        }
        break;
      }
    case LS_TEMPERATURE: {
        while (light.state() == LS_TEMPERATURE) {
          const uint16_t port = 80;
          const char * host = "192.168.0.80"; // ip or dns
          ESP_LOGD(TAG, "Connecting to %s:%i", host, port);
          WiFiClient client;
          if (!client.connect(host, port)) {
            ESP_LOGE(TAG, "Could not connect to %s:%i", host, port);
            while (light.state() == LS_TEMPERATURE) {
              delay(10);
            }
          }
          else {
            ESP_LOGD(TAG, "Connected to %s:%i", host, port);
            client.print("GET /api/getdevice?status= HTTP/1.1\n\n");
          }

          int maxloops = 0;
          while (!client.available() && maxloops < 1000 && light.state() == LS_TEMPERATURE) {
            maxloops++;
            delay(1); //delay 1 msec
          }

          bool sensorFound{false};
          if (client.available() > 0) {
            while (client.available() && !sensorFound && light.state() == LS_TEMPERATURE) {
              String line = client.readStringUntil('\r\n');
              ESP_LOGD(TAG, "Read:%s", line.c_str());
              if (line.startsWith("kamer")) {
                float temp = atof(line.substring(6).c_str());
                ESP_LOGI(TAG, "found sensor:%f", temp);
                ws.printfAll("sensor\n%.1f", temp);
                sensorFound = true;
              }
            }
          }
          else {
            ESP_LOGI(TAG, "client.available() timed out ");
          }
          if (client) client.stop();
          delay(1000);
        }
        break;
      }
    case LS_COLOR: {
        if (!setLedc(1300, 16)) {
          ESP_LOGE(TAG, "Invalid frequency/bitdepth");
        }
        while (light.state() == LS_COLOR) {
          writeHexToLedc(color.color);
        }
        preferences.putBytes("lastColor", (char*)color.color, sizeof(color.color));
        break;
      }
    case LS_STROBO: {
        ESP_LOGD(TAG, "Strobo running");
        double previousFreq{0};
        while (light.state() == LS_STROBO) {
          if (previousFreq != stroboscope.freq) {
            ESP_LOGD(TAG, "freq %.2fHz->%.2fHz", previousFreq, stroboscope.freq);
            if (setLedc(stroboscope.freq, 16)) {
              previousFreq = stroboscope.freq;
              ledcWrite(0, 65535 / 2);
              ledcWrite(1, 65535 / 2);
              ledcWrite(2, 65535 / 2);
            }
          }
          delay(1);
        }
        break;
      }
    /*
      case LS_COLORFADE : {}
      break;
      case LS_HEARTBEAT : {}
      break;
    */
    default: {
        ESP_LOGE(TAG, "ERROR! Request for unhandled lightState->%i", light.state());
        break;
      }
  }
}




void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("state\n%s", light.stateStr(light.state()));
    client->printf("strobofreq\n%.2f", stroboscope.freq);
    client->printf("colorname\n%s", color.color);
  }

  else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len) {
      if (info->opcode == WS_TEXT) {
        data[len] = 0;
        char *pch = strtok((char*)data, "\n");
        //pch points to command
        if (!strcmp("new_strobofreq", pch)) {
          pch = strtok(NULL, "\n");
          //check for valid freq
          stroboscope.freq = atof(pch);
          ESP_LOGD(TAG, "New strobofreq received: %.2f", stroboscope.freq);
          client->printf("strobofreq\n%.2f", stroboscope.freq);
          return;
        }
        /*
          else if (!strcmp("strobofreq", pch)) {
          //pch = strtok(NULL, "\n");
          ESP_LOGI(TAG, "Request for strobofreq received: %s", pch);
          client->printf("strobofreq\n%i", stroboscope.freq);
          return;
          }
        */

        else if (!strcmp("color", pch)) {
          pch = strtok(NULL, "\n");
          ESP_LOGD(TAG, "Request for color received: %s", pch);
          // check for valid color
          snprintf(color.color, sizeof(color.color), pch);
          client->printf("color\n%i", color.color);
          return;
        }

        else if (!strcmp("strobo_color", pch)) {
          pch = strtok(NULL, "\n");
          ESP_LOGD(TAG, "Request for strobocolor received: %s", pch);
          // check for valid color
          snprintf(stroboscope.color, sizeof(stroboscope.color), pch);
          client->printf("strobocolor\n%i", stroboscope.color);
          return;
        }

        else if (!strcmp("switchstate", pch)) {
          pch = strtok(NULL, "\n");
          ESP_LOGI(TAG, "Request for new state received: %s", pch);
          /* find out which state is requested */
          if (!strcmp("OFF", pch)) {
            light.set(LS_OFF);
            return;
          }
          else if (!strcmp("TEMPERATURE", pch)) {
            light.set(LS_TEMPERATURE);
            return;
          }
          else if (!strcmp("COLOR", pch)) {
            light.set(LS_COLOR);
            return;
          }
          else if (!strcmp("STROBOSCOPE", pch)) {
            ESP_LOGI(TAG, "  state:%s", pch);
            light.set(LS_STROBO);
            return;
          }
        } /*switchstate */





        else {                                                     /*unhandled requests end up here */
          ESP_LOGI(TAG, "Could not handle: '%s'", pch);
        }
      }
    }
  }
}
