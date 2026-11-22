/*
 * Human-readable descriptions of Aurora inverter states and alarms.
 *
 * Derived from the ABBAurora Arduino library by jrbenito
 * (https://github.com/jrbenito/ABBAurora), which is based on the original
 * Arduino sketches by Davide Rosa (http://www.drhack.it).
 *
 * Modified by Daniele Vergini for the aurora_monitor ESPHome component:
 *  - moved into the esphome::aurora_monitor namespace.
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

#ifndef ABBAuroraStrings_h
#define ABBAuroraStrings_h
#include <Arduino.h>

namespace esphome {
namespace aurora_monitor {

class ABBAuroraStrings {
 private:
 public:
  static String TransmissionState(byte id);
  static String GlobalState(byte id);
  static String DcDcState(byte id);
  static String InverterState(byte id);
  static String AlarmState(byte id);
  static String VersionPart1(byte id);
  static String VersionPart2(byte id);
  static String VersionPart3(byte id);
  static String VersionPart4(byte id);
};

}  // namespace aurora_monitor
}  // namespace esphome

#endif
