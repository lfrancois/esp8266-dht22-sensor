#include "../config.h"
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <runningaverage.h>
#include <array>

// SSD1306 OLED screen includes
#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"
#include "OLEDDisplayImages.h"

/***************************
 * Begin Settings
 **************************/

// DS18S20 Right now not tested yet, most likely we will have 
// to change the PIN #
#ifdef SENSOR_DS18S20
#include <OneWire.h>
#define ONE_WIRE_BUS 2  // DS18S20 pin
OneWire ds(ONE_WIRE_BUS);
#endif

// DHT22 sensor plugged to D6 for us on our NodeMCU
#ifdef SENSOR_DHT22
#include <DHT.h>
#include "../config.h"
#define DHTPIN D6
DHT dht(DHTPIN, DHTTYPE, 11); // 11 works fine for ESP8266
#endif

// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;
const int SDA_PIN = D2;
const int SDC_PIN = D3;

// Initialize the oled display for address 0x3c
// sda-pin=D2 and sdc-pin=D3
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);
OLEDDisplayUi   ui( &display );

/***************************
 * End Settings
 **************************/

//declaring prototypes
void updateDisplay(OLEDDisplay *display);
void updateThingspeak();
void updateInfluxDB();

MDNSResponder mdns;
ESP8266WebServer server(80);
// TODO: std::array nutzen
std::array<RunningAverage*, MAX_NUM_SENSORS> temp_aggregators;
std::array<RunningAverage*, MAX_NUM_SENSORS> hum_aggregators;
std::array<bool, MAX_NUM_SENSORS> sensors_ok;
//RunningAverage temp_aggregator(6);
//RunningAverage hum_aggregator(6);
//bool sensor_ok = false;
enum SensorState {
	MEASURED_OK,
	MEASURED_FAILED,
	TOO_EARLY
};

String webString="";     // String to display
// Generally, you should use "unsigned long" for variables that hold time
std::array<unsigned long, MAX_NUM_SENSORS> previousMillis;        // will store last temp was read
const long interval = 10000;             // interval at which to read sensor

