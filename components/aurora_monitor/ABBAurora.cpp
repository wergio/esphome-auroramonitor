/*
 * Aurora RS485 protocol client for ABB / Power-One Aurora inverters.
 *
 * Derived from the ABBAurora Arduino library by jrbenito
 * (https://github.com/jrbenito/ABBAurora), which is based on the original
 * Arduino sketches by Davide Rosa (http://www.drhack.it).
 *
 * Modified by Daniele Vergini for the aurora_monitor ESPHome component:
 *  - setup() configures the hardware UART for ESP32 and ESP8266, swapping
 *    UART0 to the alternate pins (GPIO13/GPIO15) on ESP8266;
 *  - added the static disconnect() method;
 *  - fixed Crc16() indexing the buffer at offset twice (data[offset + i]
 *    inside a loop that already starts at offset);
 *  - Send() discards leftover input bytes after a CRC error;
 *  - added non-blocking requests (Begin*() and Poll()) for state, DSP values,
 *    cumulated energy and any other command (BeginRawRequest()); the first
 *    three answers are parsed by the same
 *    finish*() helpers used by ReadState(), ReadDSPValue() and
 *    ReadCumulatedEnergy();
 *  - configurable transmit-enable delay and response timeout, and
 *    measurement of the response time;
 *  - added ESPHome logging of sent and received frames and of errors;
 *  - setup() no longer waits 500 ms: the caller waits for the port to settle;
 *  - moved into the esphome::aurora_monitor namespace; the RS485 direction
 *    pin is an ESPHome GPIOPin (supports inverted pins).
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

#include "ABBAurora.h"
#include "ABBAuroraStrings.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aurora_monitor {

static const char *const TAG = "aurora_monitor";

GPIOPin *ABBAurora::TXPinControl = nullptr;
HardwareSerial *ABBAurora::serial = nullptr;

ABBAurora::ABBAurora(byte address) {
  Address = address;
  SendStatus = false;
  ReceiveStatus = false;
  clearReceiveData();
}
void ABBAurora::setup(HardwareSerial &hardwareSerial, byte RXGpioPin, byte TXGpioPin, GPIOPin *TXControlPin) {
  TXPinControl = TXControlPin;

  TXPinControl->setup();
  TXPinControl->digital_write(false);  // receive

  serial = &hardwareSerial;
#if defined(USE_ESP32)
  serial->begin(19200, SERIAL_8N1, RXGpioPin, TXGpioPin, false, 500);
#elif defined(USE_ESP8266)
  serial->begin(19200, SERIAL_8N1, SERIAL_FULL);
  if (RXGpioPin == 13 && TXGpioPin == 15) {
    // Use alternate pins on Serial
    serial->swap();
  }
#endif
  // The caller waits for the port to settle before the first request.
}

void ABBAurora::disconnect() {
  if (serial != nullptr)
    serial->end();
}

void ABBAurora::clearData(byte *data, byte len) {
  for (int i = 0; i < len; i++) {
    data[i] = 0;
  }
}

int ABBAurora::Crc16(byte *data, int offset, int count) {
  byte BccLo = 0xFF;
  byte BccHi = 0xFF;

  for (int i = offset; i < (offset + count); i++) {
    byte New = data[i] ^ BccLo;
    byte Tmp = New << 4;
    New = Tmp ^ New;
    Tmp = New >> 5;
    BccLo = BccHi;
    BccHi = New ^ Tmp;
    Tmp = New << 3;
    BccLo = BccLo ^ Tmp;
    Tmp = New >> 4;
    BccLo = BccLo ^ Tmp;
  }

  return (int) word(~BccHi, ~BccLo);
}

void ABBAurora::SetTiming(unsigned long txEnableDelayMs, unsigned long responseTimeoutMs) {
  TxEnableDelayMs = txEnableDelayMs;
  ResponseTimeoutMs = responseTimeoutMs;
}

void ABBAurora::buildFrame(byte address, byte param0, byte param1, byte param2, byte param3, byte param4, byte param5,
                           byte param6) {
  SendData[0] = address;
  SendData[1] = param0;
  SendData[2] = param1;
  SendData[3] = param2;
  SendData[4] = param3;
  SendData[5] = param4;
  SendData[6] = param5;
  SendData[7] = param6;

  int crc = Crc16(SendData, 0, 8);
  SendData[8] = lowByte(crc);
  SendData[9] = highByte(crc);
}

bool ABBAurora::checkResponse() {
  if ((int) word(ReceiveData[7], ReceiveData[6]) == Crc16(ReceiveData, 0, 6)) {
    ESP_LOGV(TAG, "Serial Read OK %d %d %d %d %d %d %d %d", ReceiveData[0], ReceiveData[1], ReceiveData[2],
             ReceiveData[3], ReceiveData[4], ReceiveData[5], ReceiveData[6], ReceiveData[7]);
    return true;
  }
  ESP_LOGW(TAG, "Serial Read CRC Error %d %d %d %d %d %d %d %d", ReceiveData[0], ReceiveData[1], ReceiveData[2],
           ReceiveData[3], ReceiveData[4], ReceiveData[5], ReceiveData[6], ReceiveData[7]);
  // flush() only waits for outgoing data: discard any leftover
  // input bytes so the next frame starts aligned.
  discardInput();
  return false;
}

void ABBAurora::discardInput() {
  while (serial->available() > 0)
    serial->read();
}

bool ABBAurora::Send(byte address, byte param0, byte param1, byte param2, byte param3, byte param4, byte param5,
                     byte param6) {
  SendStatus = false;
  ReceiveStatus = false;

  buildFrame(address, param0, param1, param2, param3, param4, param5, param6);
  clearReceiveData();
  serial->setTimeout(ResponseTimeoutMs);

  for (int i = 0; i < this->MaxAttempt; i++) {
    TXPinControl->digital_write(true);  // transmit
    delay(TxEnableDelayMs);

    ESP_LOGV(TAG, "Serial Write %d %d %d %d %d %d %d %d %d %d", SendData[0], SendData[1], SendData[2], SendData[3],
             SendData[4], SendData[5], SendData[6], SendData[7], SendData[8], SendData[9]);
    if (serial->write(SendData, sizeof(SendData)) != 0) {
      serial->flush();
      SendStatus = true;

      TXPinControl->digital_write(false);  // receive
      unsigned long sentMs = millis();

      if (serial->readBytes(ReceiveData, sizeof(ReceiveData)) == sizeof(ReceiveData)) {
        LastResponseMs = millis() - sentMs;
        if (checkResponse()) {
          ReceiveStatus = true;
          break;
        }
      } else {
        ESP_LOGV(TAG, "Serial Read Error NO response");
      }
    } else {
      TXPinControl->digital_write(false);  // receive
      ESP_LOGW(TAG, "Serial Write Error");
    }
  }
  return ReceiveStatus;
}

bool ABBAurora::beginRequest(PendingKind kind, byte param0, byte param1, byte param2) {
  if (phase != PHASE_IDLE)
    return false;

  SendStatus = false;
  ReceiveStatus = false;
  buildFrame(this->Address, param0, param1, param2, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
  clearReceiveData();
  pendingKind = kind;

  TXPinControl->digital_write(true);  // transmit
  phase = PHASE_TX_ENABLE;
  phaseStartMs = millis();
  return true;
}

RequestStatus ABBAurora::Poll() {
  switch (phase) {
    case PHASE_IDLE:
      return REQUEST_IDLE;

    case PHASE_TX_ENABLE:
      if (millis() - phaseStartMs < TxEnableDelayMs)
        return REQUEST_PENDING;

      // Drop anything received before this request (e.g. a late answer).
      discardInput();
      ESP_LOGV(TAG, "Serial Write %d %d %d %d %d %d %d %d %d %d", SendData[0], SendData[1], SendData[2], SendData[3],
               SendData[4], SendData[5], SendData[6], SendData[7], SendData[8], SendData[9]);
      if (serial->write(SendData, sizeof(SendData)) == 0) {
        TXPinControl->digital_write(false);  // receive
        ESP_LOGW(TAG, "Serial Write Error");
        return completeRequest(false);
      }
      // Writing 10 bytes at 19200 baud takes about 5 ms.
      serial->flush();
      SendStatus = true;
      TXPinControl->digital_write(false);  // receive
      phase = PHASE_RESPONSE;
      phaseStartMs = millis();
      return REQUEST_PENDING;

    case PHASE_RESPONSE: {
      if (serial->available() >= (int) sizeof(ReceiveData)) {
        LastResponseMs = millis() - phaseStartMs;
        serial->readBytes(ReceiveData, sizeof(ReceiveData));
        return completeRequest(checkResponse());
      }
      if (millis() - phaseStartMs < ResponseTimeoutMs)
        return REQUEST_PENDING;

      int received = serial->available();
      if (received > 0) {
        ESP_LOGW(TAG, "Serial Read Error incomplete response (%d bytes)", received);
        discardInput();
      } else {
        ESP_LOGV(TAG, "Serial Read Error NO response");
      }
      return completeRequest(false);
    }
  }
  return REQUEST_IDLE;
}

RequestStatus ABBAurora::completeRequest(bool ok) {
  phase = PHASE_IDLE;
  ReceiveStatus = ok;

  switch (pendingKind) {
    case PENDING_STATE:
      finishState(ok);
      break;
    case PENDING_DSP:
      finishDSPValue(ok);
      break;
    case PENDING_CUMULATED_ENERGY:
      finishCumulatedEnergy(ok);
      break;
    case PENDING_RAW:
      break;
  }
  return ok ? REQUEST_OK : REQUEST_FAILED;
}

bool ABBAurora::BeginReadState() { return beginRequest(PENDING_STATE, (byte) 50, (byte) 0, (byte) 0); }

static bool isValidDSPRequest(DSP_VALUE_TYPE type, DSP_GLOBAL global) {
  return (((int) type >= 1 && (int) type <= 9) || ((int) type >= 21 && (int) type <= 63)) &&
         ((int) global >= 0 && (int) global <= 1);
}

bool ABBAurora::BeginReadDSPValue(DSP_VALUE_TYPE type, DSP_GLOBAL global) {
  if (!isValidDSPRequest(type, global))
    return false;
  return beginRequest(PENDING_DSP, (byte) 59, type, global);
}

bool ABBAurora::BeginReadCumulatedEnergy(CUMULATED_ENERGY_TYPE par) {
  if ((int) par < 0 || (int) par > 6)
    return false;
  return beginRequest(PENDING_CUMULATED_ENERGY, (byte) 78, par, (byte) 0);
}

bool ABBAurora::BeginRawRequest(byte command, byte param) {
  return beginRequest(PENDING_RAW, command, param, (byte) 0);
}

void ABBAurora::clearReceiveData() { clearData(ReceiveData, 8); }

/**
 * Reads a single value of the digital signal procesor.
 * Not all values are supported by all models.
 * Read values are in following Units:
 * Voltage V
 * Current A
 * Power W
 * Temperature °C
 *
 **/
