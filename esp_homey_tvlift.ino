/*
   esp8266 Wifi Homeyduino skeloton client

   See https://github.com/oh2th/esp_homey_tvlift
*/

#include <Arduino.h>
#include <Homey.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <base64.h>

#include "strutils.h"

/* ------------------------------------------------------------------------------- */
/* These are the pins for all ESP8266 boards */
//      Name   GPIO    Function
#define PIN_D0  16  // WAKE, Onboard LED
#define PIN_D1   5  // User
#define PIN_D2   4  // User
#define PIN_D3   0  // Low on boot means enter FLASH mode
#define PIN_D4   2  // TXD1 (must be high on boot to go to UART0 FLASH mode), Onboard LED
#define PIN_D5  14  // HSCLK
#define PIN_D6  12  // HMISO - APREQUEST
#define PIN_D7  13  // HMOSI  RXD2
#define PIN_D8  15  // HCS    TXD0 (must be low on boot to enter UART0 FLASH mode)
#define PIN_D9   3  //        RXD0
#define PIN_D10  1  //        TXD0

#define PIN_MOSI 8  // SD1
#define PIN_MISO 7  // SD0
#define PIN_SCLK 6  // CLK
#define PIN_HWCS 0  // D3

#define PIN_D11  9  // SD2
#define PIN_D12 10  // SD4
/* ------------------------------------------------------------------------------- */

// Configuration Wifi AP can be requested with PIN assigned on APREQUEST
#define APREQUEST PIN_D6
#define APTIMEOUT 180000

// TVLIFT UP and DOWN signals on defined PINs, should send a pulse for approx 500msec
#define TV_DOWN   PIN_D1        // TVlift down, pulse for 500 msec
#define TV_UP     PIN_D2        // TVlift up, pulse for 500 msec
#define TV_STATE  PIN_D5        // TVlift state, open/low = down, closed/high = up
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

unsigned long mytime = 0;       // Used for delaying, see loop function
char postmsg[256] = "";         // buffer for message to send

char currssid[33];              // current SSID
char currip[16];                // current IP address
char lastssid[33];              // last SSID
char lastip[16];                // last IP address

char devicename[32];            // Homey devicename

unsigned long portal_timer = 0;
unsigned long lastpacket = 0;

//Global variables used for the blink without delay example
unsigned long previousMillis = 0;
const unsigned long interval = 2000; //Interval in milliseconds

ESP8266WiFiMulti WiFiMulti;
ESP8266WebServer server(80);
IPAddress apIP(192, 168, 4, 1); // portal ip address
DNSServer dnsServer;
File file;

/* ------------------------------------------------------------------------------- */
volatile int state = false;
volatile int TVLiftState = false;
volatile int TVLiftPrevState = false;

/* ------------------------------------------------------------------------------- */
void setup() {
  pinMode(APREQUEST, INPUT_PULLUP);
  pinMode(PIN_D0, OUTPUT); // pin D0 and D4 are onboard leds on ESP12 boards
  pinMode(PIN_D4, OUTPUT);
  pinMode(TV_STATE, INPUT);
  // Init relay output HIGH bedore setting pinMode, this way relay will not pull on power on.
  digitalWrite(TV_DOWN, RELAY_OFF); pinMode(TV_DOWN, OUTPUT);
  digitalWrite(TV_UP, RELAY_OFF); pinMode(TV_UP, OUTPUT);

  Serial.begin(115200);

  SPIFFS.begin();

  if (SPIFFS.exists("/homey.txt")) {
    file = SPIFFS.open("/homey.txt", "r");
    file.readBytesUntil('\n', devicename, 32);
    file.close();
  } else {
    strcpy(devicename, "esp8266 Homey");
  }
  Serial.printf("\n\n%s\n", devicename);

  if (SPIFFS.exists("/last_wifi.txt")) {
    file = SPIFFS.open("/last_wifi.txt", "r");
    file.readBytesUntil('\n', lastssid, 33);
    if (lastssid[strlen(lastssid) - 1] == 13) {
      lastssid[strlen(lastssid) - 1] = 0;
    }
    file.readBytesUntil('\n', lastip, 16);
    if (lastip[strlen(lastip) - 1] == 13) {
      lastip[strlen(lastip) - 1] = 0;
    }
    file.close();
  } else {
    strcpy(lastssid, "none");
    strcpy(lastip, "none");
  }

  if (SPIFFS.exists("/known_wifis.txt")) {
    char ssid[33];
    char pass[65];
    WiFi.mode(WIFI_STA);
    file = SPIFFS.open("/known_wifis.txt", "r");
    while (file.available()) {
      memset(ssid, '\0', sizeof(ssid));
      memset(pass, '\0', sizeof(pass));
      file.readBytesUntil('\t', ssid, 32);
      file.readBytesUntil('\n', pass, 64);
      WiFiMulti.addAP(ssid, pass);
      Serial.printf("wifi loaded: %s / %s\n", ssid, pass);
    }
    file.close();
  } else {
    startPortal(); // no settings were found, so start the portal without button
  }
  //Start Homey library
  Homey.begin(devicename);
  /* Note:
     The name of each Arduino on the network needs to be unique
     since it will be used as the unique identifier for your device.
     The name is stored as a String and can thus be as long as you
     desire it to be.
  */

  Homey.setClass("socket");
  Homey.addCapability("onoff", setState);
  Homey.addAction("output", setState);
  Homey.addCondition("state", getState);

  Serial.println("Homey init complete.");
  digitalWrite(PIN_D0, HIGH); // LEDs off
  digitalWrite(PIN_D4, HIGH);
}

