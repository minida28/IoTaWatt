  void initTime() {
    uint32_t _NTPtime;

    if(!hasRTC){
      msgLog("Setting time from NTP server.");
      _NTPtime = getNTPtime();
      if(! _NTPtime){
        msgLog("Failed to retrieve NTP time, retrying.");
        while(! _NTPtime){
          _NTPtime = getNTPtime();
        }
      }
      return;
    }

    Wire.pins(pin_I2C_SDA, pin_I2C_SCL);
    rtc.begin();

    if(! rtc.initialized()){
      msgLog("Real Time Clock not initialized - Setting time from NTP server.");
      _NTPtime = getNTPtime();
      if(! _NTPtime){
        msgLog("Failed to retrieve NTP time, retrying.");
        while(! _NTPtime){
          _NTPtime = getNTPtime();
        }
      }
      msgLog("NTP time successfully retrieved.");
      rtc.adjust((uint32_t)_NTPtime - SEVENTY_YEAR_SECONDS);  
      return;
    }

    msgLog("Real Time Clock is running, comparing to NTP server.");
    _NTPtime = getNTPtime();
    if(! _NTPtime){
      msgLog("Failed to retrieve time from NTP server, using RTC time.");
      timeRefNTP = rtc.now().unixtime() + SEVENTY_YEAR_SECONDS;
      timeRefMs = millis();
      return;   
    }
    uint32_t timeDiff = rtc.now().unixtime() - UNIXtime();
    if(timeDiff){
      msgLog("Real Time Clock vs NTP server difference: ", String(timeDiff));
      msgLog("Adjusting Real Time Clock to NTP server time.");
      rtc.adjust(UNIXtime());
      return;
    } 
  }
  
  
  /***************************************************************************************************
   * setNTPtime() - returns uint32_t bimary count of seconds since 1/1/1900 - UTC aka GMT.
   * This is the fundamental time standard used by the program. The value is obtained from one of
   * many synchronized time servers using the basic UDP internet protocol.  This function is a
   * bastard son of several similar routines that I came across while putting this together. Here,
   * the emphasis was on just making it a straightforward utility resource.  It's not well documented,
   * but any similar routine that returns the NTP value could be used.
   * 
   * The program stores the returned value along with the simultaneous value of the internal 
   * millisecond clock.  With those two values and the current millisecond clock value, an updated 
   * NTP time can be computed.  The program checks back in with NTP from time to time for a 
   * reality check.
   * 
   * Unix time is NTP time minus 70 years worth of seconds (seconds since 1/1/70 when the world began). 
   **************************************************************************************************/ 
  uint32_t getNTPtime() {
    WiFiUDP udp;
    const char* ntpServerName = "time.nist.gov";
    const int NTP_PACKET_SIZE = 48;
    unsigned int localPort = 2390;
    byte packetBuffer[NTP_PACKET_SIZE] = {0xE3, 0, 6, 0xEC, 0,0,0,0,0,0,0,0, 49, 0x4E, 49, 52};
    const byte packet[16] = {0xE3, 0, 6, 0xEC, 0,0,0,0,0,0,0,0, 49, 0x4E, 49, 52};
    udp.begin(localPort);
    IPAddress timeServerIP;
    WiFi.hostByName(ntpServerName, timeServerIP);       // get a random server from the pool
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    for(int i=0; i<16; i++) packetBuffer[i] = packet[i];
    udp.beginPacket(timeServerIP, 123);
    udp.write(packetBuffer, NTP_PACKET_SIZE);           // send an NTP packet to a time server
    udp.endPacket();
    
    int maxWait = 5;                                   // We'll wait 10 100ms intervals
    while(maxWait--){
      if(udp.parsePacket()){
        udp.read(packetBuffer,NTP_PACKET_SIZE);
        udp.stop();
        timeRefNTP = (((((packetBuffer[40] << 8) | packetBuffer[41]) << 8) | packetBuffer[42]) << 8) | packetBuffer[43];
        timeRefMs = millis();
        return timeRefNTP;
      }
      yield();
      delay(100);
    }
    udp.stop();
    return 0;
  }

/********************************************************************************************
 * 
 *  uint32_t NTPtime() - Return the current time in NTP format
 *  uint32_t Unixtime() - Return the current time in Unix format
 *  Both return zero if the clock is not set//
 *  uint32_t MillisAtUNIXtime - Return the local Millis() value corresponding to the UnixTime
 * 
 *******************************************************************************************/

uint32_t NTPtime() {
  if(timeRefNTP == 0) return 0;
  return timeRefNTP + ((uint32_t)(millis() - timeRefMs)) / 1000;
 }
  
uint32_t UNIXtime() {
  if(timeRefNTP == 0) return 0;
  return timeRefNTP + ((uint32_t)(millis() - timeRefMs)) / 1000 - SEVENTY_YEAR_SECONDS;
 }
 
 

uint32_t MillisAtUNIXtime(uint32_t UnixTime){                  
  return (uint32_t)timeRefMs + 1000 * (UnixTime - SEVENTY_YEAR_SECONDS - timeRefNTP);
 }

/********************************************************************************************
 * timeSync is a SERVICE that periodically (timeSynchInterval seconds) attempts to get
 * the NTP time from the internet and synchronize to it.  No heroics to compensate for
 * internet latency.  Close enough.
 *******************************************************************************************/
 
uint32_t timeSync(struct serviceBlock* _serviceBlock){
  static boolean firstCall = true;
  if(firstCall){
    msgLog("timeSync: service started.");
    firstCall = false;
  }
  uint32_t _NTPtime = getNTPtime();
  if(! _NTPtime){
    msgLog("timeSynch: failed to get NTP time.");
  } else {
    timeRefNTP = _NTPtime;                 
    timeRefMs = millis();
    if(hasRTC){
      int32_t timeDiff = UNIXtime() - rtc.now().unixtime();
      if(timeDiff){
        msgLog("timeSynch: adjusting RTC by ", String(timeDiff));
        rtc.adjust(UNIXtime());
      }
    }
  }
  return ((uint32_t) timeSynchInterval * ( 1 + UNIXtime() / timeSynchInterval));
}

/********************************************************************************
 *   dateTime callback for SD so it can maintain dates in the directory.
 ********************************************************************************/

void dateTime(uint16_t* date, uint16_t* time) {
  uint32_t localUnixTime = UNIXtime() + localTimeDiff*3600;
  
  // return date using FAT_DATE macro to format fields
  *date = FAT_DATE(DateTime(localUnixTime).year(),
                   DateTime(localUnixTime).month(),
                   DateTime(localUnixTime).day());

  // return time using FAT_TIME macro to format fields
  *time = FAT_TIME(DateTime(localUnixTime).hour(), 
                   DateTime(localUnixTime).minute(),
                   DateTime(localUnixTime).second());
}

/********************************************************************************************
 * Just a handy little formatter.  
 *******************************************************************************************/
String formatHMS(uint32_t epoch) {
    String result = String((epoch  % 86400L) / 3600) + ":";
    if ( ((epoch % 3600) / 60) < 10 ) result += "0";
    result += String((epoch  % 3600) / 60) + ":";
    if ( (epoch % 60) < 10 ) result += "0";
    result += String(epoch % 60);
    return result;
  }

  

  
        
   
    
  

