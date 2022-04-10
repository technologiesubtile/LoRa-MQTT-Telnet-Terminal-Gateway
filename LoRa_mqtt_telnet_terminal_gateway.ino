/*
   LoRa/MQTT/Telnet Terminal/Gateway
heinrich.diesinger@cnrs.fr, F4HYZ, http://heinerd.online.fr/elektronik/
One unique device can be both LoRa to MQTT gateway and LoRa terminal. Fully configurable and with EEPROM storage of settings.

*/
#include <SPI.h>              // include libraries for Lora
#include <LoRa.h>

#include <EEPROM.h>           // eeprom

#include <ESP8266WiFi.h>      // libraries for wifi 
#include <PubSubClient.h>     // and MQTT client

// WiFi stuff:
// from telnetsleepswitch, notice double definitions
char wifi_mode[4] = "sta"; //ap, sta, off
char ssid[20] = "myssid";
char password[20] = "mypasswd";
boolean connectstate = false;


// mode selection

char charlocalmode[6] = "mqtt"; // tns, tnc, mqtt


// miscellaneous control variables
byte context = 0; // whether it is context info (y) or the payload (n)
char charsercontext = '1'; // whether info other than the message itself and command acks are displayed
char chartncontext = '1'; // e.g. "sending", "packet received", voltage info etc.
byte sercontext = 1;
byte tncontext = 1;
char charlorcontext = '1';
byte lorcontext = 1;
bool fromlora = false; //flag to distinguish whether a message arrived remotely via LoRa or locally via serial, telnetclient or -server
char charlastcmdlocal = '1';
byte lastcmdlocal = 1; // flag so that spontaneous msg as setup info and low battery warn function knows which direction to send its warning
bool cmdflag=0; // command acknowledgement, flag set prevents it from going to mqtt interface


// powerlatch and mosfet switching
#define POWERHOLD D3
#define MOSFET D1
byte paylbyte = 0;
char charpayl = '0';


// eeprom stuff
const long nvlength = 512;
char shadoweeprom[nvlength];
char shadowshadow[nvlength]; // security copy unaltered by strtoktoken
char checker[5] = "";
char *strtokIndx;


//mqtt stuff
char chartargetip[16] = "192.168.1.1"; // 192.168.4.1 for the local machine
char shadowchartargetip[16];
byte targetip[4] = {192, 168, 1, 1}; // target ip for both mqtt and telnet clients
char chartargetport[6] = "1883"; //23 for telnet, 1883 for mqtt
long targetport = 1883; // or 7777 ? for telnet only ??? or also mqtt
#define MQTT_USERNAME   ""
#define MQTT_KEY        ""
char mqttintopic[63]  =  "misc";
char mqttouttopic[63] =  "misc";


// LoRa stuff
const int csPin = 15;          // LoRa radio chip select
const int resetPin = 16;       // LoRa radio reset
const int irqPin = 0;         // change for your board; must be a hardware interrupt pin
byte msgCount = 0;            // count of outgoing messages
byte localAddress = 0xBD;     // address of this device
byte destination = 0xBE;      // destination to send to
byte promiscuous = 1;         // whether it treats incoming LoRa messages that are not addressed to the local address
char charloclorad[3] = "bd";  // take conversion from shutter.ino
char chardestlorad[3] = "be";
char charpromisc = '1';
byte upperhalfbyte = 0x10; // for the conversion of human readable hex adress
byte lowerhalfbyte = 0x10;
byte entirerecip = 0x00;
byte entiresender = 0x00; // end coversion
long lastSendTime = 0;        // last send time
long unsigned displaystamp = 0;   // timestamp of a displayed msg
char charfreq[10] = "434000000";
char charpower[5] = "20";
char charspread[5] = "12";
char charcode[5] = "7";
char charbw[10] = "125000";
long freq = 434000000; //eeprom
byte power = 20; //eeprom
byte spread = 12; //eeprom
byte code = 7; //eeprom
long bandwidth = 125000; //eeprom


// battery management
int battlevel;
float voltf;
float warnthreshold = 3.3;
float protectthreshold = 2.9;
unsigned long batmeasperiod = 30000; //check battery every 30 seconds
unsigned long lastmeasured = 0;


// variables for the serial interrupt handler:
const byte numchars = 200;
char message[numchars];  // all input be it LoRa or local
char *messptr = message;
char outmessage[numchars]; // another array to which both, context and payload messages are copied
char *outmessptr = outmessage;
boolean newdata = false;  // whether the reception is complete
int ndx; // this is used both in LoRa and serial reception, seems to work
unsigned long lastchar;
char rc;


// and for data to charray conversion etc
char interimcharray[10]; // and in battery measurement
size_t cur_len;



// declare telnet server (do NOT put in setup())
WiFiServer telnetServer(23);
WiFiClient serverClient;

//declare telnet client
//WiFiClient client; // makes no sense, use the above WiFiClient serverClient declaration instead without the server part

// declare mqtt client
WiFiClient espClient;
PubSubClient client(espClient);


void sendMessage(char *outgoing) {
  LoRa.beginPacket();                   // start packet
  LoRa.write(destination);              // add destination address
  LoRa.write(localAddress);             // add sender address
  LoRa.write(msgCount);                 // add message ID
  LoRa.write(strlen(outgoing));        // add payload length
  LoRa.print(outgoing);                 // add payload
  LoRa.endPacket();                     // finish packet and send it
  msgCount++;                           // increment message ID
}


void publish() {
  // output routine
 
  if (fromlora) { // received from remotely, so publish on local side:

      // send by mqtt
      if ((strstr(charlocalmode, "mqtt")) && client.connected()) { // if not, no need to check the rest...
      if (!(context || cmdflag)) { // mqtt never contains system messages, nor command acks
      client.publish(mqttouttopic, outmessage);
      // Serial.println("publishing on mqtt");
      // Serial.println(outmessage);
       }
      }
    
    // send by serial
    if (!(context && !sercontext)) // system messages only displayed if sercontext is set
    {
      Serial.println(outmessage);
    }

    // send by telnet server
    if  (strstr(charlocalmode, "tns") && serverClient.connected()) {
            if (!(context && !tncontext)) // context only if tncontext is set
        serverClient.println(outmessage);
    }
      
     // send by telnet client
     if  (strstr(charlocalmode, "tnc") && serverClient.connected()) {
             if (!(context || cmdflag)) // telnet destination never contains system messages, nor command acks
       serverClient.println(outmessage);
    }

  } // end publish locally


  else { // if not fromlora, publish remotely

    if (!(context && !lorcontext))
      sendMessage(outmessage); //sending by LoRa
  }

  // newdata= false; // not used here , -> in main loop

} //end publish()



