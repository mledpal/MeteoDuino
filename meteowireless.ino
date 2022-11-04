#include <NTPClient.h>
#include <WiFiUdp.h>

#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <Adafruit_BMP085.h>
#include <Wire.h>

#include <WiFiClientSecure.h>

#ifndef STASSID
#define STASSID "TU_RED"
#define STAPSK  "TU_CONTRASEÑA"
#endif

#include <Discord_WebHook.h>

const char* ssid = STASSID;
const char* password = STAPSK;

ESP8266WebServer server(80);
Adafruit_BMP085 bmp;

uint8_t DHTPin = D3;
#define DHTTYPE DHT11
DHT dht(DHTPin, DHTTYPE);

const int led = 13;


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
const long utcOffsetInSeconds = 7200;

//////////// SETUP ////////////

void setup(void) {

  bmp.begin();
  dht.begin();
  Serial.begin(115200);


  pinMode(led, OUTPUT);
  digitalWrite(led, 0);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("");

  // Wait for connection
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  NTPClient timeClient(ntpUDP, "hora.roa.es", utcOffsetInSeconds);
  timeClient.begin();
  timeClient.setTimeOffset(+7200);

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("esp8266")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);

  server.on("/meteo.html", HTTP_GET, handleTemp);

  server.on("/discord", HTTP_GET, discord);

  server.onNotFound(handleNotFound);

  /////////////////////////////////////////////////////////
  // Hook examples

  server.addHook([](const String & method, const String & url, WiFiClient * client, ESP8266WebServer::ContentTypeFunction contentType) {
    (void)method;      // GET, PUT, ...
    (void)url;         // Ejemplo: /root/myfile.html
    (void)client;      // Conexion TCP cliente
    (void)contentType; // contentType(".html") => "text/html"
    Serial.printf("A useless web hook has passed\n");
    Serial.printf("(this hook is in 0x%08x area (401x=IRAM 402x=FLASH))\n", esp_get_program_counter());
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  });

  server.addHook([](const String&, const String & url, WiFiClient*, ESP8266WebServer::ContentTypeFunction) {
    if (url.startsWith("/fail")) {
      Serial.printf("An always failing web hook has been triggered\n");
      return ESP8266WebServer::CLIENT_MUST_STOP;
    }
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  });

  server.addHook([](const String&, const String & url, WiFiClient * client, ESP8266WebServer::ContentTypeFunction) {
    if (url.startsWith("/dump")) {
      Serial.printf("The dumper web hook is on the run\n");

      
#ifdef STREAMSEND_API
      
      client->sendAll(Serial, 500);
#else
      auto last = millis();
      while ((millis() - last) < 500) {
        char buf[32];
        size_t len = client->read((uint8_t*)buf, sizeof(buf));
        if (len > 0) {
          Serial.printf("(<%d> chars)", (int)len);
          Serial.write(buf, len);
          last = millis();
        }
      }
#endif
     
      
      Serial.printf("\nTelling server to forget this connection\n");
      static WiFiClient forgetme = *client; // stop previous one if present and transfer client refcounter
      return ESP8266WebServer::CLIENT_IS_GIVEN;
    }
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  });

  server.begin();
  Serial.println("HTTP server started");

}


//////////// LOOP ////////////

void loop(void) {
  server.handleClient();
  MDNS.update();
  
  timeClient.setTimeOffset(+7200);
  timeClient.update();

  int minutos = timeClient.getMinutes();
  int seg = timeClient.getSeconds();

  if ((minutos==0 || minutos==15 || minutos==30 || minutos==45) && seg==0) {
    discord();
  }

}