// read the sensor with the given bus index. Sets temperature in temp,
// humidity in humidity. Returns the SensorState.
int ICACHE_FLASH_ATTR read_sensors(byte bus_idx, float& temp, float& humidity) {
  // Wait at least 2 seconds seconds between measurements.
  // if the difference between the current time and last time you read
  // the sensor is bigger than the interval you set, read the sensor
  // Works better than delay for things happening elsewhere also
  unsigned long currentMillis = millis();
 
  if(currentMillis - previousMillis[bus_idx] >= interval) {
    // save the last time you read the sensor 
    previousMillis[bus_idx] = currentMillis;

#ifdef SENSOR_DS18S20
		// This is the code for receiving temperature readings from a DS18S20.
		// see https://github.com/esp8266/Arduino/blob/esp8266/libraries/OneWire/examples/DS18x20_Temperature/DS18x20_Temperature.pde
		byte i;
		byte present = 0;
		byte type_s;
		byte data[12];
		byte addr[MAX_NUM_SENSORS][8]; // can hold four addresses of 8 byte each
		byte addr_count = 0;

		ds.reset();
		ds.reset_search();

		for (addr_count = 0; addr_count < MAX_NUM_SENSORS; ++addr_count) {
			if ( !ds.search(addr[addr_count])) {
				//Serial.println("No more addresses.");
				ds.reset_search();
				delay(250);
				break;
			} //else {
				//Serial.print("found address ");
				//for( i = 0; i < 8; i++) {
				//	Serial.write(' ');
				//	Serial.print(addr[addr_count][i], HEX);
				//}
				//Serial.println();
			//}
		}

		//Serial.print("found ");
		//Serial.print(addr_count);
		//Serial.println(" addresses.");

		if (addr_count == 0) {
			Serial.println("No sensors found.");
			return MEASURED_FAILED;
		}

		if (bus_idx >= addr_count) {
			Serial.print("Requested sensor ");
			Serial.print(bus_idx);
			Serial.println(" but not that many sensors on bus.");
			return MEASURED_FAILED;
		}

		if (OneWire::crc8(addr[bus_idx], 7) != addr[bus_idx][7]) {
			Serial.println("CRC is not valid!");
			return MEASURED_FAILED;
		}
		//Serial.println();

		// the first ROM byte indicates which chip
		switch (addr[bus_idx][0]) {
			case 0x10:
				//Serial.println("  Chip = DS18S20");  // or old DS1820
				type_s = 1;
				break;
			case 0x28:
				//Serial.println("  Chip = DS18B20");
				type_s = 0;
				break;
			case 0x22:
				//Serial.println("  Chip = DS1822");
				type_s = 0;
				break;
			default:
				Serial.println("Device is not a DS18x20 family device.");
				return MEASURED_FAILED;
		} 

		ds.reset();
		ds.select(addr[bus_idx]);

		ds.write(0x44, 0);        // start conversion, no parasitic power

		delay(750);     // maybe 750ms is enough, maybe not
		// we might do a ds.depower() here, but the reset will take care of it.

		present = ds.reset();
		ds.select(addr[bus_idx]);
		ds.write(0xBE);         // Read Scratchpad

		//Serial.print("  Data = ");
		//Serial.print(present, HEX);
		//Serial.print(" ");
		for ( i = 0; i < 9; i++) {           // we need 9 bytes
			data[i] = ds.read();
			//Serial.print(data[i], HEX);
			//Serial.print(" ");
		}
		//Serial.print(" CRC=");
		//Serial.print(OneWire::crc8(data, 8), HEX);
		//Serial.println();

		// Convert the data to actual temperature
		// because the result is a 16 bit signed integer, it should
		// be stored to an "int16_t" type, which is always 16 bits
		// even when compiled on a 32 bit processor.
		int16_t raw = (data[1] << 8) | data[0];
		if (type_s) {
			raw = raw << 3; // 9 bit resolution default
			if (data[7] == 0x10) {
				// "count remain" gives full 12 bit resolution
				raw = (raw & 0xFFF0) + 12 - data[6];
			}
		} else {
			byte cfg = (data[4] & 0x60);
			// at lower res, the low bits are undefined, so let's zero them
			if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
			else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
			else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
			//// default is 12 bit resolution, 750 ms conversion time
		}
		temp = (float)raw / 16.0;
		//Serial.print("Temperature: ");
		//Serial.println(temp);
#endif // of DS18S20-related code

#ifdef SENSOR_DHT22 // Read temp&hum from DHT22
		// Reading temperature for humidity takes about 250 milliseconds!
		// Sensor readings may also be up to 2 seconds 'old' (it's a very slow sensor)
		humidity = dht.readHumidity();          // Read humidity (percent)
		temp = dht.readTemperature(false);     // Read temperature as Celsius
		Serial.print("Temperature: ");
		Serial.println(temp);
		Serial.print("Humidity: ");
		Serial.println(humidity);
#endif

		//Serial.print("Free heap:");
		//Serial.println(ESP.getFreeHeap(),DEC);

		if (isnan(temp) || temp==85.0 || temp==(-127.0)) {
			Serial.println("Failed to read from sensor");
			// resetting the previous measurement time so that a failed attempt
			// will be repeated with the next query.
			previousMillis[bus_idx]=currentMillis-2000;
			if (previousMillis[bus_idx] < 0) previousMillis[bus_idx] = 0;
			return MEASURED_FAILED;
		} else {
			return MEASURED_OK;
		}
	} else {
		return TOO_EARLY; // no measurement taken - time not elapsed
	}
}


void ICACHE_FLASH_ATTR handleNotFound(){
	String message = "File Not Found\n\n";
	message += "URI: ";
	message += server.uri();
	message += "\nMethod: ";
	message += (server.method() == HTTP_GET)?"GET":"POST";
	message += "\nArguments: ";
	message += server.args();
	message += "\n";
	for (uint8_t i=0; i<server.args(); i++){
		message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
	}
	server.send(404, "text/plain", message);
}

