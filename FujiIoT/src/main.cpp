#include <Arduino.h>
#include <Scheduler.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <RemoteDebug.h>
#include <TimeLib.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "FujiHeatPump.h"

#ifndef STASSID
#define STASSID "TEST"
#define STAPSK "TEST"
#endif

#define APP_MQTT_CONNECTED_FLAG     BIT0
#define APP_LIN_COMM_FLAG           BIT1
#define APP_FORCE_SEND_STATE_FLAG   BIT2

uint32_t app_flags = 0;

// NTP Servers:
static const char ntpServerName[] = "es.pool.ntp.org";

const int timeZone = 2;     // Central European Time

const char* host = "fuji-ctl";
const char* ssid = STASSID;
const char* password = STAPSK;
const char* SN = "fuji-ctl";

char attrs_topic[64];
char cmd_topic[64];
char cmdexe_topic[64];

char msg[128];

typedef struct {
    char host[64];
    uint16_t port;
    char user[32];
    char pwd [32];
    char apikey[32];
} MQTT_SERVER_DATA;

const MQTT_SERVER_DATA mqtt_server_data = {"host",1883,"test","test-1234","home"};

#define WEBSOCKET_DISABLED true

WiFiClient espClient;
ESP8266WebServer server(80);
PubSubClient client(espClient);

RemoteDebug Debug;
FujiHeatPump hp;
WiFiUDP Udp;
StaticJsonDocument<200> cmd_json;
DeserializationError err_json;

FujiFrame last_send_ff;

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address) {

    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);

    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    Udp.beginPacket(address, 123); //NTP requests are to port 123
    Udp.write(packetBuffer, NTP_PACKET_SIZE);
    Udp.endPacket();

}

time_t getNtpTime() {

    IPAddress ntpServerIP; // NTP server's ip address

    while (Udp.parsePacket() > 0) ; // discard any previously received packets

    // get a random server from the pool
    WiFi.hostByName(ntpServerName, ntpServerIP);
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();

    while (millis() - beginWait < 1500) {
        int size = Udp.parsePacket();
        if (size >= NTP_PACKET_SIZE) {
        Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
        unsigned long secsSince1900;
        // convert four bytes starting at location 40 to a long integer
        secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
        secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
        secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
        secsSince1900 |= (unsigned long)packetBuffer[43];
        return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
        }
    }
    return 0; // return 0 if unable to get the time

}

void mqtt_connect() {

    Serial1.print(F("Attempting MQTT connection..."));    

    // Attempt to connect
    bool result = false;

    if (strlen(mqtt_server_data.user) == 0) {
        result = client.connect(SN);
    }
    else {
        result = client.connect(SN,mqtt_server_data.user,mqtt_server_data.pwd);
    }

    if (result) {
    
        Serial1.println(F("[MQTT-Connect] Connected"));
        //WiFi.localIP()
        //if (app_flags & APP_MQTT_CONNECTED_FLAG) 
        client.publish(attrs_topic, "{\"status\":\"reconnect\"}");
        client.subscribe(cmd_topic);

    } 
    else {
    
        Serial1.print(F("[MQTT-Connect] Failed! RC:"));
        Serial1.println(client.state());

    }

}

void callback(char* topic, byte* payload, unsigned int length) {
    
    err_json = deserializeJson(cmd_json, payload);

    if (err_json) {

        debugI("DeserializeJson() failed with code %s\n",err_json.f_str());

        if (strncmp((char *) payload,"status",length) == 0) {

            if (hp.getOnOff()) {
                sprintf(msg,"{\"power\":\"on\",\"mode\":%u,\"fan\":%u,\"temp\":%u,\"eco\":%u}",
                        hp.getMode(),
                        hp.getFanMode(),
                        hp.getTemp(),
                        hp.getEconomyMode());
            }
            else {
                sprintf(msg,"{\"power\":\"off\"}");
            }

            client.publish(cmdexe_topic, msg);

        }

    }
    else {

        // Get a reference to the root object
        JsonObject obj = cmd_json.as<JsonObject>();

        if (obj.containsKey("power")) {

            if (strcmp(cmd_json["power"],"on") == 0) {
                hp.setOnOff(true);      
                app_flags |= APP_FORCE_SEND_STATE_FLAG;
            }

            if (strcmp(cmd_json["power"],"off") == 0) {
                hp.setOnOff(false);
                app_flags |= APP_FORCE_SEND_STATE_FLAG;
            }

        }

        if (obj.containsKey("temp")) {
            hp.setTemp(cmd_json["temp"]);
            app_flags |= APP_FORCE_SEND_STATE_FLAG;
        }

        if (obj.containsKey("mode")) {
            hp.setMode(cmd_json["mode"]);
            app_flags |= APP_FORCE_SEND_STATE_FLAG;
        }

        if (obj.containsKey("fan")) {
            hp.setFanMode(cmd_json["fan"]);
            app_flags |= APP_FORCE_SEND_STATE_FLAG;
        }

        if (obj.containsKey("eco")) {
            if (strcmp(cmd_json["eco"],"0") == 0){
                hp.setEconomyMode(false);
            }
            else {
                hp.setEconomyMode(true);
            }
            app_flags |= APP_FORCE_SEND_STATE_FLAG;
        }

    }
}

