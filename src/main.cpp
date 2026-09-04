#include <Arduino.h>
#include <DHTesp.h>

#define DHT_PIN 4

DHTesp dht;

void setup() {
    Serial.begin(115200);

    delay(1000);

    Serial.println("================================");
    Serial.println("      POSCAMPO - DHT22");
    Serial.println("      TESTE DO SENSOR");
    Serial.println("================================");

    dht.setup(DHT_PIN, DHTesp::DHT22);

    Serial.println("DHT22 inicializado!");
}

void loop() {
    TempAndHumidity dados = dht.getTempAndHumidity();

    if (isnan(dados.temperature) || isnan(dados.humidity)) {
        Serial.println("ERRO: nao foi possivel ler o DHT22.");
    } else {
        Serial.print("Temperatura: ");
        Serial.print(dados.temperature, 1);
        Serial.println(" °C");

        Serial.print("Umidade: ");
        Serial.print(dados.humidity, 1);
        Serial.println(" %");

        Serial.println("----------------------------");
    }

    delay(2000);
}