void setuplora() {
  // LoRa setup
  // override the default CS, reset, and IRQ pins (optional)
  LoRa.setPins(csPin, resetPin, irqPin);// set CS, reset, IRQ pin
  if (!LoRa.begin(freq)) {             // initialize radio at 434 MHz; eeprom: freq instead of hardcoded 434e6
    strcpy(outmessage, "LoRa init failed. Check your connections.");
    while (true);                       // if failed, do nothing
  }
  else  {
    strcpy(outmessage, "LoRa started and settings successfully applied.");

    LoRa.setTxPower(power); // eeprom, was 20
    LoRa.setSpreadingFactor(spread); // eeprom, was 12
    LoRa.setCodingRate4(code); // eeprom, was 7
    LoRa.setSignalBandwidth(bandwidth); // eeprom, was 125E3
  }
}



void helpscreen() {
  if (!fromlora) {
    strcpy(outmessage, "Error - call help screen only locally");
     } else {
  strcpy(outmessage, ""); publish();
  strcpy(outmessage, "LoRa to serial/telnet/mqtt gateway with user configuration and EEPROM storage"); publish();
  strcpy(outmessage, "By heinrich.diesinger@cnrs.fr, F4HYZ"); publish();
  strcpy(outmessage, "to enter a command, it must be preceded by AT+, e.g. AT+targetip 192.168.1.101"); publish();
  strcpy(outmessage, "to send a command as msg w/o executing, use verbatim prefix: \\verb AT+batlevel"); publish();
  strcpy(outmessage, "to relay msg or cmd the same side of gateway, use relay prefix: \\relay text"); publish();
  strcpy(outmessage, "anything else is considered a message and cross transferred local/LoRa"); publish();
  strcpy(outmessage, "command acknowledgements bounce to the side (local/LoRa) from where cmd issued"); publish();
  strcpy(outmessage, "system msgs, warnings go to the side (local/LoRa) from where last cmd issued"); publish();
  strcpy(outmessage, "WiFi related settings: "); publish();
  strcpy(outmessage, "setupwifi reinitializes wifi to apply changed parameters; serial ack only"); publish();
  strcpy(outmessage, "wifimode ap/sta : access point or wifi client <sta>"); publish();
  strcpy(outmessage, "ssid: for either wifi mode"); publish();
   delay(500);  // not saturate the serial buffer
  strcpy(outmessage, "password: for either wifi mode"); publish();
  strcpy(outmessage, "Telnet related settings: "); publish();
  strcpy(outmessage, "setuptelnet reinitializes telnet to apply changed parameters"); publish();
  strcpy(outmessage, "targetip: ip for either telnet or mqtt <192.168.4.1>"); publish();
  strcpy(outmessage, "targetport: port for either telnet or mqtt <1883>, must be 23 for mqtt"); publish();
  strcpy(outmessage, "MQTT related settings: "); publish();
  strcpy(outmessage, "intopic: topic to subscribe to <misc>"); publish();
  strcpy(outmessage, "outtopic: topic to publish to <misc>"); publish();
  strcpy(outmessage, "mqttconnect: initialize MQTT connection"); publish();
  strcpy(outmessage, "System settings: "); publish();
  delay(1000); // not saturate the serial buffer
  strcpy(outmessage, "help: displays this info again"); publish();
  strcpy(outmessage, "localmode tns/tnc/mqtt: telnet server/client, mqtt client <mqtt>"); publish();
  strcpy(outmessage, "batlevel: shows the supply voltage"); publish();
  strcpy(outmessage, "payload 1/0: switches the open drain power output"); publish();
  strcpy(outmessage, "eepromstore: stores the settings to EEPROM (must be called manually)"); publish();  // eeprom
  strcpy(outmessage, "eepromdelete: clears EEPROM"); publish(); // eeprom
  strcpy(outmessage, "eepromretrieve: reads settings from EEPROM"); publish(); // eeprom
  strcpy(outmessage, "reboot: restarts the firmware"); publish();
  strcpy(outmessage, "shutdown: suspends processor and LoRa power"); publish();
  strcpy(outmessage, "System messages: startup msgs, battery warning, message received "); publish();
  strcpy(outmessage, "tncontext 1/0: enables system messages on telnet terminal <1>"); publish();
  strcpy(outmessage, "sercontext 1/0: enables system messages on serial terminal <1>"); publish();
  delay(1000); // not saturate the serial buffer
  strcpy(outmessage, "lorcontext 1/0: enables system messages on LoRa terminal <1>"); publish();
  strcpy(outmessage, "LoRa related settings: "); publish();
  strcpy(outmessage, "linkstrength: returns RSSI and SNR"); publish();
  strcpy(outmessage, "setuplora applies the radio settings"); publish();
  strcpy(outmessage, "loclora sets the local device address byte (hex) <0xbd>"); publish();
  strcpy(outmessage, "destlora sets the destination device address byte (hex) <0xbe>"); publish();
  strcpy(outmessage, "promiscuous 1/0 sets LoRa promiscuous mode <1> "); publish();
  strcpy(outmessage, "freq <434000000>: sets the LoRa frequency"); publish(); // eeprom 434000000
  strcpy(outmessage, "power <20>: sets LoRa power in dBm"); publish(); // eeprom 20
  strcpy(outmessage, "spread <12>: sets the LoRa spreading factor"); publish(); // eeprom 12
  strcpy(outmessage, "code <7>: sets the LoRa coding"); publish(); // eeprom 7
  strcpy(outmessage, "bandwidth <125000>: sets the LoRa bandwidth");  // eeprom 125000
  }
}



void bounce() { // changes the output apparent origin and direction of a message depending on whether it is a command
                // in order to redirect the command acknowledgement to the sender
  if (fromlora) fromlora = 0; else fromlora = 1;
}


