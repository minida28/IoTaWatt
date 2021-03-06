  /***************************************************************************************************
  *  samplePower()  Sample a channel.
  *  
  ****************************************************************************************************/
void samplePower(int channel, int overSample){
  uint32_t timeNow = millis();

      // If it's a voltage channel, use voltage only sample, update and return.
  
  if(channelType[channel] == channelTypeVoltage){
    ageBucket(&buckets[channel], timeNow);
    buckets[channel].volts = sampleVoltage(channel, calibration[channel]);
    return;
  }

         // Currently only voltage and power channels, so return if not one of those.
     
  if(channelType[channel] != channelTypePower) return;


         // From here on, dealing with a power channel and associated voltage channel.
     
  
  byte Vchan = Vchannel[channel];
  byte Ichan = channel;

  double _Irms = 0;
  double _watts = 0;
  double _Vrms = 0;

  int16_t* VsamplePtr = Vsample;
  int16_t* IsamplePtr = Isample;
  
         // Determine phase correction oversample requirement.

  float _phaseCorrection = (phaseCorrection[Vchan] - phaseCorrection[Ichan]) * samplesPerCycle / 360.0;
  int stepCorrection = int(_phaseCorrection);
  float stepFraction = _phaseCorrection - stepCorrection;
  int _overSample = 0;
  if(_phaseCorrection >= 0){
    IsamplePtr += stepCorrection;
    _overSample = stepCorrection + 2;
  }
  else {
    VsamplePtr += -stepCorrection;
    _overSample = -stepCorrection + 2;
  }
  if(overSample > _overSample) _overSample = overSample;
  
        // Invoke high speed sample collection.
        // If it fails, set power to zero and return.

  if( ! sampleCycle(Vchan, Ichan, _overSample)) { 
    ageBucket(&buckets[Ichan], timeNow);
    buckets[Ichan].watts = 0;
    buckets[Ichan].amps = 0;
    return;
  }               
       
    // Samples are collected, process them.
    
  int16_t rawV;
  int16_t rawI;  
  int32_t sumV = 0;
  int32_t sumI = 0;
  int32_t sumP = 0;
  int32_t sumVsq = 0;
  int32_t sumIsq = 0;       
  
        // get sums, squares, and all that stuff.
  
  for(int i=0; i<samples; i++){  
    rawV = *VsamplePtr;
    rawI = *IsamplePtr;
    if(stepFraction > 0) rawI += int(stepFraction * (*(IsamplePtr + 1) - *IsamplePtr));
    if(stepFraction < 0) rawV += int(-stepFraction * (*(VsamplePtr + 1) - *VsamplePtr));
    sumV += rawV;
    sumVsq += rawV * rawV;
    sumI += rawI;
    sumIsq += rawI * rawI;
    sumP += rawV * rawI;      
    VsamplePtr++;
    IsamplePtr++;  
  }
  
        // Adjust the offset values assuming symetric waves but within limits otherwise.

  int16_t offsetV = offset[Vchan] + sumV / samples;
  if(offsetV < minOffset) offsetV = minOffset;
  if(offsetV > maxOffset) offsetV = maxOffset;
  offset[Vchan] = offsetV;
  
  int16_t offsetI = offset[Ichan] + sumI / samples;
  if(offsetI < minOffset) offsetI = minOffset;
  if(offsetI > maxOffset) offsetI = maxOffset;
  offset[Ichan] = offsetI;
    
        // Voltage is relative to input and is attenuated more for 1.2V settings
        // so apply adjustment depending on Aref voltage.
    
  double Vratio = calibration[Vchan] * Vadj_3 * getAref(Vchan) / double(ADC_range);
  if(getAref(Vchan) < 1.5) Vratio *= (double)Vadj_1 / Vadj_3;

        // Iratio is straight Amps/ADC volt.
  
  double Iratio = calibration[Ichan] * getAref(Ichan) / double(ADC_range);

        // Now that the preliminaries are over, 
        // Getting Vrms, Irms, and Watts is easy.
  
  _Vrms = Vratio * sqrt((double)(sumVsq / samples));
  _Irms = Iratio * sqrt((double)(sumIsq / samples));
  _watts = Vratio * Iratio * (double)(sumP / samples);

        // If watts is negative and the channel is not explicitely signed, reverse it (backward CT).
        // If we do reverse it, mark it as such for reporting in the status API.
  
  if(_watts < 0 && !CTsigned[Ichan]){
    _watts = -_watts;
    if(_watts > 1) CTreversed[Ichan] = true;
  } else {
    CTreversed[Ichan] = false;
  }

      // Age the previous power and voltage data and set new values

  ageBucket(&buckets[Ichan], timeNow);   
  buckets[Ichan].watts = _watts;
  buckets[Ichan].amps = _Irms;
  
  ageBucket(&buckets[Vchan], timeNow);
  buckets[Vchan].volts = _Vrms;

  return;
}







  /**********************************************************************************************
  * 
  *  sampleCycle(Vchan, Ichan)
  *  
  *  This code accounts for up to 66% (60Hz) of the execution of IoTaWatt.
  *  It collects voltage and current sample pairs and saves them away for 
  *    
  *  The approach is to start sampling voltage/current pairs in a tight loop.
  *  When voltage crosses zero, we start recording the pairs.
  *  When we have crossed zero cycles*2 more times, we grab 100 more then stop and compute the results.
  *  
  *  Note:  If ever there was a time for low-level hardware manipulation, this is it.
  *  the tighter and faster the samples can be taken, the more accurate the results can be.
  *  The ESP8266 has pretty good SPI functions, but even using them optimally, it's only possible
  *  to achieve about 350 sample pairs per cycle.
  *  
  *  By manipulating the SPI chip select pin through hardware registers and
  *  running the SPI for only the required bits, again using the hardware 
  *  registers, it's possinble to get about 500 sample pairs per cycle running
  *  the SPI at 2MHz, which is the spec for the MCP3208 at 5v.
  *  
  *  The code supports both the MCP3008(10 bit) and MCP3208(12 bit) ADCs.
  *  Although there is currently no way to configure the 3008, the support
  *  here and elsewhere is low profile enough that it was left in.  
  *  
  *  I've tried to segregate the bit-banging and document it well.
  *  For anyone interested in the low level registers, you can find 
  *  them defined in esp8266_peri.h.
  * 
  ****************************************************************************************************/
  
