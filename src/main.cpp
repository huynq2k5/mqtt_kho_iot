#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>

const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_broker = "66134711837f4104800192a63e1b7f97.s1.eu.hivemq.cloud";
const char* mqtt_user = "huyng";
const char* mqtt_password = "Huy12345";
const int mqtt_port = 8883;

const float GAMMA = 0.7;
const float RL10 = 50;

const char* base_topic = "kho_iot/TB01";           
const char* config_topic = "kho_iot/kichban/TB01"; 
const char* mode_topic = "kho_iot/system/mode";
const char* control_topic = "kho_iot/TB01/cmd"; 
const char* ack_topic = "kho_iot/ack/TB01";   
bool systemManual = false; // Cờ chế độ toàn cục     

#define RELAY_FAN 18
#define RELAY_AC  17
#define RELAY_HUM 16
#define RELAY_CS 4
#define DHTPIN 12
#define DHTTYPE DHT22
#define LDR_PIN 34
#define CO2_PIN 36

LiquidCrystal_I2C lcd(0x27, 20, 4); 
DHT dht(DHTPIN, DHTTYPE);
WiFiClientSecure espClient;
PubSubClient client(espClient);

struct KichBan {
  String kieuVao; String phepSoSanh; 
  float nguong; int chanRa; int hanhDong;
};

KichBan dsKichBan[5];
int soLuongKichBan = 0;
float lastT = 0, lastH = 0;
float lastCO2 = 0, lastLux = 0;
bool statusFan = false, statusAC = false, statusHum = false; 
bool statusWindow = false;
bool manualMode[3] = {false, false, false}; 

SemaphoreHandle_t xMutex;

float docGiaTriLux() {
  int giaTriAnalog = analogRead(LDR_PIN); 
  if (giaTriAnalog == 0) return 0; 
  
  float dienAp = giaTriAnalog * 3.3 / 4095.0;
  float dienTroLdr = 10000 * dienAp / (3.3 - dienAp);
  float giaTriLux = pow(RL10 * 1e3 * pow(10, GAMMA) / dienTroLdr, (1 / GAMMA));
  
  return giaTriLux;
}

void hienThiLCD(float t, float h, float co2, float lux) {
  lcd.setCursor(0, 0);
  lcd.print("Nhiet do: "); lcd.print(t, 1); lcd.print(" C");
  lcd.setCursor(0, 1);
  lcd.print("Do am   : "); lcd.print(h, 1); lcd.print(" %");
  lcd.setCursor(0, 2);
  lcd.print("Anh sang: "); lcd.print(lux, 0); lcd.print(" Lux");
  lcd.setCursor(0, 3);
  lcd.print("Khi CO2 : "); lcd.print(co2, 0); lcd.print(" ppm");
}

void publishState(float t, float h, float co2, float lux) {
  StaticJsonDocument<256> doc;
  doc["t"] = t;
  doc["h"] = h;
  doc["co2"] = co2;
  doc["as"] = lux;
  doc["fan"] = statusFan ? 1 : 0;
  doc["ac"] = statusAC ? 1 : 0;
  doc["hum"] = statusHum ? 1 : 0;
  doc["win"] = statusWindow ? 1 : 0;
  doc["s"] = 1;

  char buffer[256];
  serializeJson(doc, buffer);
  
  client.publish(base_topic, buffer);
  client.publish("kho_iot/TB01/status", buffer);

  Serial.print("[MQTT] Đã gửi: T:"); Serial.print(t);
  Serial.print(", H:"); Serial.print(h);
  Serial.print(", CO2:"); Serial.print(co2);
  Serial.print(", LUX:"); Serial.println(lux);
}

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload, length)) return;

  String top = String(topic);
  
  if (top == mode_topic) {
    String mode = doc["mode"];
    systemManual = (mode == "MANUAL");
    Serial.print("[SYSTEM] Che đo hien tai: ");
    Serial.println(mode);
    return; 
  }

  if (top == config_topic) {
    Serial.println("[CONFIG] Đang nhan kich ban");
    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      for(int i=0; i<3; i++) manualMode[i] = false; 
      JsonArray rules = doc["data"].as<JsonArray>();
      soLuongKichBan = rules.size();
      for (int i = 0; i < soLuongKichBan && i < 5; i++) {
        JsonObject v = rules[i];
        dsKichBan[i].kieuVao = v["in"].as<String>();
        dsKichBan[i].phepSoSanh = v["op"].as<String>();
        dsKichBan[i].nguong = v["val"].as<float>();
        String out = v["out"].as<String>();
        if (out == "q") dsKichBan[i].chanRa = RELAY_FAN;
        else if (out == "a") dsKichBan[i].chanRa = RELAY_AC;
        else if (out == "cs") dsKichBan[i].chanRa = RELAY_CS;
        else dsKichBan[i].chanRa = RELAY_HUM;
        dsKichBan[i].hanhDong = (v["act"] == "ON") ? 1 : 0;
      }
      xSemaphoreGive(xMutex);
      Serial.print("[CONFIG] Da ap dung "); 
      Serial.print(soLuongKichBan); 
      Serial.println(" kich ban.");
      client.publish(ack_topic, "{\"status\":\"rules_applied\"}");
    }
  } 
  else if (top == control_topic) {
    if (systemManual) {
        String device = doc["device"];
        bool act = (doc["act"] == "ON");
        
        if (device == "q") { statusFan = act; digitalWrite(RELAY_FAN, act); }
        else if (device == "a") { statusAC = act; digitalWrite(RELAY_AC, act); }
        else if (device == "h") { statusHum = act; digitalWrite(RELAY_HUM, act); }
        else if (device == "cs") { statusWindow = act; digitalWrite(RELAY_CS, act); }
        
        publishState(dht.readTemperature(), dht.readHumidity(), analogRead(CO2_PIN), docGiaTriLux());
    }
  }
}

