#include <Arduino.h>
#include <SoftwareSerial.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>

//enable to have transparent serial connection to LoRa
//#define LoRaConfig 1

//WDT
//3 seconds WDT
#define WDT_TIMEOUT 19
esp_err_t ESP32_ERROR;

//1-wire
// GPIO where the DS18B20 is connected to
const int oneWireBus = 32;     
// Setup a oneWire instance to communicate with any OneWire devices
OneWire oneWire(oneWireBus);
// Pass our oneWire reference to Dallas Temperature sensor 
DallasTemperature sensors(&oneWire);

//LoRa
#define LRx 14
#define LTx 12
SoftwareSerial LoRa(LRx, LTx);
int LoRaPowerStatus;

//function to send a string to LoRa, must end with \0
int LoRaSendStr(char *s) {
  int i=0;

  //check \n
  Serial.print("Sending:");
  while(s[i]!='\0' && i<strlen(s)) {
    Serial.write(s[i]);
    LoRa.write(s[i]);
    i++;
  }
  Serial.println("");
  return(1);
}

void setup() {
  byte RxData;
  int pinM = 0; //LoRa TxRx mode

  // Start the Serial Monitor
  #ifdef LoRaConfig
  Serial.begin(9600); //for testing only
  pinM = 1;
  #else
  Serial.begin(115200); //115200
  #endif

  delay(100);
  Serial.print("Millis ");
  Serial.println(millis());
  Serial.println("Configuring WDT...");
  Serial.print("Watchdog Timeout (in seconds) set to : ");
  Serial.println(WDT_TIMEOUT);
  esp_task_wdt_deinit();
  // Task Watchdog configuration
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT * 1000,                 // Convertin ms
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,  // Bitmask of all cores, https://github.com/espressif/esp-idf/blob/v5.2.2/examples/system/task_watchdog/main/task_watchdog_example_main.c
    .trigger_panic = true                             // Enable panic to restart ESP32
  };
  #ifndef LoRaConfig
  //start watchdog
  ESP32_ERROR = esp_task_wdt_init(&wdt_config);
  Serial.println("Last Reset : " + String(esp_err_to_name(ESP32_ERROR)));
  esp_task_wdt_add(NULL);  //add current thread to WDT watch
  #endif

  // Start the DS18B20 sensor
  sensors.begin();
  LoRa.begin(9600);
  //Setup LoRa
  //Reset LoRa
  pinMode(25, OUTPUT); digitalWrite(25, 1);
  pinMode(26, OUTPUT); digitalWrite(26, 1);
  delay(200);
  LoRaSendStr("AT+POWER=3");
  delay(200);
  LoRaSendStr("AT+RESET");
  //Set LoRa mode
  delay(4000);
  pinMode(25, OUTPUT); digitalWrite(25, pinM);
  pinMode(26, OUTPUT); digitalWrite(26, pinM);
  LoRaPowerStatus = 3;

  Serial.print("Ready");

#ifdef LoRaConfig
  //LoRa loop connection
  while(1) {
    if (Serial.available()) {      // If anything comes in Serial (USB),
      RxData = Serial.read(); 
      LoRa.write(RxData);
    }
    if(LoRa.available()) {
      RxData = LoRa.read();
      Serial.write(RxData);
    }
  }
#endif
}

int checkMessage(int status) {
  //if message confirmed increase/maintain status
  if(status > 30 && status < 40 && LoRaPowerStatus != 3) {
    LoRaPower(3);
  }
  status=50;
  Serial.println("CheckMessage: Status confirmed");
  
  return status;
}

void LoRaPower(int power) {
  Serial.println("LoRaPower: power change");
  pinMode(25, OUTPUT); digitalWrite(25, 1);
  pinMode(26, OUTPUT); digitalWrite(26, 1);
  delay(200);
  if( power == 1){
    LoRaSendStr("AT+POWER=1");
    LoRaPowerStatus = 1;
  } else {
    LoRaSendStr("AT+POWER=3");
    LoRaPowerStatus = 3;
  }
  delay(500);
  LoRaSendStr("AT+RESET");
  //Set LoRa mode
  delay(3000);
  pinMode(25, OUTPUT); digitalWrite(25, 0);
  pinMode(26, OUTPUT); digitalWrite(26, 0);
}



void loop() {
  static char mess[20];
  int messi=0;
  byte RxData;
  static int status=50;
  char txtbuf[40];
  int i;

  //WDT reset
  esp_task_wdt_reset();
  delay(2);

  //1-wire read
  sensors.requestTemperatures(); 
  float temperatureC = sensors.getTempCByIndex(0);
  Serial.print(temperatureC);
  Serial.println("ºC");
  sprintf(txtbuf, "%02.3f T01&", temperatureC);
  Serial.println(txtbuf);
  LoRaSendStr(txtbuf);

  //LoRa delivery
  status--;
  Serial.print("Status: ");
  Serial.print(status);
  Serial.print(" PWR:");
  Serial.println(LoRaPowerStatus);
  for(int i=0; i<5000; i++){
    delay(1);
    if(LoRa.available()) {
      RxData = LoRa.read();
      Serial.write(RxData);
      Serial.write(' ');
      mess[messi++]=RxData;
      if(messi>19){messi=0;}
      if(RxData=='X'){
        status=checkMessage(status);
        }
    }
  }
  if(status==40) {
    LoRaPower(1);
  }

  if(status==1){
    Serial.println("Reboot, reset LoRa");
    status=50;
    delay(20000); //trigger watchdog
  }
}