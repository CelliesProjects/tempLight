#include <Arduino.h>
#include<WiFi.h>
#include <Wire.h>
#include <SSD1306.h>
#include<Preferences.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "index_htm.h"
#include "lightState.h"

const char * SSID = "huiskamer";
const char * PSK = "0987654321";

#define SET_STATIC_IP false                              /* If SET_STATIC_IP is set to true then STATIC_IP, GATEWAY, SUBNET and PRIMARY_DNS have to be set to some sane values */

const IPAddress STATIC_IP(192, 168, 0, 60);              /* This should be outside your router dhcp range! */
const IPAddress GATEWAY(192, 168, 0, 1);                 /* Set to your gateway ip address */
const IPAddress SUBNET(255, 255, 255, 0);                /* Usually 255,255,255,0 but check in your router or pc connected to the same network */
const IPAddress PRIMARY_DNS(192, 168, 0, 30);            /* Check in your router */
const IPAddress SECONDARY_DNS(192, 168, 0, 50);          /* Check in your router */

#define NUMBER_OF_CHANNELS 3
#define COLOR_STR_LENGTH 8    /* "#ff00ff" is 7 long + 1 extra pos for terminating zero */
#define STROBO_DEFAULT_HZ 5

#define I2C_SCL_PIN                         19
#define I2C_SDA_PIN                         23
#define OLED_ADDRESS                        0x3C


const uint8_t pin[NUMBER_OF_CHANNELS] {22, 21, 17};

const char *   TEMPERATURE_HOST_NAME      = "192.168.0.80";
const uint16_t TEMPERATURE_HOST_PORT      = 80;
const char *   TEMPERATURE_HOST_URL       = "/api/getdevice?status=";
const char *   SENSOR_NAME                = "kamer";

const char *   NVS_NAMESPACE              = "tempLight";
const char *   NVS_KEY_FREQ               = "freq";
const char *   NVS_KEY_COLOR              = "color";
const char *   NVS_KEY_LIGHTSTATE         = "lightstate";

struct temperature_t {
  float temperature;
  char color[COLOR_STR_LENGTH];
} temperature;

struct color_t {
  char color[COLOR_STR_LENGTH]; /* example: "#ff0000" for red */
} color;

struct strobo_t {
  double freq{5};
  char color[COLOR_STR_LENGTH]; /* example: "#ff0000" for red */
} stroboscope;