void TaskMQTT(void *pvParameters) {
  for (;;) {
    if (!client.connected()) {
      Serial.print("[SYSTEM] MQTT Lost. Reconnecting...");
      
      const char* willTopic = "kho_iot/TB01/status";
      const char* billMsg = "{\"s\":0}";
      
      if (client.connect("ESP32_TB01", mqtt_user, mqtt_password, willTopic, 1, true, billMsg)) {
        Serial.println(" Success!");
        client.subscribe(config_topic);
        client.subscribe(control_topic);
        client.subscribe(mode_topic);
        
        // Sau khi kết nối xong, báo ngay trạng thái Online (s=1)
        publishState(dht.readTemperature(), dht.readHumidity(), analogRead(CO2_PIN), docGiaTriLux());
      } else {
        Serial.println(" Failed.");
        vTaskDelay(30000 / portTICK_PERIOD_MS); 
      }
    }

    if (client.connected()) {
      client.loop();
      vTaskDelay(50 / portTICK_PERIOD_MS);
    } else {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
  }
}

void TaskLogic(void *pvParameters) {
  for (;;) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    float co2 = (float)analogRead(CO2_PIN);
    float lux = docGiaTriLux();
    hienThiLCD(t, h, co2, lux);

    if (!isnan(t) && !isnan(h)) {
      bool coThayDoi = false;
      if (abs(t - lastT) > 0.5 || 
          abs(h - lastH) > 2.0 || 
          abs(co2 - lastCO2) > 50.0 ||  
          abs(lux - lastLux) > 100.0) { 
        
        coThayDoi = true;
        lastT = t; 
        lastH = h;
        lastCO2 = co2; 
        lastLux = lux; 
      }

      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        if(!systemManual){
            bool nextFan = false; bool nextAC = false; bool nextHum = false; bool nextCS = false;
            for (int i = 0; i < soLuongKichBan; i++) {
              bool trigger = false;
              float val = 0;
              if (dsKichBan[i].kieuVao == "t") val = t;
              else if (dsKichBan[i].kieuVao == "h") val = h;
              else if (dsKichBan[i].kieuVao == "as") val = lux;
              else if (dsKichBan[i].kieuVao == "co2") val = co2;

              if (dsKichBan[i].phepSoSanh == ">") trigger = (val > dsKichBan[i].nguong);
              else if (dsKichBan[i].phepSoSanh == "<") trigger = (val < dsKichBan[i].nguong);

              if (trigger) {
                if (dsKichBan[i].chanRa == RELAY_FAN) nextFan = (dsKichBan[i].hanhDong == 1);
                if (dsKichBan[i].chanRa == RELAY_AC) nextAC = (dsKichBan[i].hanhDong == 1);
                if (dsKichBan[i].chanRa == RELAY_HUM) nextHum = (dsKichBan[i].hanhDong == 1);
                if (dsKichBan[i].chanRa == RELAY_CS) nextCS = (dsKichBan[i].hanhDong == 1);
              }
            }

            if (nextFan != statusFan || nextAC != statusAC || nextHum != statusHum || nextCS != statusWindow || coThayDoi) {
              statusFan = nextFan; statusAC = nextAC; statusHum = nextHum; statusWindow = nextCS;
              digitalWrite(RELAY_FAN, statusFan);
              digitalWrite(RELAY_AC, statusAC);
              digitalWrite(RELAY_HUM, statusHum);
              digitalWrite(RELAY_CS, statusWindow);
              publishState(t, h, co2, lux);
            }
        }
        xSemaphoreGive(xMutex);
      }
    }
    vTaskDelay(3000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  lcd.init(); 
  lcd.backlight();
  pinMode(RELAY_CS, OUTPUT);
  pinMode(RELAY_FAN, OUTPUT); pinMode(RELAY_AC, OUTPUT); pinMode(RELAY_HUM, OUTPUT);
  dht.begin();
  xMutex = xSemaphoreCreateMutex();
  
  Serial.print("[SYSTEM] Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  
  espClient.setInsecure();
  client.setBufferSize(512);
  
  client.setServer(mqtt_broker, mqtt_port);
  client.setKeepAlive(15); 
  client.setBufferSize(1024); 
  client.setCallback(callback);

  xTaskCreatePinnedToCore(TaskMQTT, "TaskMQTT", 10240, NULL, 2, NULL, 0); 
  xTaskCreatePinnedToCore(TaskLogic, "TaskLogic", 4096, NULL, 1, NULL, 1);
}

void loop() { vTaskDelete(NULL); }