void eepromstore() //concatenates all setup data in a csv to the shadoweeprom, writes 1 by 1 into eeprom, and commit()
{
  strcpy(shadoweeprom, "99,");
  strcat(shadoweeprom, ssid);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, password);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, wifi_mode);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, charlocalmode);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, mqttintopic);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, mqttouttopic);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, chartargetip);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, chartargetport);
  strcat(shadoweeprom, ",");
  // now the single char byte-booleans:
  cur_len = strlen(shadoweeprom);
  shadoweeprom[cur_len] = charsercontext;
  shadoweeprom[cur_len + 1] = ',';
  shadoweeprom[cur_len + 2] = '\0';
  //cur_len = strlen(shadoweeprom);
  cur_len += 2;
  shadoweeprom[cur_len] = chartncontext;
  shadoweeprom[cur_len + 1] = ',';
  shadoweeprom[cur_len + 2] = '\0';
  cur_len += 2;
  shadoweeprom[cur_len] = charlorcontext;
  shadoweeprom[cur_len + 1] = ',';
  shadoweeprom[cur_len + 2] = '\0';
  cur_len += 2;
  shadoweeprom[cur_len] = charlastcmdlocal;
  shadoweeprom[cur_len + 1] = ',';
  shadoweeprom[cur_len + 2] = '\0';
  strcat(shadoweeprom, charloclorad);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, chardestlorad);
  strcat(shadoweeprom, ",");
  cur_len = strlen(shadoweeprom);
  shadoweeprom[cur_len] = charpromisc;
  shadoweeprom[cur_len + 1] = ',';
  shadoweeprom[cur_len + 2] = '\0';
  strcat(shadoweeprom, charfreq);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, charpower);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, charspread);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, charcode);
  strcat(shadoweeprom, ",");
  strcat(shadoweeprom, charbw);
 
  strcpy(outmessage, "shadoweeprom is: "); strcat(outmessage, shadoweeprom);
  Serial.println(outmessage); // this goes to the serial console only, too much for lora

  for (int i = 0; i < nvlength; ++i)
  {
    EEPROM.write(i, shadoweeprom[i]);
    if (shadoweeprom[i] = '\0') break; // stop at the termination
  }

  if (EEPROM.commit()) strcpy(outmessage, "EEPROM successfully committed");
    
  else strcpy(outmessage, "writing to EEPROM failed");
    

}



byte parse_char(char c) //needed by addresscheck functiom
// convert a hex symbol to byte
{ if ( c >= '0' && c <= '9' ) return ( c - '0' );
  if ( c >= 'a' && c <= 'f' ) return ( c - 'a' + 10 );
  if ( c >= 'A' && c <= 'F' ) return ( c - 'A' + 10 );
  // if nothing,
  return 16;
  // or alternatively
  //  abort()
}


void addresscheck() {
  // convert the adress char array into a byte and compare with the local device adreess
  if (strlen(charloclorad) == 2)
  { // convert the two hex characters into integers from 0 to 15, 16 if not a hex symbol
    upperhalfbyte = parse_char(charloclorad[0]);
    lowerhalfbyte = parse_char(charloclorad[1]);
    if ((upperhalfbyte == 16) || (lowerhalfbyte == 16))
    {
      strcpy(outmessage, "malformatted local address - either char is not a hex symbol");
      publish();
    }
    else
    { localAddress = upperhalfbyte * 0x10 + lowerhalfbyte;
    }
  }
  else
  {
    strcpy(outmessage, "malformed local address - length != 2");
    publish();
  }

  if (strlen(chardestlorad) == 2)
  { // convert the two hex characters into integers from 0 to 15, 16 if not a hex symbol
    upperhalfbyte = parse_char(chardestlorad[0]);
    lowerhalfbyte = parse_char(chardestlorad[1]);
    if ((upperhalfbyte == 16) || (lowerhalfbyte == 16))
    {
      strcpy(outmessage, "malformatted destination address - either char is not a hex symbol");
      publish();
    }
    else
    { destination = upperhalfbyte * 0x10 + lowerhalfbyte;
    }
  }
  else
  {
    strcpy(outmessage, "malformed destination address - length != 2");
    publish();
  }
}