SSD1306 OLED(OLED_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
AsyncWebServer http(80);
AsyncWebSocket ws("/ws");
lightState light;
Preferences preferences;
bool oledFound{false};

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

inline float mapFloat(const float &x, const float &in_min, const float &in_max, const float &out_min, const float &out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
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

void updateOLED() {
  if (!oledFound) return;
  OLED.clear();
  OLED.setFont(ArialMT_Plain_16);
  OLED.drawString(64, 0, WiFi.localIP().toString());
  OLED.drawString(64, 16 + 24, light.stateStr(light.state()));
  OLED.setFont(ArialMT_Plain_24);
  OLED.drawString(64, 16, (String)temperature.temperature+"℃");
  OLED.display();
}

void setup() {
  ESP_LOGI(TAG, "tempLight v0.0");
  preferences.begin(NVS_NAMESPACE, false);
  preferences.getBytes(NVS_KEY_COLOR, color.color, sizeof(color.color));
  lightState_t state = (lightState_t)preferences.getUInt(NVS_KEY_LIGHTSTATE, LS_OFF);
  ESP_LOGI(TAG, "Restoring state to '%s'", light.stateStr(state));
  light.set(state);

  stroboscope.freq = preferences.getULong(NVS_KEY_FREQ, STROBO_DEFAULT_HZ);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
  Wire.beginTransmission(OLED_ADDRESS);
  uint8_t error = Wire.endTransmission();
  if (error) ESP_LOGI(TAG, "No SSD1306 OLED found.");
  else {
    OLED.init();
    OLED.flipScreenVertically();
    OLED.setTextAlignment(TEXT_ALIGN_CENTER);
    ESP_LOGI(TAG, "Found SSD1306 OLED on I2C at address 0x%x.", OLED_ADDRESS);
    oledFound = true;
  }

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
  //spin up a task that updates the temp every second
  xTaskCreatePinnedToCore(
    temperatureTask,                /* Function to implement the task */
    "tempTask",                     /* Name of the task */
    3000,                           /* Stack size in words */
    NULL,                           /* Task input parameter */
    1,                              /* Priority of the task */
    NULL,                           /* Task handle. */
    1);

}

void loop() {
  switch (light.state()) {
    case LS_OFF: {
        ledcWrite(0, 0);
        ledcWrite(1, 0);
        ledcWrite(2, 0);
        while (light.state() == LS_OFF) {
          delay(1);
        }
        break;
      }
    case LS_TEMPERATURE: {
        if (!setLedc(1300, 16)) {
          ESP_LOGE(TAG, "Invalid frequency/bitdepth");
        }
        while (light.state() == LS_TEMPERATURE) {
          const uint32_t LOWTEMP = 15;
          const uint32_t NICETEMP = 20;
          const uint32_t HIGHTEMP = 25;
          //calculate R,G,B values to put in temperature.color
          uint32_t red, green, blue;
          // red
          if (temperature.temperature < NICETEMP) red = 0;
          else if (temperature.temperature > HIGHTEMP) red = 255;
          else red = mapFloat(temperature.temperature, NICETEMP, HIGHTEMP, 0, 255);
          // blue
          if (temperature.temperature < LOWTEMP) blue = 255;
          else if (temperature.temperature > NICETEMP) blue = 0;
          else blue = mapFloat(temperature.temperature, LOWTEMP, NICETEMP, 255, 0);
          // green
          if (temperature.temperature < LOWTEMP || temperature.temperature > HIGHTEMP) green = 0;
          else if (temperature.temperature > LOWTEMP && temperature.temperature < NICETEMP) green = mapFloat(temperature.temperature, LOWTEMP, NICETEMP, 0, 255);
          else green = mapFloat(temperature.temperature, NICETEMP, HIGHTEMP, 255, 0);

          snprintf(temperature.color, sizeof(temperature.color), "#%02x%02x%02x", red, green, blue);
          writeHexToLedc(temperature.color);
          delay(1);
        }
        break;
      }
    case LS_COLOR: {
        if (!setLedc(1300, 16)) {
          ESP_LOGE(TAG, "Invalid frequency/bitdepth");
        }
        writeHexToLedc(color.color);
        char oldHex[COLOR_STR_LENGTH];
        preferences.getBytes(NVS_KEY_COLOR, oldHex, COLOR_STR_LENGTH);
        while (light.state() == LS_COLOR) {
          if (strcmp(color.color, oldHex)) {
            writeHexToLedc(color.color);
            snprintf((char*)oldHex, COLOR_STR_LENGTH, color.color);
            ESP_LOGI(TAG, "Saving %s to prefs", color.color);
            preferences.putBytes(NVS_KEY_COLOR, (char*)color.color, sizeof(color.color));
          }
          delay(1);
        }
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
              preferences.putULong(NVS_KEY_FREQ, stroboscope.freq);
            }
          }
          delay(1);
        }
        break;
      }
    default: {
        ESP_LOGE(TAG, "ERROR! Request for unhandled lightState->%i", light.state());
        break;
      }
  }
  updateOLED();
}