void ICACHE_FLASH_ATTR setup(void){
	// TODO: Create cache collection
	for(byte i=0; i < MAX_NUM_SENSORS; ++i) {
		temp_aggregators[i] = new RunningAverage(6);
		hum_aggregators[i] = new RunningAverage(6);
		sensors_ok[i] = false;
	}

	Serial.begin(9600);
	delay(500);

    // Turn On VCC
    pinMode(D4, OUTPUT);
    digitalWrite(D4, HIGH);
    // initialize display
    display.init();
    display.clear();
    display.display();

    //display.flipScreenVertically();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER);
    display.setContrast(255);

	WiFi.begin(ssid, password);
	Serial.println("");
	Serial.println("Wifi temperature sensor v0.1");

	// Wait for connection
    int counter = 0;
	while (WiFi.status() != WL_CONNECTED && counter < 10) {
		delay(500);
		Serial.print(".");
        display.clear();
        display.drawString(64, 10, "Connecting to WiFi");
        display.drawXbm(46, 30, 8, 8, counter % 3 == 0 ? activeSymbol : inactiveSymbol);
        display.drawXbm(60, 30, 8, 8, counter % 3 == 1 ? activeSymbol : inactiveSymbol);
        display.drawXbm(74, 30, 8, 8, counter % 3 == 2 ? activeSymbol : inactiveSymbol);
        display.display();

        counter++;
	}
    ui.setTargetFPS(30);

    //Hack until disableIndicator works:
    //Set an empty symbol
    ui.setActiveSymbol(emptySymbol);
    ui.setInactiveSymbol(emptySymbol);

    ui.disableIndicator();

    // Inital UI takes care of initalising the display too.
    ui.init();

	Serial.println("");
	Serial.print("Connected to ");
	Serial.println(ssid);
	Serial.print("IP address: ");
	Serial.println(WiFi.localIP());

	if (mdns.begin(mdnsname, WiFi.localIP())) {
		Serial.println("MDNS responder started");
	}

	server.on("/", [](){
			webString = "Measurements of device " + String(mdnsname) + "\n";
			for (byte sensor_idx = 0; sensor_idx < MAX_NUM_SENSORS; ++sensor_idx) {
				if (sensors_ok[sensor_idx]) {       // read sensor
					webString+="Sensor "+String(sensor_idx)+" temperature: "+String(temp_aggregators[sensor_idx]->getAverage())+" degree Celsius\n";
#ifdef SENSOR_DHT22 // Read temp&hum from DHT22
					webString+="Sensor "+String(sensor_idx)+" humidity: "+String(hum_aggregators[sensor_idx]->getAverage())+" % r.H.\n";
#endif
				} else {
					webString+="Sensor "+String(sensor_idx)+": Cannot read data from sensor \n";
				}
			}
			server.send(200, "text/plain", webString);
		});
	// Append the index of the sensor to read to the URI. So /temperature/1
	// would attempt to return the value of the second temperature sensor.
	for (byte sensor_idx = 0; sensor_idx < MAX_NUM_SENSORS; ++sensor_idx) {
		String url("/temperature/" + String(sensor_idx));
		server.on(url.c_str(), [sensor_idx](){
				if (sensors_ok[sensor_idx]) {       // read sensor
					webString="{\"temperature\": "+String(temp_aggregators[sensor_idx]->getAverage())+",\"unit\": \"Celsius\"}";
					server.send(200, "text/plain", webString);
				} else {
					webString="{\"error\": \"Cannot read data from sensor "+String(sensor_idx)+".\"}";
					server.send(503, "text/plain", webString);
				}
			});
		Serial.println("Registered handler for "+url);
	}
	// add a default redirect: /temperature should redirect to /temperature/0
	server.on("/temperature", []() {
				webString = "Redirecting to /temperature/0";
				server.sendHeader("Location", "/temperature/0");
				server.send(301, "text/plain", webString);
			});
#ifdef SENSOR_DHT22 // Read humidity from DHT22
	server.on("/humidity", [](){
			if (sensors_ok[0]) {       // read sensor
				webString="{\"humidity\": "+String(hum_aggregators[0]->getAverage())+",\"unit\": \"% r.H.\"}";
				server.send(200, "text/plain", webString);
			} else {
				webString="{\"error\": \"Cannot read data from sensor.\"}";
				server.send(503, "text/plain", webString);
			}
			});

#endif

	server.onNotFound(handleNotFound);

	server.begin();
	ESP.wdtEnable(5000);
}