boolean sampleCycle(int Vchan, int Ichan, int overSample) {
  
  #define cycles 1                            // Cycles to sample (>1 you're on your own)
  
  uint32_t cmdMask = ((4 << SPILMOSI) | (4 << SPILMISO));
  uint32_t dataMask = ((ADC_bits + 1) << SPILMOSI) | ((ADC_bits + 1) << SPILMISO);
  const uint32_t mask = ~((SPIMMOSI << SPILMOSI) | (SPIMMISO << SPILMISO));
  volatile uint8_t * fifoPtr8 = (volatile uint8_t *) &SPI1W0;
  
  uint8_t  Iport = chanAddr[Ichan] % 8;       // Port on ADC
  uint8_t  Vport = chanAddr[Vchan] % 8;
    
  int16_t offsetV = offset[Vchan];            // Bias offset
  int16_t offsetI = offset[Ichan];
  
  int16_t rawV;                               // Raw ADC readings
  int16_t lastV;
      
  int16_t * VsamplePtr = Vsample;             // -> to sample storage arrays
  int16_t * IsamplePtr = Isample;
    
  int16_t crossLimit = cycles * 2 + 1;        // number of crossings in total
  int16_t crossCount = 0;                     // number of crossings encountered
  int16_t crossGuard = 0;                     // Guard against faux crossings
  
  uint32_t startMs = millis();                // Start of current half cycle
  uint32_t timeoutMs = 600 / frequency;       // Maximum time allowed per half cycle
  uint32_t firstCrossUs;                      // Time cycle at usec resolution for phase calculation
  uint32_t lastCrossUs;                       

  byte ADC_IselectPin = ADC_selectPin[chanAddr[Ichan] >> 3];  // Chip select pin
  byte ADC_VselectPin = ADC_selectPin[chanAddr[Vchan] >> 3];
  uint32_t ADC_IselectMask = 1 << ADC_IselectPin;             // Mask for hardware chip select (pins 0-15)
  uint32_t ADC_VselectMask = 1 << ADC_VselectPin;

  boolean ADC_Iselect16 = false;                              // Pin 16 is special, so take note.
  if(ADC_IselectPin == 16) ADC_Iselect16 = true;
  boolean ADC_Vselect16 = false;
  if(ADC_VselectPin == 16) ADC_Vselect16 = true;
  
  SPI.beginTransaction(SPISettings(2000000,MSBFIRST,SPI_MODE0));
 
  if(readADC(Ichan) < 4) return false;                // channel is unplugged (grounded)

  rawV = readADC(Vchan) - offsetV;                    // Prime the pump
  samples = 0;                                        // Start with nothing

          // Have at it.
                      
  do {                                     
                      /************************************
                       *  Sample the Current (I) channel  *
                       ************************************/
          
        if(ADC_Iselect16) GP16O &= ~1;
        else GPOC = ADC_IselectMask;                     // digitalWrite(ADC_IselectPin, LOW); Select the ADC
  
              // hardware send 5 bit start + sgl/diff + port_addr
        
        SPI1U1 = (SPI1U1 & mask) | cmdMask;
        SPI1W0 = (0x18 | Iport) << 3;
        SPI1CMD |= SPIBUSY;
        while(SPI1CMD & SPIBUSY) {} 
        delayMicroseconds(1);                           // extend the sample&hold time
            
              // Sample and Hold then...
              // Read the results
                                  
        SPI1U1 = (SPI1U1 & mask) | dataMask;
        SPI1W0 = 0xFF;
        SPI1CMD |= SPIBUSY;
        
              // Do some housekeeping asynchronously while SPI runs.
        
          *VsamplePtr = (lastV + rawV) >> 1;                // Average before and after to align with I in time
          lastV = rawV;
        
          if(crossCount) {                                  // If past first crossing 
            if(crossCount < crossLimit){
              samples++;
              VsamplePtr++;                                 // Accumulate samples
              IsamplePtr++;                                 // Only count samples between crossings
              if(samples >= MAX_SAMPLES){                   // If over the legal limit
                trace(T_SAMP,0);                            // shut down and return
                if(ADC_Iselect16) GP16O = 1;                // (Chip select high)
                else GPOS = ADC_IselectMask;                
                return false;
              }
            }
          }
          crossGuard--;                                     // count down crossGuard                         
        
              // Now wait for SPI to complete
        
        while(SPI1CMD & SPIBUSY) {}                       
                  
        if(ADC_Iselect16) GP16O = 1;                        // digitalWrite(ADC_IselectPin, HIGH);  Deselect the ADC
        else GPOS = ADC_IselectMask;                       

              // extract the rawI from the SPI hardware buffer and adjust with offset.
        
        *IsamplePtr = (word((*fifoPtr8 & 0x3f), *(fifoPtr8+1)) >> (14 - ADC_bits)) - offsetI;

                      /************************************
                       *  Sample the Voltage (V) channel  *
                       ************************************/
                                   
        if(ADC_Vselect16) GP16O &= ~1;                      // digitalWrite(ADC_VselectPin, LOW); Select the ADC
        else GPOC = ADC_VselectMask;   

              // hardware send 5 bit start + sgl/diff + port_addr
                                            
        SPI1U1 = (SPI1U1 & mask) | cmdMask;                 // Set number of bits 
        SPI1W0 = (0x18 | Vport) << 3;                      // Data left aligned in low byte 
        SPI1CMD |= SPIBUSY;                                // Start the SPI clock  
        while(SPI1CMD & SPIBUSY) {}                        // Loop till SPI completes  
        delayMicroseconds(1);
                                                 
              // Sample and Hold then...
              // Start reading the results
            
        SPI1U1 = (SPI1U1 & mask) | dataMask;                                // Set number of bits (n-1)
        SPI1W0 = 0xFFFF;                                                    // Pad buffer with ones (don't know why)
        SPI1CMD |= SPIBUSY;                                                 // Start the SPI clock

              // Do some loop housekeeping asynchronously while SPI runs.
        
         if((*IsamplePtr > -3) && (*IsamplePtr < 3)) *IsamplePtr = 0;       // Filter noise from previous reading while SPI reads ADC

              // Check for timeout.  The clock gets reset at each crossing, so the
              // timeout value is a little more than a half cycle - 10ms @ 60Hz, 12ms @ 50Hz.
              // The most common cause of timeout here is unplugging the AC reference VT.  Since the
              // device is typically sampling 60% of the time, there is a high probability this
              // will happen if the adapter is unplugged.
              // So handling needs to be robust.
        
          if((uint32_t)(millis()-startMs)>timeoutMs){                       // Something is wrong
            trace(T_SAMP,2);                                                // Leave a meaningful trace
            trace(T_SAMP,Ichan);
            trace(T_SAMP,Vchan);
            if(ADC_Vselect16) GP16O = 1;                                    // ADC select pin high
            else GPOS = ADC_VselectMask;                                    // Diagnostic stuff if anybody is listening
            return false;                                                   // Return a failure
          }
          
              // Now wait for SPI to complete
        
        while(SPI1CMD & SPIBUSY) {}                                         // Loop till SPI completes
        if(ADC_Vselect16) GP16O = 1;                                        // digitalWrite(ADC_VselectPin, HIGH); Deselect the ADC
        else GPOS = ADC_VselectMask;    

              // extract the rawV from the SPI hardware buffer and adjust with offset. 
                                                                    
        rawV = (word((*fifoPtr8 & 0x3f), *(fifoPtr8+1)) >> (14 - ADC_bits)) - offsetV;  // Result is in left aligned in first FiFo word

        // Finish up loop cycle by checking for zero crossing.

        // Crossing is defined by I and V having different signs (Xor) and crossGuard negative.

        if(((rawV ^ lastV) & crossGuard) >> 15) {        // If crossed unambiguously (one but not both Vs negative and crossGuard negative 
          ESP.wdtFeed();                                 // Red meat for the silicon dog
          WDT_FEED();
          startMs = millis();                            // Reset the cycle clock 
          crossCount++;                                  // Count this crossing
          crossGuard = 10;                               // No more crosses for awhile
          if(crossCount == 1){
            trace(T_SAMP,4);
            firstCrossUs = micros();
          }
          else if(crossCount == crossLimit) {
            trace(T_SAMP,6);
            lastCrossUs = micros();                     // To compute frequency
            lastCrossMs = millis();                     // For main loop dispatcher to estimate when next crossing is imminent
            crossGuard = overSample;                   // Collect caller's required oversamples;
          }                               
        }

          // Keep sampling until prescribed crossings + phase shift overun
   
  } while(crossCount < crossLimit  || crossGuard > 0);  
  
  *VsamplePtr = (lastV + rawV) / 2;                     // Loose end

        // If AC is floating, we can end up here with the noise looking enough like crossings.
        // If that's the case, this should detect it.  
  
  if(*VsamplePtr > -10 && *VsamplePtr < 10){
    trace(T_SAMP,7);
    return false;
  }
  trace(T_SAMP,8);

          // Update damped frequency.

  frequency = (0.9 * frequency) + (0.1 * (1000000.0 * cycles)  / float((uint32_t)(lastCrossUs - firstCrossUs)));

          // Note the sample rate.
          // This is just a snapshot from single cycle sampling.
          // It can be a little off per cycle, but by damping the 
          // saved value we can get a pretty accurate average.

  samplesPerCycle = samplesPerCycle * .9 + (samples / cycles) * .1;
  cycleSamples++;
  
  return true;
}