void eepromretrieve() // reads all eeprom into shadoweeprom 1 by 1, split into token by strtok to charfreq etc, converts by atoi
{
  // write eeprom into shadow eeprom buffer
  char eeprchar;
  for (int i = 0; i < nvlength; i++) {
    eeprchar = char(EEPROM.read(i));
    shadoweeprom[i] = eeprchar;
    if (eeprchar = '\0') break;
  }

  // since strtok destroys the charray, make a copy of it
  strcpy(shadowshadow, shadoweeprom);

  // decompose it into the char versions of the parameters
  strtokIndx = strtok(shadoweeprom, ",");
  if (strtokIndx != NULL) strncpy(checker, strtokIndx, 2); // add checking mechanism if eeprom has ever been written,
  // and if not, return from the fct to use hardcoded defaults
  else strcpy(checker, "");

  if (strstr(checker, "99")) {  // only if "checksum" is ok, continue decomposing
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(ssid, strtokIndx);
    else strcpy(ssid, ""); // this to clear the argument from the last message, if there is not a new one
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(password, strtokIndx);
    else strcpy(password, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(wifi_mode, strtokIndx);
    else strcpy(wifi_mode, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charlocalmode, strtokIndx);
    else strcpy(charlocalmode, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(mqttintopic, strtokIndx);
    else strcpy(mqttintopic, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(mqttouttopic, strtokIndx);
    else strcpy(mqttouttopic, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(chartargetip, strtokIndx);
    else strcpy(chartargetip, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(chartargetport, strtokIndx);
    else strcpy(chartargetport, "");
    strtokIndx = strtok(NULL, ",");
    //if (strtokIndx != NULL) strcpy(charsercontext, strtokIndx); // doesnt work for a single character
    //else strcpy(charsercontext, "");
    charsercontext = strtokIndx[0]; // ! single charcter
    strtokIndx = strtok(NULL, ",");
    //if (strtokIndx != NULL) strcpy(chartncontext, strtokIndx);
    //else strcpy(chartncontext, "");
    chartncontext = strtokIndx[0]; // ! single character
    strtokIndx = strtok(NULL, ",");
    //if (strtokIndx != NULL) strcpy(charlorcontext, strtokIndx);
    //else strcpy(charlorcontext, "");
    charlorcontext = strtokIndx[0]; // ! single charcter
    strtokIndx = strtok(NULL, ",");
    //if (strtokIndx != NULL) strcpy(charlastcmdlocal, strtokIndx); // doesnt work if charlastcmdlocal is a char rather than a char*
    //else strcpy(charlastcmdlocal, "");
    charlastcmdlocal = strtokIndx[0]; // ! single character
    // we could have combined the two lines and done
    // charlastcmdlocal = strtok(NULL, ",")[0];
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charloclorad, strtokIndx);
    else strcpy(charloclorad, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(chardestlorad, strtokIndx);
    else strcpy(chardestlorad, "");
    strtokIndx = strtok(NULL, ",");
    charpromisc = strtokIndx[0]; // ! single charcter
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charfreq, strtokIndx);
    else strcpy(charfreq, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charpower, strtokIndx);
    else strcpy(charpower, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charspread, strtokIndx);
    else strcpy(charspread, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charcode, strtokIndx);
    else strcpy(charcode, "");
    strtokIndx = strtok(NULL, ",");
    if (strtokIndx != NULL) strcpy(charbw, strtokIndx);
    else strcpy(charbw, "");

    // and further convert it into non-char format with atoi() functions and the like:

    // chartargetip: further decompose into byte array; the separator is now a dot !
    strcpy(shadowchartargetip, chartargetip); // the original char array will be deteriorated by the strtok function
    strtokIndx = strtok(shadowchartargetip, ".");
    for (int i = 0; i <= 3; i++) {
      if (strtokIndx != NULL) {
        targetip[i] = atoi(strtokIndx);
      }
      //if (strtokIndx != NULL) strcpy(targetip[i], atoi(strtokIndx)); // add checking mechanism if eeprom has ever been written,
      // and if not, return from the fct to use hardcoded defaults
      //else strcpy(targetip[i], ""); //doesnt work, empty is not a byte
      strtokIndx = strtok(NULL, ".");
    }

    // port number (easy)
    targetport = atoi(chartargetport);

    // byte-booleans
    sercontext = charsercontext - '0'; // single characters cannot be converted by atoi() !
    tncontext = chartncontext - '0';
    lorcontext = charlorcontext - '0';
    lastcmdlocal = charlastcmdlocal - '0';
    promiscuous = charpromisc - '0';

    // hex conversion:
    // charloclorad
    // chardestlorad
    addresscheck();

    // Lora stuff
    freq = atoi(charfreq);
    power = atoi(charpower);
    spread = atoi(charspread);
    code = atoi(charcode);
    bandwidth = atoi(charbw);

    // reset the "checksum"
    strcpy(checker, ""); //resetting the checking mechanism

    // now that lastcommand is known, publish some stuff
    fromlora = lastcmdlocal;
    strcpy(outmessage, "EEPROM content: \r\n");
    strcat(outmessage, shadowshadow);
  }
  else // if checker not == 99
    strcpy(outmessage, "EEPROM empty, using factory defaults");
}  // end eepromretrieve



void eepromdelete()
{
  for (int i = 0; i < nvlength; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  delay(500);
  strcpy(outmessage, "EEPROM successfully deleted");
}



void reboot()
{
  ESP.restart();
  // ESP.reset(); // less than restart ! can leave some of the registers in the old state
}



void setfreq()
{
  if (strstr(outmessage, "freq ")) {
    strtokIndx = outmessptr + 5;
    if (strlen(strtokIndx) <= 10) {
      strcpy(charfreq, strtokIndx);
      freq = atoi(charfreq);
      //lorastart();
      //eepromstore();
      strcpy(outmessage, "LoRa freq set to: ");
      strcat(outmessage, charfreq);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charfreq);
  }
}



void setpower()
{
  if (strstr(outmessage, "power ")) {
    strtokIndx = outmessptr + 6;
    if (strlen(strtokIndx) <= 2) {
      strcpy(charpower, strtokIndx);
      power = atoi(charpower);
      //LoRa.setTxPower(power);
      //eepromstore();
      strcpy(outmessage, "LoRa power set to: ");
      strcat(outmessage, charpower);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charpower);
  }
}



void setspread()
{
  if (strstr(outmessage, "spread ")) {
    strtokIndx = outmessptr + 7;
    if (strlen(strtokIndx) <= 2) {
      strcpy(charspread, strtokIndx);
      spread = atoi(charspread);
      //LoRa.setSpreadingFactor(spread);
      //eepromstore();
      strcpy(outmessage, "LoRa spreading factor set to: ");
      strcat(outmessage, charspread);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charspread);
  }
}



void setcode()
{
  if (strstr(outmessage, "code ")) {
    strtokIndx = outmessptr + 5;
    if (strlen(strtokIndx) <= 1) {
      strcpy(charcode, strtokIndx);
      code = atoi(charcode);
      //LoRa.setCodingRate4(code);
      //eepromstore();
      strcpy(outmessage, "LoRa coding rate set to: ");
      strcat(outmessage, charcode);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charcode);
  }
}



void setbandwidth()
{
  if (strstr(outmessage, "bandwidth ")) {
    strtokIndx = outmessptr + 10;
    if (strlen(strtokIndx) <= 10) {
      strcpy(charbw, strtokIndx);
      // Serial.print("charbw = "); Serial.println(charbw); //test !!!
      bandwidth = atoi(charbw);
      strcpy(outmessage, "LoRa bandwidth set to: ");
      strcat(outmessage, charbw);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charbw);
  }
}


void loclora() {
  if (strstr(outmessage, "loclora ")) {
    strtokIndx = outmessptr + 8;
    if (strlen(strtokIndx) <= 10) {
      strcpy(charloclorad, strtokIndx);
      // Serial.print("charbw = "); Serial.println(charbw); //test !!!
      addresscheck();
      strcpy(outmessage, "LoRa local address set to: ");
      strcat(outmessage, charloclorad);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charloclorad);
  }
}


void destlora() {
  if (strstr(outmessage, "destlora ")) {
    strtokIndx = outmessptr + 9;
    if (strlen(strtokIndx) <= 10) {
      strcpy(chardestlorad, strtokIndx);
      // Serial.print("charbw = "); Serial.println(charbw); //test !!!
      addresscheck();
      strcpy(outmessage, "LoRa destination address set to: ");
      strcat(outmessage, chardestlorad);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, chardestlorad);
  }
}



void setwifimode() {
    if (strstr(outmessage, "wifimode ")) {
    strtokIndx = outmessptr + 9;
    if (strlen(strtokIndx) <= 10) {
      strcpy(wifi_mode, strtokIndx);
      strcpy(outmessage, "wifi mode set to: ");
      strcat(outmessage, wifi_mode);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, wifi_mode);
  }
}

void setssid() {
  if (strstr(outmessage, "ssid ")) {
    strtokIndx = outmessptr + 5;
    if (strlen(strtokIndx) <= 20) {
      strcpy(ssid, strtokIndx);
      strcpy(outmessage, "ssid set to: ");
      strcat(outmessage, ssid);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, ssid);
  }
}


void setpassword() {
  if (strstr(outmessage, "password ")) {
    strtokIndx = outmessptr + 9;
    if (strlen(strtokIndx) <= 20) {
      strcpy(password, strtokIndx);
      strcpy(outmessage, "password set to: ");
      strcat(outmessage, password);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, password);
  }
}


void setlocalmode() {
  if (strstr(outmessage, "localmode ")) {
    strtokIndx = outmessptr + 10;
    if (strlen(strtokIndx) <= 5) {
      strcpy(charlocalmode, strtokIndx);
      strcpy(outmessage, "local mode set to: ");
      strcat(outmessage, charlocalmode);
      // localmode from charversion:
      // if (strstr(charlocalmode, "tns")) tnservermode = 1; else tnservermode = 0;

    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, charlocalmode);
  }
}



void setintopic() {
  if (strstr(outmessage, "intopic ")) {
    strtokIndx = outmessptr + 8;
    if (strlen(strtokIndx) <= 63) {
      strcpy(mqttintopic, strtokIndx);
      strcpy(outmessage, "MQTT intopic set to: ");
      strcat(outmessage, mqttintopic);
     } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, mqttintopic);
  }
}


void setouttopic() {
  if (strstr(outmessage, "outtopic ")) {
    strtokIndx = outmessptr + 9;
    if (strlen(strtokIndx) <= 63) {
      strcpy(mqttouttopic, strtokIndx);
      strcpy(outmessage, "MQTT outtopic set to: ");
      strcat(outmessage, mqttouttopic);
     } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, mqttouttopic);
  }
}



void settargetip() {
  if (strstr(outmessage, "targetip ")) {
    strtokIndx = outmessptr + 9;
    if (strlen(strtokIndx) <= 16) {
      strcpy(chartargetip, strtokIndx);
      strcpy(outmessage, "target ip set to: ");
      strcat(outmessage, chartargetip);
      
    // targetip from char version: further decompose into byte array; the separator is now a dot !
    strcpy(shadowchartargetip, chartargetip); // it will be destroyed
    strtokIndx = strtok(shadowchartargetip, ".");
    for (int i = 0; i <= 3; i++) {
      if (strtokIndx != NULL) {
        targetip[i] = atoi(strtokIndx);
      }
      strtokIndx = strtok(NULL, ".");
    }
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, chartargetip);
  }
}



void settargetport() {
if (strstr(outmessage, "targetport ")) {
    strtokIndx = outmessptr + 11;
    if (strlen(strtokIndx) <= 6) {
      strcpy(chartargetport, strtokIndx);
      strcpy(outmessage, "target port set to: ");
      strcat(outmessage, chartargetport);
      // port number from char version (easy)
      targetport = atoi(chartargetport);
    } else {
      strcpy(outmessage, "error: argument too long");
    }
  } else {
    strcpy(outmessage, chartargetport);
  }
}



void setpromisc() {
  if (strstr(outmessage, "promiscuous ")) {
    strtokIndx = outmessptr + 12;
    if (strlen(strtokIndx) == 1) {
      charpromisc = strtokIndx[0];
      strcpy(outmessage, "LoRa promiscuous mode set to: ");
      cur_len = strlen(outmessage);
      outmessage[cur_len] = charpromisc;
      outmessage[cur_len + 1] = '\0';
      promiscuous = charpromisc - '0';
    } else {
      strcpy(outmessage, "error: argument too short or long");
    }
  } else {
    strcpy(outmessage, "promiscuous state is ");
    cur_len = strlen(outmessage);
    outmessage[cur_len] = charpromisc;
    outmessage[cur_len + 1] = '\0';
  }
}



void setpayload() {
  if (strstr(outmessage, "payload ")) {
      strtokIndx = outmessptr + 8;
      if (strlen(strtokIndx) == 1) {
      charpayl = strtokIndx[0];
      paylbyte = strtokIndx[0] - '0';
      digitalWrite(MOSFET, paylbyte);
      strcpy(outmessage, "payload switched to: ");
      cur_len = strlen(outmessage);
      outmessage[cur_len] = charpayl;
      outmessage[cur_len + 1] = '\0';
      } else {
      strcpy(outmessage, "error: argument too short or long");
    }
    } else {
    strcpy(outmessage, "payload state is ");
    cur_len = strlen(outmessage);
    outmessage[cur_len] = charpayl;
    outmessage[cur_len + 1] = '\0';
    }
}



void settncontext() {
   if (strstr(outmessage, "tncontext ")) {
    strtokIndx = outmessptr + 10;
    if (strlen(strtokIndx) == 1) {
      //strcpy(chardestlorad, strtokIndx);
      chartncontext = strtokIndx[0];
      strcpy(outmessage, "telnet context enable set to : ");
      cur_len = strlen(outmessage);
      outmessage[cur_len] = chartncontext;
      outmessage[cur_len + 1] = '\0';
      tncontext = chartncontext - '0';
    } else {
      strcpy(outmessage, "error: argument too short or long");
    }
  } else {
    strcpy(outmessage, "telnet context enable is ");
    cur_len = strlen(outmessage);
    outmessage[cur_len] = chartncontext;
    outmessage[cur_len + 1] = '\0';
  }
}



void setsercontext() {
  if (strstr(outmessage, "sercontext ")) {
    strtokIndx = outmessptr + 11;
    if (strlen(strtokIndx) == 1) {
      //strcpy(chardestlorad, strtokIndx);
      charsercontext = strtokIndx[0];
      strcpy(outmessage, "serial context enable set to : ");
      cur_len = strlen(outmessage);
      outmessage[cur_len] = charsercontext;
      outmessage[cur_len + 1] = '\0';
      sercontext = charsercontext - '0';
    } else {
      strcpy(outmessage, "error: argument too short or long");
    }
  } else {
    strcpy(outmessage, "serial context enable is ");
    cur_len = strlen(outmessage);
    outmessage[cur_len] = charsercontext;
    outmessage[cur_len + 1] = '\0';
  }
}



void setlorcontext() {
  if (strstr(outmessage, "lorcontext ")) {
    strtokIndx = outmessptr + 11;
    if (strlen(strtokIndx) == 1) {
      //strcpy(chardestlorad, strtokIndx);
      charlorcontext = strtokIndx[0];
      strcpy(outmessage, "LoRa context enable set to : ");
      cur_len = strlen(outmessage);
      outmessage[cur_len] = charlorcontext;
      outmessage[cur_len + 1] = '\0';
      lorcontext = charlorcontext - '0';
    } else {
      strcpy(outmessage, "error: argument too short or long");
    }
  } else {
    strcpy(outmessage, "LoRa context enable is ");
    cur_len = strlen(outmessage);
    outmessage[cur_len] = charlorcontext;
    outmessage[cur_len + 1] = '\0';
  }
}




void linkstrength()
{
  strcpy(outmessage, "RSSI: ");
  dtostrf(LoRa.packetRssi(), 0, 1, interimcharray);
  strcat(outmessage, interimcharray);
  strcat(outmessage, ", SNR: ");
  dtostrf(LoRa.packetSnr(), 0, 1, interimcharray);
  strcat(outmessage, interimcharray);
}



void batmeasure()
{
  battlevel = analogRead(A0);
  voltf = battlevel * (5.4 / 1024.0); //220k + 220k + 100k
}


void batlevel()
{
  batmeasure();
  dtostrf(voltf, 0, 2, interimcharray);
  strcpy(outmessage, "The voltage is ");
  strcat(outmessage, interimcharray);
  strcat(outmessage, " V");
}



void batperiodically()
{
  batmeasure();
  if (voltf < warnthreshold)
  {
    // issue a battery warning
    dtostrf(voltf, 0, 2, interimcharray);
    strcpy(outmessage, "Battery critically low, ");
    strcat(outmessage, interimcharray);
    strcat(outmessage, " V");
    if (voltf < protectthreshold)
    {
      strcat(outmessage, ", suspending system power");
      context = 1; publish(); context = 0; // put here because there is no after function
      delay(1000); // wait a while that the LoRa message goes out
      digitalWrite(POWERHOLD, LOW);
    }
  context = 1; publish(); context = 0; // repeat publishing here, because the previous one turns off the mcu
                                       // this function is not called by the command parser that ystematically publishes the outmessage
  }
}


void suspend()
{
  strcpy(outmessage, "Suspending system power");
  publish(); // put here because there is no "after function"
  delay(1000); // wait a while that the LoRa message goes out
  digitalWrite(POWERHOLD, LOW);
}



boolean setup_wifi() {
  delay(100);
  // here we use Serial.print() always instead of publish() because during wifi setup, neither LoRa nor telnet nor mqtt is initialized !!
  Serial.print("Connecting, SSID "); Serial.print(ssid); Serial.print(" as "); Serial.println(wifi_mode);
  // here wifi_mode is directly a char array and not the strange type in the original declaration

  if (strstr(wifi_mode, "ap"))
  {
    // AP mode
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    Serial.println("IP address: 192.168.4.1");
    return true;
  }
  else if (strstr(wifi_mode, "sta")) // newly added because here we also have WIFI_OFF !!!
  {
    // network cient - inital connect
    WiFi.mode(WIFI_STA);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);  // to keep the WiFi connection alive always, needed for telnet client
    WiFi.begin(ssid, password);
    unsigned long prevtime = millis();  // comment this to force wifi, preventing prog to go in stealth
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
      Serial.print(".");
      unsigned long currentime = millis();  // comment the 6 lines to force wifi connection, preventing prog to go in stealth
      if (currentime - prevtime >= 30000)
      {
        Serial.println("WiFi unavailable");
        return false;
      }
    }
    randomSeed(micros());

    Serial.print("IP address: "); Serial.println(WiFi.localIP()); // hopefully this works for both AP and STA; no ! in AP it
    // is always 192.168.4.1
    return true;
  }
  else
  {
    // switch off radio to go into the stealth mode
    // in mqtt LoRa gateway it switched off WiFi if no connection possible to suppress the default STA-AP mode
    Serial.println("Going in stealth mode until reset");
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1); //Needed, at least in my tests WiFi doesn't power off without this for some reason
  }
}


