/*
 * Aurora RS485 protocol client for ABB / Power-One Aurora inverters.
 *
 * Derived from the ABBAurora Arduino library by jrbenito
 * (https://github.com/jrbenito/ABBAurora), which is based on the original
 * Arduino sketches by Davide Rosa (http://www.drhack.it).
 *
 * Modified by Daniele Vergini for the aurora_monitor ESPHome component:
 *  - added the static disconnect() method;
 *  - added non-blocking requests (Begin*() and Poll()) for state, DSP values,
 *    cumulated energy and any other command (raw answer), configurable timing
 *    and response time measurement;
 *  - include ABBAuroraEnums.h from the component directory;
 *  - moved into the esphome::aurora_monitor namespace; the RS485 direction
 *    pin is an ESPHome GPIOPin (supports inverted pins) instead of the
 *    RS485Transmit/RS485Receive macros;
 *  - removed the unused Wire.h include.
 *
 * This file is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation. See LICENSE-LGPL-2.1.txt in this directory.
 *
 * This file is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
 * for more details.
 */

#ifndef ABBAurora_h
#define ABBAurora_h
#include <Arduino.h>
#include "ABBAuroraEnums.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace aurora_monitor {

// Result of Poll() for a non-blocking request.
enum RequestStatus {
  REQUEST_IDLE,     // no request in progress
  REQUEST_PENDING,  // request in progress, call Poll() again later
  REQUEST_OK,       // valid answer received and parsed
  REQUEST_FAILED,   // no answer, incomplete answer or CRC error
};

class ABBAurora {
 private:
  // Kind of the non-blocking request in progress, used to parse its answer.
  enum PendingKind : byte {
    PENDING_STATE,
    PENDING_DSP,
    PENDING_CUMULATED_ENERGY,
    PENDING_RAW,  // answer left in ReceiveData for the caller to decode
  };

  // Phase of the non-blocking request in progress.
  enum RequestPhase : byte {
    PHASE_IDLE,
    PHASE_TX_ENABLE,  // transceiver switched to transmit, waiting before writing
    PHASE_RESPONSE,   // frame written, waiting for the answer
  };

  int MaxAttempt = 1;
  static GPIOPin *TXPinControl;
  static HardwareSerial *serial;

  unsigned long TxEnableDelayMs = 50;
  unsigned long ResponseTimeoutMs = 1000;

  byte SendData[10];
  PendingKind pendingKind = PENDING_STATE;
  RequestPhase phase = PHASE_IDLE;
  unsigned long phaseStartMs = 0;

  void clearData(byte *data, byte len);

  int Crc16(byte *data, int offset, int count);

  void buildFrame(byte address, byte param0, byte param1, byte param2, byte param3, byte param4, byte param5,
                  byte param6);

  bool checkResponse();

  void discardInput();

  bool Send(byte address, byte param0, byte param1, byte param2, byte param3, byte param4, byte param5, byte param6);

  bool beginRequest(PendingKind kind, byte param0, byte param1, byte param2);

  RequestStatus completeRequest(bool ok);

  bool finishState(bool ok);

  bool finishDSPValue(bool ok);

  bool finishCumulatedEnergy(bool ok);

  union {
    byte asBytes[4];
    float asFloat;
  } foo;

  union {
    byte asBytes[4];
    unsigned long asUlong;
  } ulo;

 public:
  bool SendStatus = false;
  bool ReceiveStatus = false;
  byte Address = 0;
  byte ReceiveData[8];

  static void setup(HardwareSerial &serial, byte RXGpioPin, byte TXGpioPin, GPIOPin *TXControlPin);
  static void disconnect();

  ABBAurora(byte address);

  void clearReceiveData();

  // Time to wait after enabling the transmitter and maximum time to wait
  // for an answer, used by both the blocking and the non-blocking requests.
  void SetTiming(unsigned long txEnableDelayMs, unsigned long responseTimeoutMs);

  // Milliseconds between the end of the last request and its complete answer.
  unsigned long LastResponseMs = 0;

  // Non-blocking requests: Begin*() starts a request, then Poll() must be
  // called until it returns REQUEST_OK or REQUEST_FAILED. The answer is
  // parsed into the same structures filled by the blocking Read*() methods.
  bool BeginReadState();
  bool BeginReadDSPValue(DSP_VALUE_TYPE type, DSP_GLOBAL global);
  bool BeginReadCumulatedEnergy(CUMULATED_ENERGY_TYPE par);
  // Any other command; once Poll() returns REQUEST_OK the 8-byte answer is in
  // ReceiveData. Some answers (P/N, serial number) carry no transmission state.
  bool BeginRawRequest(byte command, byte param = 0);
  RequestStatus Poll();
  bool IsBusy() const { return phase != PHASE_IDLE; }

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    byte InverterState;
    byte Channel1State;
    byte Channel2State;
    byte AlarmState;
    bool ReadState;
  } DataState;

  DataState State;

  bool ReadState();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    String Par1;
    String Par2;
    String Par3;
    String Par4;
    bool ReadState;
  } DataVersion;

  DataVersion Version;

  bool ReadVersion();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    float Value;
    bool ReadState;
  } DataDSP;

  DataDSP DSP;

  bool ReadDSPValue(DSP_VALUE_TYPE type, DSP_GLOBAL global);

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    unsigned long Seconds;
    bool ReadState;
  } DataTimeDate;

  DataTimeDate TimeDate;

  bool ReadTimeDate();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    byte Alarms1;
    byte Alarms2;
    byte Alarms3;
    byte Alarms4;
    bool ReadState;
  } DataLastFourAlarms;

  DataLastFourAlarms LastFourAlarms;

  bool ReadLastFourAlarms();

  bool ReadJunctionBoxState(byte nj);

  bool ReadJunctionBoxVal(byte nj, byte par);

  // Inverters
  typedef struct {
    String PN;
    bool ReadState;
  } DataSystemPN;

  DataSystemPN SystemPN;

  bool ReadSystemPN();

  typedef struct {
    String SerialNumber;
    bool ReadState;
  } DataSystemSerialNumber;

  DataSystemSerialNumber SystemSerialNumber;

  bool ReadSystemSerialNumber();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    String Week;
    String Year;
    bool ReadState;
  } DataManufacturingWeekYear;

  DataManufacturingWeekYear ManufacturingWeekYear;

  bool ReadManufacturingWeekYear();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    String Release;
    bool ReadState;
  } DataFirmwareRelease;

  DataFirmwareRelease FirmwareRelease;

  bool ReadFirmwareRelease();

  typedef struct {
    byte TransmissionState;
    byte GlobalState;
    unsigned long Energy;
    bool ReadState;
  } DataCumulatedEnergy;

  DataCumulatedEnergy CumulatedEnergy;

  bool ReadCumulatedEnergy(CUMULATED_ENERGY_TYPE par);

  bool WriteBaudRateSetting(byte baudcode);

  // Central
  bool ReadFlagsSwitchCentral();

  bool ReadCumulatedEnergyCentral(byte var, byte ndays_h, byte ndays_l, byte global);

  bool ReadFirmwareReleaseCentral(byte var);

  bool ReadBaudRateSettingCentral(byte baudcode, byte serialline);

  bool ReadSystemInfoCentral(byte var);

  bool ReadJunctionBoxMonitoringCentral(byte cf, byte rn, byte njt, byte jal, byte jah);

  bool ReadSystemPNCentral();

  bool ReadSystemSerialNumberCentral();
};

}  // namespace aurora_monitor
}  // namespace esphome

#endif