//**********************************************************************************************
//
//        readADC(uint8_t channel)
//
//**********************************************************************************************

int readADC(uint8_t channel)
{ 
  uint32_t align = 0;               // SPI requires out and in to be word aligned                                                                 
  uint8_t ADC_out [4] = {0, 0, 0, 0};
  uint8_t ADC_in  [4] = {0, 0, 0, 0};  
  uint8_t ADCselectPin;
  
  SPI.beginTransaction(SPISettings(2000000,MSBFIRST,SPI_MODE0));  // SD may have changed this
  ADCselectPin = ADC_selectPin[chanAddr[channel] >> 3];
      
  ADC_out[0] = 0x18 | (chanAddr[channel] & 0x07);
  
  digitalWrite(ADCselectPin, LOW);                  // Lower the chip select
  SPI.transferBytes(ADC_out, ADC_in, 1);            // Start bit, single bit, ADC port
                                                    // At this point ADC is sampling the port
                                                    // by charging the S&H capacitor. The sample 
                                                    // period ends when we initiate the next read
  SPI.transferBytes(ADC_out, ADC_in, 2);            // Start reading the results
  digitalWrite(ADCselectPin, HIGH);                 // Raise the chip select to deselect and reset
  
  return (word(ADC_in[0] & 0x3F, ADC_in[1]) >> (14 - ADC_bits)); // Put the result together and return
}