/* ------------------------------------------------------------------------------- */
void loop() {
  if (WiFi.getMode() == WIFI_STA) {

    char foo[64];
    if (WiFiMulti.run() != WL_CONNECTED) {
      currssid[0] = '\0';
      delay(1000);
    } else {
      WiFi.SSID().toCharArray(foo, 64);
      if (strcmp(currssid, foo) != 0) {
        strcpy(currssid, foo);
      }
      WiFi.localIP().toString().toCharArray(foo, 64);
      if (strcmp(currip, foo) != 0) {
        strcpy(currip, foo);
      }
      // if our connection has changed, save last wifi info
      if (strcmp(currip, lastip) != 0 || strcmp(currssid, lastssid) != 0) {
        strcpy(lastip, currip);
        strcpy(lastssid, currssid);
        file = SPIFFS.open("/last_wifi.txt", "w");
        file.printf("%s\n%s\n", lastssid, lastip);
        file.close();
      }
    }

    if ((WiFiMulti.run() == WL_CONNECTED)) {
      digitalWrite(PIN_D0, LOW); // led on when in connect

      //Handle Homey incoming connections
      Homey.loop();
      /* Note:
          The Homey.loop(); function needs to be called as often as possible.
          Failing to do so will cause connection problems and instability.
          Avoid using the delay function at all times. Instead please use the
          method explaind on the following page on the Arduino website:
          https://www.arduino.cc/en/Tutorial/BlinkWithoutDelay
      */

      //This is the 'blink without delay' code
      unsigned long currentMillis = millis();
      if (currentMillis - previousMillis > interval) {
        previousMillis = currentMillis;

        //(This code will be executed every <interval> milliseconds.)

        TVLiftState = digitalRead(TV_STATE);
        if (TVLiftState == HIGH) {
          Homey.setCapabilityValue("onoff", true);
          digitalWrite(PIN_D4, HIGH); // interval led on as a heartbeat
          Serial.println("TVLiftState is HIGH = TV is up");
        } else {
          Homey.setCapabilityValue("onoff", false);
          Serial.println("TVLiftState is LOW = TV is down");
          digitalWrite(PIN_D4, LOW); // led off
        }
        if (TVLiftState != TVLiftPrevState) {
          // TVLift State changed
          Homey.trigger("state", TVLiftState);
          TVLiftPrevState = TVLiftState;
        }
      }
    }
  } else if (WiFi.getMode() == WIFI_AP) { // portal mode
    dnsServer.processNextRequest();
    server.handleClient();

    // blink onboard leds if we are in portal mode
    if (int(millis() % 1000) < 500) {
      digitalWrite(PIN_D0, LOW);
      digitalWrite(PIN_D4, HIGH);
    } else {
      digitalWrite(PIN_D0, HIGH);
      digitalWrite(PIN_D4, LOW);
    }
  }
  if (digitalRead(APREQUEST) == LOW && WiFi.getMode() == WIFI_STA) {
    startPortal();
  }
  if (millis() - portal_timer > APTIMEOUT && WiFi.getMode() == WIFI_AP) {
    Serial.println("Portal timeout. Booting.");
    delay(1000);
    ESP.restart();
  }
}

void setState() {
  state = Homey.value.toInt();
  applyState();
}