bool ABBAurora::ReadDSPValue(DSP_VALUE_TYPE type, DSP_GLOBAL global) {
  if (isValidDSPRequest(type, global))
    return finishDSPValue(Send(this->Address, (byte) 59, type, global, (byte) 0, (byte) 0, (byte) 0, (byte) 0));

  clearReceiveData();
  return finishDSPValue(false);
}

bool ABBAurora::finishDSPValue(bool ok) {
  DSP.ReadState = ok;

  if (DSP.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  DSP.TransmissionState = ReceiveData[0];
  DSP.GlobalState = ReceiveData[1];

  foo.asBytes[0] = ReceiveData[5];
  foo.asBytes[1] = ReceiveData[4];
  foo.asBytes[2] = ReceiveData[3];
  foo.asBytes[3] = ReceiveData[2];

  DSP.Value = foo.asFloat;

  return DSP.ReadState;
}

bool ABBAurora::ReadTimeDate() {
  TimeDate.ReadState = Send(this->Address, (byte) 70, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  if (TimeDate.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  TimeDate.TransmissionState = ReceiveData[0];
  TimeDate.GlobalState = ReceiveData[1];
  TimeDate.Seconds = ((unsigned long) ReceiveData[2] << 24) + ((unsigned long) ReceiveData[3] << 16) +
                     ((unsigned long) ReceiveData[4] << 8) + (unsigned long) ReceiveData[5];
  return TimeDate.ReadState;
}

bool ABBAurora::ReadLastFourAlarms() {
  LastFourAlarms.ReadState = Send(this->Address, (byte) 86, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  if (LastFourAlarms.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
    ReceiveData[2] = 255;
    ReceiveData[3] = 255;
    ReceiveData[4] = 255;
    ReceiveData[5] = 255;
  }

  LastFourAlarms.TransmissionState = ReceiveData[0];
  LastFourAlarms.GlobalState = ReceiveData[1];
  LastFourAlarms.Alarms1 = ReceiveData[2];
  LastFourAlarms.Alarms2 = ReceiveData[3];
  LastFourAlarms.Alarms3 = ReceiveData[4];
  LastFourAlarms.Alarms4 = ReceiveData[5];

  return LastFourAlarms.ReadState;
}

bool ABBAurora::ReadJunctionBoxState(byte nj) {
  return Send(this->Address, (byte) 200, nj, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadJunctionBoxVal(byte nj, byte par) {
  return Send(this->Address, (byte) 201, nj, par, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

// Inverters

bool ABBAurora::ReadSystemPN() {
  SystemPN.ReadState = Send(this->Address, (byte) 52, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  SystemPN.PN = String(String((char) ReceiveData[0]) + String((char) ReceiveData[1]) + String((char) ReceiveData[2]) +
                       String((char) ReceiveData[3]) + String((char) ReceiveData[4]) + String((char) ReceiveData[5]));

  return SystemPN.ReadState;
}

bool ABBAurora::ReadSystemSerialNumber() {
  SystemSerialNumber.ReadState =
      Send(this->Address, (byte) 63, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  SystemSerialNumber.SerialNumber =
      String(String((char) ReceiveData[0]) + String((char) ReceiveData[1]) + String((char) ReceiveData[2]) +
             String((char) ReceiveData[3]) + String((char) ReceiveData[4]) + String((char) ReceiveData[5]));

  return SystemSerialNumber.ReadState;
}

bool ABBAurora::ReadManufacturingWeekYear() {
  ManufacturingWeekYear.ReadState =
      Send(this->Address, (byte) 65, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  if (ManufacturingWeekYear.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  ManufacturingWeekYear.TransmissionState = ReceiveData[0];
  ManufacturingWeekYear.GlobalState = ReceiveData[1];
  ManufacturingWeekYear.Week = String(String((char) ReceiveData[2]) + String((char) ReceiveData[3]));
  ManufacturingWeekYear.Year = String(String((char) ReceiveData[4]) + String((char) ReceiveData[5]));

  return ManufacturingWeekYear.ReadState;
}

bool ABBAurora::ReadFirmwareRelease() {
  FirmwareRelease.ReadState =
      Send(this->Address, (byte) 72, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  if (FirmwareRelease.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  FirmwareRelease.TransmissionState = ReceiveData[0];
  FirmwareRelease.GlobalState = ReceiveData[1];
  FirmwareRelease.Release = String(String((char) ReceiveData[2]) + "." + String((char) ReceiveData[3]) + "." +
                                   String((char) ReceiveData[4]) + "." + String((char) ReceiveData[5]));

  return FirmwareRelease.ReadState;
}

bool ABBAurora::ReadCumulatedEnergy(CUMULATED_ENERGY_TYPE par) {
  if ((int) par >= 0 && (int) par <= 6)
    return finishCumulatedEnergy(Send(this->Address, (byte) 78, par, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0));

  clearReceiveData();
  return finishCumulatedEnergy(false);
}

bool ABBAurora::finishCumulatedEnergy(bool ok) {
  CumulatedEnergy.ReadState = ok;

  if (CumulatedEnergy.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  CumulatedEnergy.TransmissionState = ReceiveData[0];
  CumulatedEnergy.GlobalState = ReceiveData[1];
  if (CumulatedEnergy.ReadState == true) {
    ulo.asBytes[0] = ReceiveData[5];
    ulo.asBytes[1] = ReceiveData[4];
    ulo.asBytes[2] = ReceiveData[3];
    ulo.asBytes[3] = ReceiveData[2];

    CumulatedEnergy.Energy = ulo.asUlong;
  }
  return CumulatedEnergy.ReadState;
}

bool ABBAurora::WriteBaudRateSetting(byte baudcode) {
  if (baudcode <= 3) {
    return Send(this->Address, (byte) 85, baudcode, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
  } else {
    clearReceiveData();
    return false;
  }
}

// Central
bool ABBAurora::ReadFlagsSwitchCentral() {
  return Send(this->Address, (byte) 67, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadCumulatedEnergyCentral(byte var, byte ndays_h, byte ndays_l, byte global) {
  return Send(this->Address, (byte) 68, var, ndays_h, ndays_l, global, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadFirmwareReleaseCentral(byte var) {
  return Send(this->Address, (byte) 72, var, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadBaudRateSettingCentral(byte baudcode, byte serialline) {
  return Send(this->Address, (byte) 85, baudcode, serialline, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadSystemInfoCentral(byte var) {
  return Send(this->Address, (byte) 101, var, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadJunctionBoxMonitoringCentral(byte cf, byte rn, byte njt, byte jal, byte jah) {
  return Send(this->Address, (byte) 103, cf, rn, njt, jal, jah, (byte) 0);
}

bool ABBAurora::ReadSystemPNCentral() {
  return Send(this->Address, (byte) 105, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadSystemSerialNumberCentral() {
  return Send(this->Address, (byte) 107, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);
}

bool ABBAurora::ReadState() {
  return finishState(Send(this->Address, (byte) 50, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0));
}

bool ABBAurora::finishState(bool ok) {
  State.ReadState = ok;

  if (State.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
    ReceiveData[2] = 255;
    ReceiveData[3] = 255;
    ReceiveData[4] = 255;
    ReceiveData[5] = 255;
  }

  State.TransmissionState = ReceiveData[0];
  State.GlobalState = ReceiveData[1];
  State.InverterState = ReceiveData[2];
  State.Channel1State = ReceiveData[3];
  State.Channel2State = ReceiveData[4];
  State.AlarmState = ReceiveData[5];

  return State.ReadState;
}

bool ABBAurora::ReadVersion() {
  Version.ReadState = Send(this->Address, (byte) 58, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0, (byte) 0);

  if (Version.ReadState == false) {
    ReceiveData[0] = 255;
    ReceiveData[1] = 255;
  }

  Version.TransmissionState = ReceiveData[0];
  Version.GlobalState = ReceiveData[1];

  Version.Par1 = ABBAuroraStrings::VersionPart1(ReceiveData[2]);
  Version.Par2 = ABBAuroraStrings::VersionPart2(ReceiveData[3]);
  Version.Par3 = ABBAuroraStrings::VersionPart3(ReceiveData[4]);
  Version.Par4 = ABBAuroraStrings::VersionPart4(ReceiveData[5]);

  return Version.ReadState;
}

}  // namespace aurora_monitor
}  // namespace esphome