class HttpServerTask : public Task {
protected:

    void setup() {

        debugI("HttpServer task Setup");
        
        // Ruta para '/'
        server.on("/", []() {
            server.send(200, "text/plain", "WebServer");
        });
        
        // Ruta para URI desconocida
        server.onNotFound( []() {
            server.send(404, "text/plain", "Not found");
        });
        
        // Iniciar servidor
        server.begin();
        debugI("HttpServer Starts");

    }

    void loop()  {
        yield();
    }

} httpserver_task;

class FujiTask : public Task {
protected:

    void setup() {

        debugI("Fuji task Setup");
        digitalWrite(0,HIGH);
        hp.connect(&Serial, true); // use Serial and bind as a secondary controller

    }

    void loop()  {

        if (hp.waitForFrame()) {   // attempt to read state from bus and place a reply frame in the buffer
            
            delay(60);              // frames should be sent 50-60ms after recieving - potentially other work can be done here
            hp.sendPendingFrame();  // send any frame waiting in the buffer
        }
          
        yield();

    }

} fuji_task;

class MQTTTask : public Task {
protected:

    void setup() {
        sprintf(attrs_topic,"/%s/%s/attrs",mqtt_server_data.apikey,SN);
        sprintf(cmd_topic,"/%s/%s/cmd",mqtt_server_data.apikey,SN);
        sprintf(cmdexe_topic,"/%s/%s/cmdexe",mqtt_server_data.apikey,SN);
        client.setServer(mqtt_server_data.host,mqtt_server_data.port);
        client.setCallback(callback);
    }

    void loop()  {

        if (!client.connected()) {
            while (!client.connected()) {
                mqtt_connect();
                delay(5000);
            }
        }
        else {

            if ((app_flags & APP_FORCE_SEND_STATE_FLAG) || (memcmp(&last_send_ff,hp.getCurrentState(),9) != 0)) {

                memcpy(&last_send_ff,hp.getCurrentState(),sizeof(FujiFrame));

                if (hp.getOnOff()) {
                    sprintf(msg,"{\"power\":\"on\",\"mode\":%u,\"fan\":%u,\"temp\":%u,\"eco\":%u}",
                            hp.getMode(),
                            hp.getFanMode(),
                            hp.getTemp(),
                            hp.getEconomyMode());
                }
                else {
                    sprintf(msg,"{\"power\":\"off\"}");
                }

                client.publish(attrs_topic, msg);

                app_flags &= ~APP_FORCE_SEND_STATE_FLAG;

            }
/*
            if ((millis() - last_lin_comm) > MAX_LIN_COMM_TIMEOUT)) {

            }
*/
        }

        yield();

    }

} mqtt_task;

class MainLoopTask : public Task {
protected:
    void loop()  {
        Debug.handle();
        ArduinoOTA.handle();
        client.loop();
        server.handleClient();
        yield();
    }
} mainloop_task;

void setup(void) {

    //Configuración  del GPIO0
    pinMode(0, OUTPUT);
    digitalWrite(0,LOW);

    Serial1.begin(115000);

    delay(500);

    Serial1.println(F("\nSETUP Main routine! Booting ...."));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        WiFi.begin(ssid, password);
    }

    Serial1.println("Ready");
    Serial1.print("IP address: ");
    Serial1.println(WiFi.localIP());
    Serial1.println("Starting UDP");
    Udp.begin(8888);
    Serial1.print("Local port: ");
    Serial1.println(Udp.localPort());
    Serial1.println("Waiting for sync");
    setSyncProvider(getNtpTime);
    setSyncInterval(300);
    Serial1.println("Done!");


    Debug.begin(host);
    Debug.setResetCmdEnabled(true); // Enable the reset command
    
    ArduinoOTA.onStart([]() {
		Serial1.println("* OTA: Start");
	});
	ArduinoOTA.onEnd([]() {
		Serial1.println("\n*OTA: End");
	});
	ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
		Serial1.printf("*OTA: Progress: %u%%\r", (progress / (total / 100)));
	});
	ArduinoOTA.onError([](ota_error_t error) {
		Serial1.printf("*OTA: Error[%u]: ", error);
		if (error == OTA_AUTH_ERROR) Serial1.println("Auth Failed");
		else if (error == OTA_BEGIN_ERROR) Serial1.println("Begin Failed");
		else if (error == OTA_CONNECT_ERROR) Serial1.println("Connect Failed");
		else if (error == OTA_RECEIVE_ERROR) Serial1.println("Receive Failed");
		else if (error == OTA_END_ERROR) Serial1.println("End Failed");
	});

    ArduinoOTA.begin();

    //Scheduler.start(&print_task);
    Scheduler.start(&fuji_task);
    Scheduler.start(&mqtt_task);
    Scheduler.start(&httpserver_task);
    Scheduler.start(&mainloop_task);
    Scheduler.begin();

}

void loop(void) {}