void applyState() {
  if (state == HIGH && TVLiftState == LOW ) {
    digitalWrite(TV_UP, RELAY_ON);
    delay(200);
    digitalWrite(TV_UP, RELAY_OFF);
    Serial.println("Bring TV up");
  } else if (state == LOW && TVLiftState == HIGH ) {
    digitalWrite(TV_DOWN, RELAY_ON);
    delay(200);
    digitalWrite(TV_DOWN, RELAY_OFF);
    Serial.println("Bring TV down");
  } else {
    Serial.println("Nothing to do.");
  }
}

void getState() {
  Serial.println("getState(): state is " + String(state));
  return Homey.returnResult(state);
}

/* ------------------------------------------------------------------------------- */
/* Portal code begins here                                                         */
/* ------------------------------------------------------------------------------- */

void startPortal() {
  portal_timer = millis();
  WiFi.disconnect();
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("ESP8266 HOMEY");

  dnsServer.setTTL(300);
  dnsServer.setErrorReplyCode(DNSReplyCode::ServerFailure);
  dnsServer.start(53, "*", apIP);

  server.on("/", httpRoot);
  server.on("/style.css", httpStyle);
  server.on("/wifis.html", httpWifis);
  server.on("/savewifi", httpSaveWifi);
  server.on("/boot", httpBoot);

  server.onNotFound([]() {
    server.sendHeader("Refresh", "1;url=/");
    server.send(404, "text/plain", "QSD QSY");
  });
  server.begin();
  Serial.println("Started portal");
}
/* ------------------------------------------------------------------------------- */

void httpRoot() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/index.html", "r");
  html = file.readString();
  file.close();
  html.replace("###LASTSSID###", lastssid);
  html.replace("###LASTIP###", lastip);

  server.send(200, "text/html; charset=UTF-8", html);
}

/* ------------------------------------------------------------------------------- */

void httpWifis() {
  String html;
  char tablerows[1024];
  char rowbuf[256];
  char ssid[33];
  char pass[33];
  int counter = 0;

  portal_timer = millis();
  memset(tablerows, '\0', sizeof(tablerows));

  file = SPIFFS.open("/wifis.html", "r");
  html = file.readString();
  file.close();

  if (SPIFFS.exists("/known_wifis.txt")) {
    file = SPIFFS.open("/known_wifis.txt", "r");
    while (file.available()) {
      memset(rowbuf, '\0', sizeof(rowbuf));
      memset(ssid, '\0', sizeof(ssid));
      memset(pass, '\0', sizeof(pass));
      file.readBytesUntil('\t', ssid, 33);
      file.readBytesUntil('\n', pass, 33);
      sprintf(rowbuf, "<tr><td>SSID</td><td><input type=\"text\" name=\"ssid%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, ssid);
      strcat(tablerows, rowbuf);
      sprintf(rowbuf, "<tr><td>PASS</td><td><input type=\"text\" name=\"pass%d\" maxlength=\"32\" value=\"%s\"></td></tr>", counter, pass);
      strcat(tablerows, rowbuf);
      counter++;
    }
    file.close();
  }
  html.replace("###TABLEROWS###", tablerows);
  html.replace("###COUNTER###", String(counter));

  if (counter > 3) {
    html.replace("table-row", "none");
  }

  server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpSaveWifi() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/known_wifis.txt", "w");

  for (int i = 0; i < server.arg("counter").toInt(); i++) {
    if (server.arg("ssid" + String(i)).length() > 0) {
      file.print(server.arg("ssid" + String(i)));
      file.print("\t");
      file.print(server.arg("pass" + String(i)));
      file.print("\n");
    }
  }
  // Add new
  if (server.arg("ssid").length() > 0) {
    file.print(server.arg("ssid"));
    file.print("\t");
    file.print(server.arg("pass"));
    file.print("\n");
  }
  file.close();

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "3;url=/");
  server.send(200, "text/html; charset=UTF-8", html);
}
/* ------------------------------------------------------------------------------- */

void httpStyle() {
  portal_timer = millis();
  String css;

  file = SPIFFS.open("/style.css", "r");
  css = file.readString();
  file.close();
  server.send(200, "text/css", css);
}
/* ------------------------------------------------------------------------------- */

void httpBoot() {
  portal_timer = millis();
  String html;

  file = SPIFFS.open("/ok.html", "r");
  html = file.readString();
  file.close();

  server.sendHeader("Refresh", "3;url=about:blank");
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);
  ESP.restart();
}
/* ------------------------------------------------------------------------------- */