/****************************************************************************************************
 * sampleVoltage() is used during calibration of a Voltage channel.
 * It samples one cycle and returns the voltage corresponding to the supplied calibration factor.
 ****************************************************************************************************/
float sampleVoltage(uint8_t Vchan, float Vcal){
  uint32_t sumVsq = 0;
  int16_t rawV = 0;
  int16_t lastV = 0;
  int16_t offsetV = offset[Vchan];
  int16_t* VsamplePtr = Vsample;
  uint32_t startMs = millis();
  uint32_t firstCrossUs;
  uint32_t timeoutMs = 600 / frequency;
  int16_t crossCount = 0;
  int16_t crossLimit = 3;
  int16_t crossGuard = 0;
  samples = 0;

  rawV = readADC(Vchan) - offsetV;
  while(crossCount < crossLimit){
    lastV = rawV;
    rawV = readADC(Vchan) - offsetV;
    *VsamplePtr = rawV;
    
    if(((rawV ^ lastV) & crossGuard) >> 15) {  
      crossCount++;
      crossGuard = 10;
      startMs = millis();
      if(crossCount == 1){
        firstCrossUs = micros();     
      }
    }
    crossGuard--;
    
    if(crossCount){
      VsamplePtr++;
      sumVsq += rawV * rawV;
      samples++;
    }     
    
    if((uint32_t)(millis()-startMs) > timeoutMs){
      return 0;
    }
    
  }
  buckets[Vchan].hz = 1000000.0  / float((uint32_t)(micros() - firstCrossUs));
  frequency = (0.9 * frequency) + (0.1 * buckets[Vchan].hz);
  samplesPerCycle = samplesPerCycle * .9 + (samples / cycles) * .1;
  cycleSamples++;
  double Vratio = Vcal * Vadj_3 * getAref(Vchan) / double(ADC_range);
  if(getAref(Vchan) < 1.5) Vratio *= (double)Vadj_1 / Vadj_3;
  return  Vratio * sqrt((double)(sumVsq / samples));
}
//**********************************************************************************************
//
//        getAref()  -  Get the current value of Aref
//
//**********************************************************************************************