void ICACHE_FLASH_ATTR loop(void){
    // clear the display
    display.clear();

    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(10, 128, String(millis()));
    // write the buffer to the display
    display.display();

    updateDisplay(&display);
    updateInfluxDB();
    updateThingspeak();
    delay(10);
    server.handleClient();
	ESP.wdtFeed();
	for (byte sensor_idx = 0; sensor_idx < MAX_NUM_SENSORS; ++sensor_idx) {
		float temp = 0.0;
		float humidity = 0.0;
		switch (read_sensors(sensor_idx, temp, humidity)) {
			case MEASURED_OK:
				sensors_ok[sensor_idx] = true;
				temp_aggregators[sensor_idx]->addValue(temp);
				hum_aggregators[sensor_idx]->addValue(humidity);
				break;
			case MEASURED_FAILED:
				Serial.println("Measurement failed");
				sensors_ok[sensor_idx] = false;
				break;
			case TOO_EARLY:
				;;
				break;
		}
	}

}

void updateDisplay(OLEDDisplay *display) {
  String temperature;
  String humidity;

  Serial.print("Update Data");
  temperature = String(temp_aggregators[0]->getAverage()) + " °C";
  humidity = String(hum_aggregators[0]->getAverage()) + "% r.H.";
  display->clear();
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_16);
  display->drawString(64, 10, temperature);
  display->drawString(64, 40, humidity);
  display->display();
  delay(10000);
}

void updateInfluxDB() {
    String InfluxData;
    String temperature = String(temp_aggregators[0]->getAverage());
    String humidity = String(hum_aggregators[0]->getAverage());

    Serial.println("Writing data to host " + String(INFLUXDB_HOST) + ":" + INFLUXDB_PORT + "'s database=" + String(DATABASE));
    InfluxData = "ekho-" + String(mdnsname) + " temp=" + temperature + ",humidity=" + humidity;

    WiFiClient client;
    if (!client.connect(INFLUXDB_HOST, INFLUXDB_PORT)) {
        Serial.println("connection failed");
        return;
    }
    delay(10);
    client.println("POST /write?db=" + String(DATABASE) + " HTTP/1.1");
    Serial.println("POST /write?db=" + String(DATABASE) + " HTTP/1.1");
    client.println("Host: " + String(INFLUXDB_HOST) + ":" + String(INFLUXDB_PORT));
    Serial.println("Host: " + String(INFLUXDB_HOST) + ":" + String(INFLUXDB_PORT));
    client.println("Connection: close");
    Serial.println("Connection: close");
    client.println("Content-Type: application/x-www-form-urlencoded");
    Serial.println("Content-Type: application/x-www-form-urlencoded");
    client.print("Content-Length: ");
    Serial.print("Content-Length: ");
    client.println(InfluxData.length());
    Serial.println(InfluxData.length());
    client.println();
    Serial.println();
    client.println(InfluxData);
    Serial.println(InfluxData);

    delay(50);
    Serial.println("Reply from InfluxDB");
    while(client.available()) {
        Serial.print((char)client.read());
    }
    Serial.println();
    client.stop();
}

void updateThingspeak() {
    String temperature = String(temp_aggregators[0]->getAverage());
    String humidity = String(hum_aggregators[0]->getAverage());
    String url = "/update?api_key=";
    url += THINGSPEAK_API_KEY;
    url += "&field1=";
    url += temperature;
    url += "&field2=";
    url += humidity;

    Serial.print("Requesting URL: ");
    Serial.println(url);

    // This will send the request to the server
    Serial.print("connecting to ");
    Serial.println(THINGSPEAK_HOST);

    // Use WiFiClient class to create TCP connections
    WiFiClient client;
    const int httpPort = 80;
    if (!client.connect(THINGSPEAK_HOST, httpPort)) {
        Serial.println("connection failed");
        return;
    }
    delay(10);
    client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + THINGSPEAK_HOST + "\r\n" +
                 "Connection: close\r\n\r\n");
    delay(10);
    Serial.println();
    Serial.println("closing connection");
}
