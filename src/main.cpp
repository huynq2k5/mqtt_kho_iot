#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_broker = "66134711837f4104800192a63e1b7f97.s1.eu.hivemq.cloud";
const char* mqtt_user = "huyng";
const char* mqtt_password = "Huy12345";
const int mqtt_port = 8883;

const char* base_topic = "kho_iot/TB01";           
const char* config_topic = "kho_iot/kichban/TB01"; 
const char* mode_topic = "kho_iot/system/mode";
const char* control_topic = "kho_iot/TB01/cmd"; 
const char* ack_topic = "kho_iot/ack/TB01";   
bool systemManual = false; // Cờ chế độ toàn cục     

#define RELAY_FAN 18
#define RELAY_AC  17
#define RELAY_HUM 16
#define DHTPIN 12
#define DHTTYPE DHT22

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
bool statusFan = false, statusAC = false, statusHum = false;
bool manualMode[3] = {false, false, false}; 

SemaphoreHandle_t xMutex;

void publishState(float t, float h) {
  StaticJsonDocument<256> doc;
  doc["t"] = t;
  doc["h"] = h;
  doc["fan"] = statusFan ? 1 : 0;
  doc["ac"] = statusAC ? 1 : 0;
  doc["hum"] = statusHum ? 1 : 0;
  doc["s"] = 1; // Báo hiệu Online

  char buffer[256];
  serializeJson(doc, buffer);
  
  // Gửi cho Base Topic (Dữ liệu)
  client.publish(base_topic, buffer);
  
  // Gửi cho Status Topic (Để Web cập nhật nút bấm và LWT)
  client.publish("kho_iot/TB01/status", buffer);

  Serial.printf("[MQTT] Published State: T:%.2f, H:%.2f, F:%d, A:%d, H:%d\n", t, h, statusFan, statusAC, statusHum);
}

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, payload, length)) return;

  String top = String(topic);
  
  if (top == mode_topic) {
    String mode = doc["mode"];
    systemManual = (mode == "MANUAL");
    Serial.printf("[SYSTEM] Mode changed to: %s\n", mode.c_str());
    return; // Thoát sớm
  }

  if (top == config_topic) {
    Serial.println("[CONFIG] Received new sensor rules...");
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
        else dsKichBan[i].chanRa = RELAY_HUM;
        dsKichBan[i].hanhDong = (v["act"] == "ON") ? 1 : 0;
      }
      xSemaphoreGive(xMutex);
      Serial.printf("[CONFIG] Applied %d rules. Manual override reset.\n", soLuongKichBan);
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
        
        publishState(dht.readTemperature(), dht.readHumidity());
    }
  }
}

void TaskMQTT(void *pvParameters) {
  for (;;) {
    if (!client.connected()) {
      Serial.print("[SYSTEM] MQTT Lost. Reconnecting...");
      
      // --- SỬA TẠI ĐÂY: Thêm tham số Last Will ---
      // Cấu trúc: connect(id, user, pass, willTopic, willQos, willRetain, willMessage)
      const char* willTopic = "kho_iot/TB01/status";
      const char* billMsg = "{\"s\":0}"; // Bản tin di chúc báo Offline
      
      if (client.connect("ESP32_TB01", mqtt_user, mqtt_password, willTopic, 1, true, billMsg)) {
        Serial.println(" Success!");
        client.subscribe(config_topic);
        client.subscribe(control_topic);
        client.subscribe(mode_topic);
        
        // Sau khi kết nối xong, báo ngay trạng thái Online (s=1)
        publishState(dht.readTemperature(), dht.readHumidity());
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

    if (!isnan(t) && !isnan(h)) {
      bool coThayDoi = false;

      if (abs(t - lastT) > 0.5 || abs(h - lastH) > 2.0) {
        coThayDoi = true;
        lastT = t; 
        lastH = h;
      }

      if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
        if(!systemManual){
			bool nextFan = false; 
			bool nextAC = false; 
			bool nextHum = false;

			for (int i = 0; i < soLuongKichBan; i++) {
			  bool trigger = false;
			  float val = (dsKichBan[i].kieuVao == "t") ? t : h;
			  if (dsKichBan[i].phepSoSanh == ">") trigger = (val > dsKichBan[i].nguong);
			  else if (dsKichBan[i].phepSoSanh == "<") trigger = (val < dsKichBan[i].nguong);

			  if (trigger) {
				if (dsKichBan[i].chanRa == RELAY_FAN) nextFan = (dsKichBan[i].hanhDong == 1);
				if (dsKichBan[i].chanRa == RELAY_AC) nextAC = (dsKichBan[i].hanhDong == 1);
				if (dsKichBan[i].chanRa == RELAY_HUM) nextHum = (dsKichBan[i].hanhDong == 1);
			  }
			}

			if (manualMode[0]) nextFan = statusFan;
			if (manualMode[1]) nextAC = statusAC;
			if (manualMode[2]) nextHum = statusHum;

			if (nextFan != statusFan || nextAC != statusAC || nextHum != statusHum) {
			  coThayDoi = true;
			  statusFan = nextFan; 
			  statusAC = nextAC; 
			  statusHum = nextHum;
			  digitalWrite(RELAY_FAN, statusFan);
			  digitalWrite(RELAY_AC, statusAC);
			  digitalWrite(RELAY_HUM, statusHum);
			}

			if (coThayDoi) {
			  publishState(t, h);
			}
			xSemaphoreGive(xMutex);
		}
      }
    }
    vTaskDelay(5000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
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
  client.setServer(mqtt_broker, mqtt_port);
  client.setCallback(callback);

  xTaskCreatePinnedToCore(TaskMQTT, "TaskMQTT", 8192, NULL, 2, NULL, 0); 
  xTaskCreatePinnedToCore(TaskLogic, "TaskLogic", 4096, NULL, 1, NULL, 1);
}

void loop() { vTaskDelete(NULL); }