float getAref(int channel) {
   
  uint32_t align = 0;               // SPI requires out and in to be word aligned                                                                 
  uint8_t ADC_out [4] = {0, 0, 0, 0};
  uint8_t ADC_in  [4] = {0, 0, 0, 0};  
  uint8_t ADCselectPin;
  
  SPI.beginTransaction(SPISettings(2000000,MSBFIRST,SPI_MODE0));  // SD may have changed this
  ADCselectPin = ADC_selectPin[chanAref[channel] >> 3];    
  ADC_out[0] = 0x18 | (chanAref[channel] & 0x07);            

  digitalWrite(ADCselectPin, LOW);                  // Lower the chip select
  SPI.transferBytes(ADC_out, ADC_in, 1);            // Start bit, single bit, ADC port
                                                    // At this point ADC is sampling the port
                                                    // by charging the S&H capacitor. The sample 
                                                    // period ends when we initiate the next read
  SPI.transferBytes(ADC_out, ADC_in, 2);            // Start reading the results
  digitalWrite(ADCselectPin, HIGH);                 // Raise the chip select to deselect and reset
                                                    // Put the result together and return
  uint16_t ADCvalue = (word(ADC_in[0] & 0x3F, ADC_in[1]) >> (14 - ADC_bits));
  if(ADCvalue == 4095 | ADCvalue == 0) return 0;    // no ADC
  return VrefVolts * ADC_range / ADCvalue;  
}

//**********************************************************************************************
//
//        printSamples()  -  print the current samples.
//        fileSamples() - write the current samples to an SD file ("samples.txt").
//        These are diagnostic tools, not currently used.
//
//**********************************************************************************************

void printSamples() {
  Serial.println(samples);
  for(int i=0; i<(samples + 50); i++)
  {
    Serial.print(Vsample[i]);
    Serial.print(", ");
    Serial.print(Isample[i]);
    Serial.println();
  }
  return;
}

void fileSamples() {
  File sampleFile;
  SD.remove("samples.txt");
  sampleFile = SD.open("samples.txt",FILE_WRITE);
  sampleFile.println(samples);
  for(int i=0; i<(samples + 50); i++)
  {
    sampleFile.print(Vsample[i]);
    sampleFile.print(", ");
    sampleFile.print(Isample[i]);
    sampleFile.println();
  }
  sampleFile.close();
  return;
}


