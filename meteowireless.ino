#include <NTPClient.h>
#include <WiFiUdp.h>


#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <WiFiClientSecure.h>

#include <DHT.h>
#include <SFE_BMP180.h>
#include <Wire.h>

#include <Discord_WebHook.h>

#ifndef STASSID
#define STASSID "IXIUM"
#define STAPSK  "Mavic2Z00m"
#endif

const char* ssid = STASSID;
const char* password = STAPSK;


WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
const long utcOffsetInSeconds = 3600;

ESP8266WebServer server(80);

SFE_BMP180 bmp;
#define ALTITUDE  416.0 // metros 

uint8_t DHTPin = 2;
#define DHTTYPE DHT11
DHT dht(DHTPin, DHTTYPE);

const int led = 13;

float datos[7];

float temp1, temp2, sensacionTermica, humedad, presion, nivelMar, altura;

//////////// SETUP ////////////

void setup() {

  if(bmp.begin()){
    Serial.println("BMP180 Iniciado correctamente");
  } else {
    Serial.println("El sensor BMP180 tiene problemas");
  }
  
  dht.begin();
  Serial.begin(9600);


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
  timeClient.setTimeOffset(utcOffsetInSeconds);

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
    (void)url;         // example: /root/myfile.html
    (void)client;      // the webserver tcp client connection
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

      // Here the request is not interpreted, so we cannot for sure
      // swallow the exact amount matching the full request+content,
      // hence the tcp connection cannot be handled anymore by the
      // webserver.
#ifdef STREAMSEND_API
      // we are lucky
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
      // Two choices: return MUST STOP and webserver will close it
      //                       (we already have the example with '/fail' hook)
      // or                  IS GIVEN and webserver will forget it
      // trying with IS GIVEN and storing it on a dumb WiFiClient.
      // check the client connection: it should not immediately be closed
      // (make another '/dump' one to close the first)
      Serial.printf("\nTelling server to forget this connection\n");
      static WiFiClient forgetme = *client; // stop previous one if present and transfer client refcounter
      return ESP8266WebServer::CLIENT_IS_GIVEN;
    }
    return ESP8266WebServer::CLIENT_REQUEST_CAN_CONTINUE;
  });

  // Hook examples
  /////////////////////////////////////////////////////////

  server.begin();
  Serial.println("HTTP server started");

}


//////////// LOOP ////////////

void loop(void) {
  server.handleClient();
  MDNS.update();
  
  timeClient.setTimeOffset(utcOffsetInSeconds);
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

  timeClient.setTimeOffset(utcOffsetInSeconds);
  timeClient.update();
  
  unsigned long epochTime = timeClient.getEpochTime();  
  String hora = String(timeClient.getFormattedTime());  
  
  leerDatos();

  String xml = "<?xml version='1.0' encoding='UTF-8'?>\r\n";
  
  xml += "<meteorologica id='1'>\r\n";
  xml += "\t<fecha>" + String(epochTime) + "</fecha>\r\n";
  xml += "\t<hora>" + hora + "</hora>\r\n";
  xml += "\t<temperaturas media='" + String((temp1 + temp2) / 2) + "' sensacion='" + String(sensacionTermica, 2) + "' unidad ='ºC' >\r\n";
  xml += "\t\t<sensor1>" + String(temp1) + "</sensor1>\r\n";
  xml += "\t\t<sensor2>" + String(temp2) + "</sensor2>\r\n";
  xml += "\t</temperaturas>\r\n";
  xml += "\t<presion altura ='" + String(altura, 2) + "'>\r\n";
  xml += "\t\t<local>" + String(presion, 0) + "</local>";
  xml += "\t\t<mar>" + String(nivelMar, 0) + "</mar>\r\n";
  xml += "\t</presion>\r\n";
  xml += "\t<humedad>" + String(humedad, 0) + "</humedad>\r\n";
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

  leerDatos();
  
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
  cadena += "<tr><th>Temperatura 1</th><th>Temperatura 2</th><th>Temperatura Media</th><th>Sensación Térmica</th><th>Presión</th><th>Humedad</th><th>Altura Real</th></tr>";
  cadena += "<tr><td>" + String(temp1) + " ºC </td>";
  cadena += "<td>" + String(temp2) + " ºC </td>";
  cadena += "<td>" + String((temp1+temp2)/2) + " ºC</td>";
  cadena += "<td>" + String(sensacionTermica) + " ºC </td>";
  cadena += "<td>" + String(presion) + " Pa </td>";
  cadena += "<td>" + String(humedad) + " % </td>";
  cadena += "<td>" + String(altura) + " m. </td>";
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
  timeClient.setTimeOffset(utcOffsetInSeconds);
  timeClient.update();

  String hora = String(timeClient.getFormattedTime());

  leerDatos();
  
  String mensaje = "[ Hora: "+ hora +" ] ";
  mensaje +="[ T1: "+String(temp1, 2)+" ºC | T2: " + String(temp2, 2)+ " ºC | ST : " + String(sensacionTermica, 2)+" ºC | TM: " + String(((temp1 + temp2) / 2), 2) + " ºC ] ";
  mensaje +=" [ H: " + String(humedad, 0) + "% ]";
  mensaje +=" [ P: " + String(presion, 0) + " Pa ] ";
  mensaje +=" [ A: " + String(altura, 2) + " m ] ";
  
  send_discord(mensaje);
  
  Serial.println("Send data to Discord: OK");
  server.send(200, "text/plain", "OK");

}



void send_discord(String mensaje) {
  const String discord_webhook = "https://discord.com/api/webhooks/995371444961804490/kZZOPXQXdub5lNdA4j4LUv7xgWDGJtpZNqxwonphQb2yjIWBNf55TKA5hCOHCaz8sC8l";
  Discord_Webhook discord;
  discord.begin(discord_webhook);
  discord.send(mensaje);
  //delay(500);
}

void leerDatos() {
  
  char status;
  double T1,P,p0,a;

  temp2 = dht.readTemperature();
  humedad = dht.readHumidity();
  sensacionTermica = dht.computeHeatIndex(dht.readTemperature(), dht.readHumidity(), false);
  
  status = bmp.startTemperature();

  if(status!=0) {
        
    delay(status);

    status = bmp.getTemperature(T1);

    status = bmp.startPressure(3);

    if ( status != 0 ) {
      
      delay(status);

      status = bmp.getPressure(P,T1);

      if (status != 0) {
        
        p0 = bmp.sealevel(P,ALTITUDE); 
        a = bmp.altitude(P,p0);        
      }      
    }
  }
  
  
  temp1 = T1;
  presion = P;
  nivelMar = p0;
  altura = a;

}