// MANEJADORES DE LINKS
void handleRoot() {

  digitalWrite(led, 1);

  timeClient.setTimeOffset(+7200);
  timeClient.update();

  
  unsigned long epochTime = timeClient.getEpochTime();  
  String hora = String(timeClient.getFormattedTime());
  

  float temp1 = bmp.readTemperature();
  float temp2 = dht.readTemperature();
  float humedad = dht.readHumidity();
  float sensacionTermica = dht.computeHeatIndex(temp2, humedad, false);
  float presion = bmp.readPressure() / 100;
  float altura = bmp.readAltitude();
  float nivelmar = bmp.readSealevelPressure(presion) / 100;

  String xml = "<?xml version='1.0' encoding='UTF-8'?>\r\n";
  xml += "<meteorologica id='1'>\r\n";
  xml += "\t<fecha>" + String(epochTime) + "</fecha>\r\n";
  xml += "\t<hora>" + hora + "</hora>\r\n";
  xml += "\t<temperaturas media='" + String((temp1 + temp2) / 2) + "' sensacion='" + String(sensacionTermica, 2) + "' unidad ='ºC' >\r\n";
  xml += "\t\t<sensor1>" + String(temp1) + "ºC</sensor1>\r\n";
  xml += "\t\t<sensor2>" + String(temp2) + "ºC</sensor2>\r\n";
  xml += "\t</temperaturas>\r\n";
  xml += "\t<presion altura ='" + String(altura, 2) + " m'>\r\n";
  xml += "\t\t<local>" + String(presion, 0) + " Pa</local>";
  xml += "\t\t<mar>" + String(nivelmar, 0) + " Pa</mar>\r\n";
  xml += "\t</presion>\r\n";
  xml += "\t<humedad>" + String(humedad, 0) + " %</humedad>\r\n";
  xml += "</meteorologica>\r\n";


  //Serial.println(mensaje_discord);

  server.send(200, "text/xml", xml);
  digitalWrite(led, 0);



}

void handleTemp() { // Ruta /meteo

  //timeClient.update();

  float humi = dht.readHumidity();
  float temp = dht.readTemperature();
  float hic = dht.computeHeatIndex(temp, humi, false);

  float temp_media = (temp + bmp.readTemperature()) / 2;


  String cadena = "<!DOCTYPE html>";
  cadena += "<html lang='es'>";
  cadena += "<head>";
  cadena += "<meta charset='UTF-8'>";
  cadena += "<meta http-equiv='X-UA-Compatible' content='IE=edge'>";
  cadena += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  cadena += "<style>";
  cadena += "table, tr, td, th { color: white; text-align: center; }";
  cadena += "body { font-family: 'Verdana' ; background-color: rgba(0,0,0,0.4); }";
  cadena += "</style>";
  cadena += "<title>MeteoDuino</title>";
  cadena += "</head>";
  cadena += "<body>";
  cadena += "<table border='1' cellpadding='5' align='center'>";
  cadena += "<tr><th>Temperatura 1</th><th>Temperatura 2</th><th>Temperatura Media</th><th>Sensación Térmica</th><th>Presión</th><th>Humedad</th><th>Altura Relativa</th></tr>";
  cadena += "<tr><td>" + String(String(bmp.readTemperature())) + " ºC </td>";
  cadena += "<td>" + String(String(temp)) + " ºC </td>";
  cadena += "<td>" + String(temp_media) + " ºC</td>";
  cadena += "<td>" + String(hic) + " ºC </td>";
  cadena += "<td>" + String(bmp.readPressure() / 100) + " Pa </td>";
  cadena += "<td>" + String(humi) + " % </td>";
  cadena += "<td>" + String(bmp.readAltitude()) + " m. </td>";
  cadena += "</tr></table>";
  cadena += "</body></html>";

  server.send(200, "text/html", cadena);
  digitalWrite(led, 0);

}

void handleNotFound() {
  digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  digitalWrite(led, 0);
}

void discord() {
  timeClient.setTimeOffset(+7200);
  timeClient.update();

  String hora = String(timeClient.getFormattedTime());

  float temp1 = bmp.readTemperature();
  float temp2 = dht.readTemperature();
  float tempmed = (temp1+temp2)/2;
  float humedad = dht.readHumidity();
  float sensacionTermica = dht.computeHeatIndex(temp2, humedad, false);
  float presion = bmp.readPressure() / 100;
  float altura = bmp.readAltitude();
  float nivelmar = bmp.readSealevelPressure(presion) / 100;


  String mensaje = "[ Hora: "+ hora +" ] ";
  mensaje +="[ T1: "+String(temp1,2)+"ºC | T2: " + String(temp2, 2)+ "ºC | ST : " + String(sensacionTermica,2)+"ºC | TM: " + String(tempmed, 2) + " ºC ] ";
  mensaje +=" [ H: " + String(humedad,0) + "% ]";
  mensaje +=" [ P: " + String(presion, 0) + " Pa ] ";
  mensaje +=" [ A: " + String(altura, 2) + " m ] ";
  
  send_discord(mensaje);
  
  Serial.println("Send data to Discord: OK");
  server.send(200, "text/plain", "OK");

}

void send_discord(String mensaje) {
  const String discord_webhook = "AQUI_TU_WEBHOOK";
  Discord_Webhook discord;
  discord.begin(discord_webhook);
  discord.send(mensaje);
  //delay(500);
}