void setupwifi() {
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
  connectstate = setup_wifi();
  // wifi_station_connect(); // needed after deepsleep
  strcpy(outmessage, "done");
}



void establish_telnet() {  // if not as server, then connect the client to target ip
  // here we use Serial.print() always instead publish() because during wifi setup, neither LoRa nor telnet or mqtt is initialized !!
  if (!serverClient.connected()) {
    if (serverClient.connect(targetip, targetport)) { //serverClient put instead of client
    delay(500);
    strcpy(outmessage, "telnet connected to ip ");
    strcat(outmessage, chartargetip);
    strcat(outmessage, " on port ");
    strcat(outmessage, chartargetport);
    //nothing, end 
    } else {
      strcpy(outmessage, "failed to connect telnet client, try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}


void setuptelnet() {  // telnet server
  serverClient.stop();
   // cannot do harm because it is called only on startup or manually to refresh settings
   telnetServer.stop();
   telnetServer.begin();
    telnetServer.setNoDelay(true);
    strcpy(outmessage, "listening to telnet connections on port 23");
}



void callback(char* topic, byte* payload, unsigned int length)
{
 
  payload[length] = '\0';
  for (int i = 0; i <= length; i++)
  message[i] = (char)payload[i];
  newdata = true;
  // we want it to show on the serial terminal although it is from mqtt destined to LoRa, if serial is accepting context
 fromlora = true;
 context = 1;
 strcpy(outmessage, "MQTT message received: "); publish();
 strcpy(outmessage, message); publish();
 fromlora = false;
 context =0;
 } //end callback



  void establish_mqtt() {
  //here use publish() because during mqtt setup, telnet is already initialized !!
  client.setServer(targetip, targetport);
  yield();
  if (!client.connected()) {
    strcpy(outmessage, "Attempting MQTT connection...");
    context=1;
     // Attempt to connect
    if (client.connect("", MQTT_USERNAME, MQTT_KEY))
    {
    strcat(outmessage, "connected");
    client.subscribe(mqttintopic);
    }

    else {
      strcat(outmessage, "failed, rc=");
      strcat(outmessage, (char*)client.state());
      strcat(outmessage, ", try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
  client.setCallback(callback);
  }




void cmdparse() {
  
  if (strstr(outmessage, "help") == outmessptr) {
    helpscreen();
  }
  else if (strstr(outmessage, "linkstrength") == outmessptr) {
    linkstrength();
  }
  else if (strstr(outmessage, "batlevel") == outmessptr) {
    batlevel();
  }
  else if (strstr(outmessage, "shutdown") == outmessptr) {
    suspend();
  }
  else if (strstr(outmessage, "payload") == outmessptr) {
    setpayload();
  }
  else if (strstr(outmessage, "tncontext") == outmessptr) {
    settncontext();
  }
  else if (strstr(outmessage, "sercontext") == outmessptr) {
    setsercontext();
  }
  else if (strstr(outmessage, "lorcontext") == outmessptr) {
    setlorcontext();
  }
  else if (strstr(outmessage, "loclora") == outmessptr) {
    loclora();
  }
  else if (strstr(outmessage, "destlora") == outmessptr) {
    destlora();
  }
  else if (strstr(outmessage, "promiscuous") == outmessptr) {
    setpromisc();
  }
  else if (strstr(outmessage, "freq") == outmessptr) {
    setfreq();
  }
  else if (strstr(outmessage, "power") == outmessptr) {
    setpower();
  }
  else if (strstr(outmessage, "spread") == outmessptr) {
    setspread();
  }
  else if (strstr(outmessage, "code") == outmessptr) {
    setcode();
  }
  else if (strstr(outmessage, "bandwidth") == outmessptr) {
    setbandwidth();
  }
  else if (strstr(outmessage, "eepromretrieve") == outmessptr) {
    eepromretrieve();
  }
  else if (strstr(outmessage, "eepromdelete") == outmessptr) {
    eepromdelete();
  }
  else if (strstr(outmessage, "eepromstore") == outmessptr) {
    eepromstore();
  }
  else if (strstr(outmessage, "reboot") == outmessptr) {
    reboot();
  }
  else if (strstr(outmessage, "setuplora") == outmessptr) {
    setuplora();
  }
  else if (strstr(outmessage, "wifimode") == outmessptr) {
    setwifimode();
  }
  else if (strstr(outmessage, "ssid") == outmessptr) {
    setssid();
  }
  else if (strstr(outmessage, "password") == outmessptr) {
    setpassword();
  }
  else if (strstr(outmessage, "setupwifi") == outmessptr) {
    setupwifi();
  }
  else if (strstr(outmessage, "setuptelnet") == outmessptr) {
    setuptelnet();
  }
  else if (strstr(outmessage, "localmode") == outmessptr) {
    setlocalmode();
  }
   else if (strstr(outmessage, "intopic") == outmessptr) {
    setintopic();
  }
   else if (strstr(outmessage, "outtopic") == outmessptr) {
    setouttopic();
  }
    else if (strstr(outmessage, "mqttconnect") == outmessptr) {
    establish_mqtt();
  }
  else if (strstr(outmessage, "targetip") == outmessptr) {
    settargetip();
  }
  else if (strstr(outmessage, "targetport") == outmessptr) {
    settargetport();
  }
  }  // end cmdparse()




void onReceive(int packetSize)
// contrary to LoRa Duplex, we read into a charray rather than into a string to prepare for pubsub
{
  if (packetSize == 0) return;          // if there's no packet, return

  // read packet header bytes:
  byte recipient = LoRa.read();          // recipient address
  byte sender = LoRa.read();            // sender address
  byte incomingMsgId = LoRa.read();     // incoming msg ID
  byte incomingLength = LoRa.read();    // incoming msg length

  //String incoming = "";
  static byte ndx = 0;

  while (LoRa.available()) {
    char inChar = LoRa.read();
    message[ndx] = inChar;
    ndx++;
    if (ndx >= numchars) {
      ndx = numchars - 1;
    }
}

  message[ndx] = '\0'; // terminate the charray
  newdata = true;
  fromlora = 1;
  context = 1;

    if (incomingLength != ndx) {   // check length for error
   
    strcpy(outmessage, "error: message length does not match length"); publish();
    context = 0;
    return;                             // skip rest of function
  }
  ndx = 0;

   // if the recipient isn't this device or broadcast,
  if (promiscuous == false) {
  if (recipient != localAddress && recipient != 0xFF) {
  //if (recipient != localAddress) {
  strcpy(outmessage, "this message is not for me"); publish();
  return;                             // skip rest of function
  }
  }
 
  

 // adapted to publish() instead println
  //dtostrf(sender, 0,0, interimcharray);
  strcpy(outmessage, "Message received from: 0x"); //strcat(outmessage, interimcharray); publish();
  char* buf2 = outmessage + 25; buf2 += sprintf(buf2, "%02X", sender); publish();
  strcpy(outmessage, "Message received for: 0x"); //strcat(outmessage, interimcharray); publish();
  buf2 = outmessage + 24; buf2 += sprintf(buf2, "%02X", recipient); publish();
  dtostrf(LoRa.packetRssi(), 0, 0, interimcharray); //Snr is a float, (char*) doesnt work on it
  strcpy(outmessage, "RSSI: "); strcat(outmessage, interimcharray); publish();
  dtostrf(LoRa.packetSnr(), 0, 2, interimcharray); //Snr is a float, (char*) doesnt work on it
  strcpy(outmessage, "SNR: "); strcat(outmessage, interimcharray); publish();
  strcpy(outmessage, "Message: "); publish();
  
  context = 0;

}// end onreceive





void setup() {

pinMode(POWERHOLD, OUTPUT); // configure and set the POWERHOLD output
digitalWrite(POWERHOLD, HIGH);

pinMode(MOSFET, OUTPUT); //configure and set the MOSFET output
digitalWrite(MOSFET, LOW);


EEPROM.begin(nvlength);  // eeprom

Serial.begin(9600);                   // initialize serial

context = 1; //specify that the following are context messages
fromlora = 1; // set direction toward local interface

// retrieve EEPROM data
eepromretrieve();
publish();

connectstate = setup_wifi();           // attempt connection to wifi:
// insert force no sleep from the telnet client example:
//while (!Serial);
delay(1000);



  if (strstr(charlocalmode, "tns")) {
    setuptelnet(); publish();
  } else if (strstr(charlocalmode, "tnc")) {
    establish_telnet(); publish();
  } else if (strstr(charlocalmode, "mqtt")) {
     establish_mqtt(); publish();
  }

  setuplora(); publish();

  strcpy(outmessage, "LoRa MQTT Terminal Gateway setup complete"); publish();
  
  helpscreen(); publish();
  Serial.println();
  delay(1000);
  context = 0;

} // end setup





void loop() {

// take care of the connections

// if (!connectstate) connectstate = setup_wifi(); // if wrong passwd it keeps retrying and impossible to enter credentials

if (connectstate){  // if conneted to wifi

// maintain connections; telnet *server* doesnt seem to need it because the client must reconnect
 if (strstr(charlocalmode, "tnc")) {
    establish_telnet();
  } else if (strstr(charlocalmode, "mqtt")) {
     establish_mqtt();
     client.loop();
  }
}
  //for telnet server, there is no such thing as "if (!serverClient.connected()) reconnect()" because it is the client that must reconnect
  // so there is only if connected, then serverClient.println(..) at each time there is something to output


  // input routines

  // incoming data from serial
  while (Serial.available() && newdata == false) {
    lastchar = millis();
    rc = Serial.read();
    if (rc != '\n') {
      //if (rc != '\r')  //suppress carriage return !
      { message[ndx] = rc;
        ndx++;
      }
      if (ndx >= numchars) {
        ndx = numchars - 1;
      }
    }
    else {
      message[ndx] = '\0';
      ndx = 0;
      newdata = true;
      fromlora = false;
    }
  }
  // put the 2s timeout in case the android terminal cannot terminate
  if (ndx != 0 && millis() - lastchar > 2000) {
    message[ndx] = '\0';
    ndx = 0;
    newdata = true;
    fromlora = false;
  }


  // look for Client connect trial in telnet server version
  if (strstr(charlocalmode, "tns")) {
    if (telnetServer.hasClient()) {
      if (!serverClient || !serverClient.connected()) {
        if (serverClient) serverClient.stop();
        serverClient = telnetServer.available();
        serverClient.flush();  // clear input buffer, else you get strange characters
      }

      else telnetServer.available().stop(); //no place
    }  // end hasClient
  } // en if (tnservermode)


  // handle incoming data from telnet
  // works for both telnetserver and telnetclient
  while (serverClient.available() && newdata == false) {
    rc = serverClient.read();
    if (rc != '\n') {
      //if (rc != '\r')  //suppress carriage return !
      { message[ndx] = rc;
        ndx++;
      }
      if (ndx >= numchars) {
        ndx = numchars - 1;
      }
    }
    else {
      message[ndx] = '\0';
      ndx = 0;
      newdata = true;
      fromlora = false;
    }
  }
  delay(10);  // to avoid strange characters left in buffer


  // parse for a packet, and call onReceive with the result:
  onReceive(LoRa.parsePacket());


  // check battery periodically and handle if necessary
  if (millis() - lastmeasured > batmeasperiod)
  { batperiodically();
    lastmeasured = millis();
  }


  // distinguish between data and commands
  if (newdata) {
    if (strstr(message, "AT+") == messptr) {  //if it is a command, begins with AT+ -> strip it off and pass to the command parser
      messptr += 3;
      strcpy(outmessage, messptr); // keep both message and outmessage and write the prog such that message is quickly released
      // in case that it is used by fast interrupt handling routines
      messptr = message; // reset the pointer !
      // lastcmdlocal = !fromlora;
      if (fromlora) lastcmdlocal = 0; else lastcmdlocal = 1; // protocolize where the last command was from, for spontaneous msg as batt
      bounce(); //so that the command acknowledgement goes back to the sender
      context = 0; // the publish within the function is not system message but the ack of the user request
      cmdflag=1; 
      cmdparse(); // proceed to command parsing
      publish(); // publishes outmessage containing command ack; if function is empty, the call is published itself !
      cmdflag=0; 
    }
    else
    { // treat as data, pass on to publish()

      if (strstr(message, "\\relay ") == messptr) { //if it begins with \relay, strip off and send as if it were data
        // but both data and ack go back the same way
        messptr += 7;
        strcpy(message, messptr);
        messptr = message; // reset the pointer !
        bounce(); // inverse fromlora direction byte; both msg and context are reflected the same way in case of \relay
        context = 0;
        strcpy(outmessage, message);
        publish();
        // acknowledge
        context = 1; 
        strcpy(outmessage, "data relayed");
        publish();
        context = 0; 
      }
      else { // if it is not beginning with \relay, treat as data and transfer but send system message back to the sender
        if (strstr(message, "\\verb ") == messptr) {  //if it begins with \verb, strip off, then continue as if it were plain data
          messptr += 6;
          strcpy(message, messptr);
          messptr = message; // reset the pointer !
        }
        context = 0;
        strcpy(outmessage, message);
        publish();
        // acknowledge
        bounce(); // inverse fromlora direction byte;
        context = 1; 
        strcpy(outmessage, "data transfered");
        publish();
        context = 0; 
      } // end not beginning by \relay
    } // end treat as data
    newdata = false;
  } // end if newdata


 
} //end main loop



