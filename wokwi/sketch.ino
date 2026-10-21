/*
  VoltStation - Estacao inteligente de recarga solar para veiculos eletricos
  Sprint 4 - Desafio GoodWe - FIAP

  O ESP32 faz o papel do inversor hibrido GoodWe:
  le os sensores, decide de onde vem a energia e liga o carregador.

  Ordem de prioridade:
    1. SOLAR   -> se sobra energia solar
    2. BATERIA -> se for horario de pico e a bateria tiver carga
    3. REDE    -> so fora do horario de pico
    4. ESPERA  -> horario de pico sem sol e sem bateria
    PROTECAO   -> temperatura alta: desliga o carregador

  Controles na simulacao:
    - Sensor de luz (LDR)   = sol / geracao solar
    - Potenciometro         = carga da bateria (%)
    - DHT22                 = temperatura do carregador
    - Chave deslizante      = horario de pico (ligada = pico)
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <LiquidCrystal_I2C.h>

// ---------- Pinos ----------
#define PIN_LDR      34   // sensor de luz (geracao solar)
#define PIN_BATERIA  35   // potenciometro (carga da bateria)
#define PIN_DHT      15   // sensor de temperatura
#define PIN_PICO      4   // chave: horario de pico
#define PIN_RELE     26   // rele do carregador do carro
#define LED_SOLAR    25   // verde
#define LED_BATERIA  33   // amarelo
#define LED_REDE     32   // vermelho
#define PIN_BUZZER   27   // alarme

// ---------- Parametros do sistema ----------
const float SOLAR_MAX_W      = 7000;  // pico de geracao dos paineis (W)
const float CONSUMO_CASA_W   = 1000;  // consumo fixo da casa (W)
const float RECARGA_MIN_W    = 1400;  // menor potencia de recarga do carregador (W)
const float RECARGA_MAX_W    = 7000;  // carregador GoodWe HCA de 7 kW
const float RECARGA_BAT_W    = 3700;  // potencia de recarga usando a bateria (W)
const int   BATERIA_MIN      = 30;    // abaixo disso a bateria nao e usada (%)
const float TEMP_MAX         = 60;    // acima disso o carregador desliga (C)
const float CO2_POR_KWH      = 1.28;  // kg de CO2 evitado por kWh vs carro a gasolina
const int   ESCALA_TEMPO     = 60;    // 1 s real = 1 min simulado

// ---------- MQTT ----------
const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_SENHA  = "";
const char* MQTT_BROKER = "broker.hivemq.com";
const char* MQTT_TOPICO = "fiap/voltstation572949/telemetria";  // topico unico da equipe

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DHTesp dht;
LiquidCrystal_I2C lcd(0x27, 16, 2);

float energiaLimpaKwh = 0;   // energia entregue ao carro vinda do sol/bateria
float co2EvitadoKg    = 0;
unsigned long ultimoCiclo = 0;

// ---------- Conexoes ----------
void conectarWiFi() {
  Serial.print("Conectando ao Wi-Fi");
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);   // canal 6 = conexao rapida no Wokwi
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " OK" : " falhou (seguindo offline)");
}

unsigned long ultimaTentativa = 0;
void conectarMQTT() {
  if (mqtt.connected() || millis() - ultimaTentativa < 5000) return;
  ultimaTentativa = millis();
  if (WiFi.status() != WL_CONNECTED) {      // tenta reconectar o Wi-Fi
    WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
    return;
  }
  String id = "voltstation-" + String(random(10000));
  if (mqtt.connect(id.c_str())) {
    Serial.println("MQTT conectado");
  }
}

// ---------- Saidas ----------
void acenderLed(int ledAceso) {
  digitalWrite(LED_SOLAR,   ledAceso == LED_SOLAR);
  digitalWrite(LED_BATERIA, ledAceso == LED_BATERIA);
  digitalWrite(LED_REDE,    ledAceso == LED_REDE);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PICO, INPUT_PULLUP);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(LED_SOLAR, OUTPUT);
  pinMode(LED_BATERIA, OUTPUT);
  pinMode(LED_REDE, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  dht.setup(PIN_DHT, DHTesp::DHT22);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("  VoltStation");
  lcd.setCursor(0, 1);
  lcd.print(" Desafio GoodWe");

  conectarWiFi();
  mqtt.setServer(MQTT_BROKER, 1883);
}

void loop() {
  if (!mqtt.connected()) conectarMQTT();
  mqtt.loop();

  if (millis() - ultimoCiclo < 1000) return;
  float dtHoras = (millis() - ultimoCiclo) / 3600000.0 * ESCALA_TEMPO;
  ultimoCiclo = millis();

  // ===== 1. Leitura dos sensores =====
  int luz = analogRead(PIN_LDR);                         // mais luz = valor menor
  float solarW = (4095 - luz) / 4095.0 * SOLAR_MAX_W;
  int bateria = map(analogRead(PIN_BATERIA), 0, 4095, 0, 100);
  float temp = dht.getTemperature();
  bool pico = digitalRead(PIN_PICO) == HIGH;              // chave ligada = pico

  float excedente = solarW - CONSUMO_CASA_W;

  // ===== 2. Decisao da fonte de energia =====
  String modo;
  float recargaW = 0;

  if (!isnan(temp) && temp > TEMP_MAX) {
    modo = "PROTECAO";
    recargaW = 0;
    acenderLed(-1);
  } else if (excedente >= RECARGA_MIN_W) {
    modo = "SOLAR";
    recargaW = min(excedente, RECARGA_MAX_W);
    acenderLed(LED_SOLAR);
  } else if (pico && bateria > BATERIA_MIN) {
    modo = "BATERIA";
    recargaW = RECARGA_BAT_W;
    acenderLed(LED_BATERIA);
  } else if (!pico) {
    modo = "REDE";
    recargaW = RECARGA_MAX_W;
    acenderLed(LED_REDE);
  } else {
    modo = "ESPERA";
    recargaW = 0;
    acenderLed(-1);
  }

  // ===== 3. Acionamento =====
  digitalWrite(PIN_RELE, recargaW > 0);
  if (modo == "PROTECAO") tone(PIN_BUZZER, 1000, 300);

  // ===== 4. Indicadores de sustentabilidade =====
  if (modo == "SOLAR" || modo == "BATERIA") {
    float kwh = recargaW / 1000.0 * dtHoras;
    energiaLimpaKwh += kwh;
    co2EvitadoKg += kwh * CO2_POR_KWH;
  }

  // ===== 5. Display =====
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(modo);
  lcd.setCursor(9, 0);
  lcd.print(recargaW / 1000.0, 1);
  lcd.print("kW");
  lcd.setCursor(0, 1);
  lcd.print("S:");
  lcd.print(solarW / 1000.0, 1);
  lcd.print(" B:");
  lcd.print(bateria);
  lcd.print("% ");
  lcd.print(isnan(temp) ? 0 : (int)temp);
  lcd.print("C");

  // ===== 6. Telemetria (Serial + MQTT) =====
  char json[256];
  snprintf(json, sizeof(json),
    "{\"modo\":\"%s\",\"solar_w\":%.0f,\"bateria\":%d,\"recarga_w\":%.0f,"
    "\"temp\":%.1f,\"pico\":%s,\"energia_limpa_kwh\":%.3f,\"co2_evitado_kg\":%.3f}",
    modo.c_str(), solarW, bateria, recargaW,
    isnan(temp) ? 0.0 : temp, pico ? "true" : "false",
    energiaLimpaKwh, co2EvitadoKg);

  Serial.println(json);
  if (mqtt.connected()) mqtt.publish(MQTT_TOPICO, json);
}