void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    client->printf("strobofreq\n%.2f", stroboscope.freq);
    client->printf("colorname\n%s", color.color);
    client->printf("sensor\n%f", temperature.temperature);
    client->printf("state\n%s", light.stateStr(light.state()));
    return;
  }

  else if (type == WS_EVT_DATA) {
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len) {
      if (info->opcode == WS_TEXT) {
        data[len] = 0;
        char *pch = strtok((char*)data, "\n");

        //pch points to command
        if (!strcmp("setstrobofreq", pch)) {
          pch = strtok(NULL, "\n");
          //check for valid freq
          if (atof(pch) > 0 && atof(pch) < 100) {
            stroboscope.freq = atof(pch);
            ESP_LOGD(TAG, "New strobofreq received: %.2f", stroboscope.freq);
            client->printf("strobofreq\n%.2f", stroboscope.freq);
          }
          else
            client->printf("error\ninvalid stroboscope freq %i", stroboscope.freq);
          return;
        }

        else if (!strcmp("color", pch)) {
          pch = strtok(NULL, "\n");
          ESP_LOGD(TAG, "Request for color received: %s", pch);
          // check for valid color
          snprintf(color.color, sizeof(color.color), pch);
          return;
        }

        else if (!strcmp("switchstate", pch)) {
          pch = strtok(NULL, "\n");
          lightState_t oldState = light.state();
          ESP_LOGD(TAG, "Request for state switch received: %s", pch);
          /* find out which state is requested */
          if (!strcmp("OFF", pch) && light.state() != LS_OFF) {
            light.set(LS_OFF);
            //return;
          }
          else if (!strcmp("TEMPERATURE", pch) && light.state() != LS_TEMPERATURE) {
            light.set(LS_TEMPERATURE);
          }
          else if (!strcmp("COLOR", pch) && light.state() != LS_COLOR) {
            light.set(LS_COLOR);
          }
          else if (!strcmp("STROBOSCOPE", pch) && light.state() != LS_STROBO) {
            //ESP_LOGI(TAG, "  state:%s", pch);
            light.set(LS_STROBO);
          }
          //we are done with the handlers so check if we need to save the new state to NVS
          if (light.state() != oldState) {
            ESP_LOGI(TAG, "Saving light state %s", light.stateStr(light.state()));
            preferences.putUInt(NVS_KEY_LIGHTSTATE, light.state());
            oldState = light.state();
          }
          return;
        } /*switchstate */

        else {                                                     /*unhandled commands end up here */
          ESP_LOGI(TAG, "ERROR! Unknown command: '%s'", pch);
        }
      }
    }
  }
}

void temperatureTask(void * param) {
  ESP_LOGD(TAG, "Connecting to %s:%i", TEMPERATURE_HOST_NAME, TEMPERATURE_HOST_PORT);
  WiFiClient client;
  while (1) {
    if (!client.connect(TEMPERATURE_HOST_NAME, TEMPERATURE_HOST_PORT)) {
      ESP_LOGE(TAG, "Could not connect to %s:%i", TEMPERATURE_HOST_NAME, TEMPERATURE_HOST_PORT);
      while (1) {  //TODO: wait a bit and retry?
        delay(10);
      }
    }
    else {
      ESP_LOGD(TAG, "Connected to %s:%i", TEMPERATURE_HOST_NAME, TEMPERATURE_HOST_PORT);
      client.printf("GET %s HTTP/1.1\n\n", TEMPERATURE_HOST_URL);
    }

    int maxloops = 0;
    while (!client.available() && maxloops < 1000) {
      maxloops++;
      delay(1); //delay 1 msec
    }

    bool sensorFound{false};
    if (client.available() > 0) {
      while (client.available() && !sensorFound) {
        String line = client.readStringUntil((char)'\r\n');
        ESP_LOGD(TAG, "Read:%s", line.c_str());
        if (line.startsWith(SENSOR_NAME)) {
          temperature.temperature = atof(line.substring(sizeof(SENSOR_NAME) + 2).c_str());
          ESP_LOGD(TAG, "found sensor:%s %f", SENSOR_NAME, temperature);
          static float previousTemperature;
          if (previousTemperature != temperature.temperature) {
            ws.printfAll("sensor\n%f", temperature.temperature);
            previousTemperature = temperature.temperature;
            updateOLED();





          }
          sensorFound = true;
        }
      }
      if (!sensorFound) {
        ESP_LOGE(TAG, "Sensor %s was not found.", SENSOR_NAME);
      }
    }
    else {
      ESP_LOGE(TAG, "client.available() timed out ");
    }
    if (client) client.stop();
    delay(1000);
  }
} /*temperatureTask */
