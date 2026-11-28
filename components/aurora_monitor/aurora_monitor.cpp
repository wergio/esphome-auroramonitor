#include "aurora_monitor.h"

#include <cmath>
#include <cstring>

#include "ABBAuroraStrings.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aurora_monitor {

static const char *const TAG = "aurora_monitor";

static const char *const STATUS_CONNECTED = "CONNECTED";
static const char *const STATUS_DISCONNECTED = "DISCONNECTED";
// State and alarm text while the inverter does not answer.
static const char *const STATE_OFFLINE = "Offline";

// Pause between two inverters of the same polling cycle.
static const uint32_t INTER_INVERTER_DELAY_MS = 100;
// Pause between closing and reopening the serial port after a missed answer.
static const uint32_t REOPEN_DELAY_MS = 1000;
// Time given to the serial port to settle after it has been opened.
static const uint32_t SERIAL_SETTLE_MS = 500;
// Minimum input (DC) power for the efficiency: below it, about 2-5% of the
// nominal power of a residential Aurora inverter, the ratio is dominated by
// the inverter's own consumption (some tens of watts) and by measurement noise.
static const float EFFICIENCY_MIN_INPUT_POWER = 100.0f;
// Duration of the pulse on the reset pin.
static const uint32_t RESET_PULSE_MS = 2000;
// How long to wait at boot for the clock before restoring the saved values
// without checking whether their day, week, month or year is over.
static const uint32_t RESTORE_WAIT_MS = 60000;

// Command of each InverterExtra request.
// How each InverterExtra is requested.
struct ExtraSpec {
  uint8_t command;
  uint8_t param;
  bool hourly;  // also read every hour while the inverter is connected
};

static const ExtraSpec EXTRA_SPECS[EXTRA_COUNT] = {
    {58, 0, false},                    // model
    {52, 0, false},                    // part number
    {63, 0, false},                    // serial number
    {65, 0, false},                    // manufacturing week and year
    {72, 0, false},                    // firmware release
    {70, 0, true},                     // clock
    {86, 0, false},                    // last four alarms
    {59, ISOLATION_RESISTANCE, true},  // isolation resistance (Riso)
    {59, I_LEAK_DC_DC, true},          // leakage current of the DC/DC stage
    {59, I_LEAK_INVERTER, true},       // leakage current of the inverter stage
};

// Interval of the hourly extra requests.
static const uint32_t HOURLY_INTERVAL_MS = 3600000;

// Key of the saved alarm history, combined with the inverter address.
static const char *const ALARM_HISTORY_KEY = "aurora_monitor_alarm_history_v1";

// The inverter clock counts the seconds since 2000-01-01 00:00 (local time).
static const int32_t CLOCK_EPOCH_DAYS = 10957;  // days from 1970-01-01 to 2000-01-01

// Key of the saved values, combined with the inverter address.
static const char *const SAVED_VALUES_KEY = "aurora_monitor_saved_values_v3";

// Hardware UART used for the RS485 bus. On ESP8266 it is UART0, either on its
// default pins or swapped to GPIO13/GPIO15 (not connected to the USB adapter).
static HardwareSerial &rs485_serial() {
#ifdef USE_ESP32
  return Serial2;
#else
  return Serial;
#endif
}

// How each InverterReading is requested from the inverter.
struct ReadingSpec {
  bool energy;  // true: cumulated energy counter, false: DSP measurement
  uint8_t code;
};

static const ReadingSpec READING_SPECS[READING_COUNT] = {
    {false, V_IN_1},
    {false, V_IN_2},
    {false, I_IN_1},
    {false, I_IN_2},
    {false, POWER_IN_1},
    {false, POWER_IN_2},
    {false, GRID_POWER},
    {false, GRID_VOLTAGE},
    {false, GRID_CURRENT},
    {false, FREQUENCY},
    {false, POWER_PEAK_TODAY},
    {false, POWER_PEAK},
    {false, TEMPERATURE_INVERTER},
    {false, TEMPERATURE_BOOSTER},
    {true, CURRENT_DAY},
    {true, CURRENT_WEEK},
    {true, CURRENT_MONTH},
    {true, CURRENT_YEAR},
    {true, TOTAL},
};

// Instantaneous values, cleared when an inverter stops answering (e.g. at
// night). Energy counters and power peaks keep their last value.
static const InverterReading INSTANT_READINGS[] = {
    READING_V_IN_1,
    READING_V_IN_2,
    READING_I_IN_1,
    READING_I_IN_2,
    READING_POWER_IN_1,
    READING_POWER_IN_2,
    READING_GRID_POWER,
    READING_GRID_VOLTAGE,
    READING_GRID_CURRENT,
    READING_GRID_FREQUENCY,
    READING_TEMPERATURE_INVERTER,
    READING_TEMPERATURE_BOOSTER,
};

// Whether a reading can be published. Right after switching on, before its
// DSP has started, the inverter answers with values that are not measures:
// zero, or bytes that are not a number. Zero is discarded only for the
// readings that cannot be zero while the inverter answers (it is powered by
// its DC inputs and cannot run without the grid); currents and powers are
// really zero while it does not feed the grid.
static bool is_valid_reading(InverterReading reading, float value) {
  if (!std::isfinite(value))
    return false;
  switch (reading) {
    case READING_V_IN_1:
    case READING_V_IN_2:
    case READING_GRID_VOLTAGE:
    case READING_GRID_FREQUENCY:
    case READING_TEMPERATURE_INVERTER:
    case READING_TEMPERATURE_BOOSTER:
      return value != 0.0f;
    default:
      return true;
  }
}

// Values saved to flash and published again after a reboot, so that they
// stay visible in Home Assistant even if the device restarts at night.
static const InverterReading SAVED_READINGS[] = {
    READING_POWER_PEAK_TODAY, READING_POWER_PEAK_MAX, READING_ENERGY_TODAY, READING_ENERGY_WEEK,
    READING_ENERGY_MONTH,     READING_ENERGY_YEAR,    READING_ENERGY_TOTAL,
};
static_assert(sizeof(SAVED_READINGS) / sizeof(SAVED_READINGS[0]) == SAVED_READING_COUNT,
              "SAVED_READING_COUNT does not match SAVED_READINGS");

// Days since 1970-01-01 for a civil date (proleptic Gregorian calendar).
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = (uint32_t) (y - era * 400);
  const uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t) doe - 719468;
}

// Year and month for a number of days since 1970-01-01.
static void civil_from_days(int32_t z, int32_t *year, uint32_t *month) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = (uint32_t) (z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  const uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  *month = m;
  *year = (int32_t) yoe + era * 400 + (m <= 2 ? 1 : 0);
}

// Index of the week (starting on Monday) for a number of days since 1970-01-01.
static int32_t week_from_days(int32_t z) {
  // 1970-01-01 was a Thursday.
  return (z + 3) / 7;
}

void AuroraMonitor::add_inverter(uint8_t address, const std::string &prefix, uint8_t mppt_inputs) {
  Inverter inv;
  inv.address = address;
  inv.mppt_inputs = mppt_inputs;
  inv.name = prefix.empty() ? "address " + std::to_string(address) : prefix;
  for (float &value : inv.last_values)
    value = NAN;
  this->inverters_.push_back(inv);
}

void AuroraMonitor::setup() {
  // The activity LED stays on until the first polling cycle is complete.
  if (this->activity_led_ != nullptr)
    this->activity_led_->setup();
  this->set_activity_led_(true);

  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(false);
  }

  this->inverter_.SetTiming(this->tx_enable_delay_ms_, this->response_timeout_ms_);
  this->open_serial_();

  for (Inverter &inv : this->inverters_) {
    // Request only the readings that something uses.
    for (uint8_t r = 0; r < READING_COUNT; r++)
      inv.needed[r] = inv.sensors[r] != nullptr;
    if (inv.power_in_total != nullptr || inv.efficiency != nullptr)
      inv.needed[READING_POWER_IN_1] = inv.needed[READING_POWER_IN_2] = true;
    if (inv.efficiency != nullptr)
      inv.needed[READING_GRID_POWER] = true;
    // A single-input inverter has nothing to report on channel 2.
    if (inv.mppt_inputs == 1)
      inv.needed[READING_V_IN_2] = inv.needed[READING_I_IN_2] = inv.needed[READING_POWER_IN_2] = false;

    publish_(inv.connection_status, STATUS_DISCONNECTED);
    this->load_saved_values_(inv);

    // Alarm history kept across reboots.
    const uint32_t alarms_key = fnv1_hash(std::string(ALARM_HISTORY_KEY) + "_" + std::to_string(inv.address));
    inv.alarms_pref = global_preferences->make_preference<SavedAlarms>(alarms_key, true);
    SavedAlarms saved_alarms;
    if (inv.alarms_pref.load(&saved_alarms) && saved_alarms.size <= ALARM_HISTORY_SIZE) {
      memcpy(inv.alarm_history, saved_alarms.codes, sizeof(inv.alarm_history));
      inv.alarm_history_size = saved_alarms.size;
      if (inv.alarm_history_size > 0)
        this->publish_alarm_history_(inv);
    }
  }
  this->setup_ms_ = millis();
  // Values whose period may be over wait for the clock.
  this->check_day_change_();

  this->next_action_ms_ = millis() + SERIAL_SETTLE_MS;
}

void AuroraMonitor::dump_config() {
  ESP_LOGCONFIG(TAG, "Aurora Monitor:");
  for (const Inverter &inv : this->inverters_)
    ESP_LOGCONFIG(TAG, "  Inverter %s: RS485 address %u, %u MPPT input(s)", inv.name.c_str(), inv.address,
                  inv.mppt_inputs);
  ESP_LOGCONFIG(TAG, "  UART pins: RX GPIO%u, TX GPIO%u", this->rx_pin_, this->tx_pin_);
  LOG_PIN("  TX control pin: ", this->tx_control_pin_);
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  LOG_PIN("  Activity LED pin: ", this->activity_led_);
  ESP_LOGCONFIG(TAG, "  TX enable delay: %u ms, response timeout: %u ms", (unsigned) this->tx_enable_delay_ms_,
                (unsigned) this->response_timeout_ms_);
  LOG_UPDATE_INTERVAL(this);
}

void AuroraMonitor::update() {
  this->check_day_change_();

  if (this->state_ != PollState::IDLE) {
    ESP_LOGW(TAG, "Previous polling cycle still running, skipping this update");
    return;
  }
  if (this->inverters_.empty())
    return;
  this->inverter_index_ = 0;
  this->state_ = PollState::PROBE;
  // Run loop() continuously during the cycle, so that answers are picked up
  // as soon as they arrive and response times are measured accurately.
  this->high_freq_.start();
}

void AuroraMonitor::loop() {
  if (this->state_ == PollState::IDLE)
    return;
  if ((int32_t) (millis() - this->next_action_ms_) < 0)
    return;

  if (this->state_ == PollState::REOPEN) {
    this->reopen_serial_();
    return;
  }

  Inverter &inv = this->inverters_[this->inverter_index_];

  bool ok;
  if (!this->request_active_) {
    if (this->start_request_(inv)) {
      this->request_active_ = true;
      return;
    }
    ok = false;  // the request could not be started
  } else {
    const RequestStatus status = this->inverter_.Poll();
    if (status == REQUEST_PENDING)
      return;
    this->request_active_ = false;
    // No valid answer (timeout, incomplete answer, CRC error): try again. The
    // state request is repeated only while the inverter is connected, so that
    // a single lost answer does not mark it as disconnected, while an inverter
    // that is off costs a single timeout per cycle.
    if (status == REQUEST_FAILED && (this->state_ != PollState::PROBE || inv.connected) &&
        this->attempts_ < this->retries_) {
      this->attempts_++;
      ESP_LOGD(TAG, "Inverter %s: no valid answer, retrying (%u/%u)", inv.name.c_str(), this->attempts_,
               this->retries_);
      return;
    }
    this->attempts_ = 0;
    ok = status == REQUEST_OK;
    if (ok) {
      this->record_response_time_(this->inverter_.LastResponseMs);
      // A valid frame can still report an error, e.g. a value that this
      // model does not provide: its data bytes are then meaningless.
      const uint8_t transmission_state = this->transmission_state_();
      if (transmission_state != 0) {
        ESP_LOGD(TAG, "Inverter %s: request refused: %s (%u)", inv.name.c_str(),
                 ABBAuroraStrings::TransmissionState(transmission_state).c_str(), transmission_state);
        // The inverter answered, so it is connected: only the value is missing.
        if (this->state_ != PollState::PROBE)
          ok = false;
      }
    }
  }

  if (this->state_ == PollState::PROBE) {
    this->on_probe_done_(inv, ok);
  } else if (this->state_ == PollState::READ) {
    this->on_reading_done_(inv, ok);
  } else {
    this->on_extra_done_(inv, ok);
  }
}

bool AuroraMonitor::start_request_(Inverter &inv) {
  this->inverter_.Address = inv.address;

  if (this->state_ == PollState::PROBE) {
    this->responses_ = 0;
    return this->inverter_.BeginReadState();
  }
  if (this->state_ == PollState::EXTRA)
    return this->inverter_.BeginRawRequest(EXTRA_SPECS[this->extra_index_].command,
                                           EXTRA_SPECS[this->extra_index_].param);

  const ReadingSpec &spec = READING_SPECS[this->reading_index_];
  if (spec.energy)
    return this->inverter_.BeginReadCumulatedEnergy((CUMULATED_ENERGY_TYPE) spec.code);
  return this->inverter_.BeginReadDSPValue((DSP_VALUE_TYPE) spec.code, MODULE_MESSUREMENT);
}

void AuroraMonitor::on_probe_done_(Inverter &inv, bool ok) {
  if (ok) {
    if (!inv.connected) {
      inv.connected = true;
      ESP_LOGI(TAG, "Inverter %s (address %u) connected", inv.name.c_str(), inv.address);
      publish_(inv.connection_status, STATUS_CONNECTED);
      // At boot and every morning: read the information, clock and alarms.
      for (uint8_t e = 0; e < EXTRA_COUNT; e++) {
        if (has_extra_sensor_(inv, (InverterExtra) e))
          inv.extras_pending |= 1 << e;
      }
      inv.last_hourly_ms = millis();
    } else if (millis() - inv.last_hourly_ms >= HOURLY_INTERVAL_MS) {
      for (uint8_t e = 0; e < EXTRA_COUNT; e++) {
        if (EXTRA_SPECS[e].hourly && has_extra_sensor_(inv, (InverterExtra) e))
          inv.extras_pending |= 1 << e;
      }
      inv.last_hourly_ms = millis();
    }
    if (this->inverter_.State.TransmissionState == 0) {
      publish_if_changed_(inv.global_state, ABBAuroraStrings::GlobalState(this->inverter_.State.GlobalState).c_str());
      publish_if_changed_(inv.alarm_state, ABBAuroraStrings::AlarmState(this->inverter_.State.AlarmState).c_str());
      publish_if_changed_(inv.inverter_state,
                          ABBAuroraStrings::InverterState(this->inverter_.State.InverterState).c_str());
      publish_if_changed_(inv.input_states[0],
                          ABBAuroraStrings::DcDcState(this->inverter_.State.Channel1State).c_str());
      publish_if_changed_(inv.input_states[1],
                          ABBAuroraStrings::DcDcState(this->inverter_.State.Channel2State).c_str());
      // A new alarm: collect it from the inverter's alarm queue.
      if (this->inverter_.State.AlarmState != inv.last_alarm_state) {
        inv.last_alarm_state = this->inverter_.State.AlarmState;
        if (has_extra_sensor_(inv, EXTRA_LAST_ALARMS))
          inv.extras_pending |= 1 << EXTRA_LAST_ALARMS;
      }
    }

    this->set_activity_led_(true);
    this->power_in_1_ = NAN;
    this->power_in_2_ = NAN;
    this->grid_power_ = NAN;
    this->reading_index_ = 0;
    this->state_ = PollState::READ;
    if (!this->skip_to_needed_reading_(inv))
      this->finish_readings_(inv);
    return;
  }

  if (inv.connected) {
    inv.connected = false;
    // Normally the inverter switching off in the evening, not a fault.
    ESP_LOGI(TAG, "Inverter %s (address %u) stopped answering", inv.name.c_str(), inv.address);
    publish_(inv.connection_status, STATUS_DISCONNECTED);
    this->clear_instant_values_(inv);
    // Typically the inverter switching off in the evening: save the final
    // values of the day, so that they can be restored after a reboot.
    this->save_values_(inv);
  } else {
    ESP_LOGD(TAG, "Inverter %s (address %u) not responding", inv.name.c_str(), inv.address);
  }

  ABBAurora::disconnect();
  this->next_action_ms_ = millis() + REOPEN_DELAY_MS;
  this->state_ = PollState::REOPEN;
}

void AuroraMonitor::on_reading_done_(Inverter &inv, bool ok) {
  const InverterReading reading = (InverterReading) this->reading_index_;

  const float value =
      READING_SPECS[reading].energy ? (float) this->inverter_.CumulatedEnergy.Energy : this->inverter_.DSP.Value;
  if (ok && !is_valid_reading(reading, value)) {
    // Not published: Home Assistant keeps the previous state.
    ESP_LOGD(TAG, "Inverter %s: reading %u discarded (%f, global state %u)", inv.name.c_str(), this->reading_index_,
             value, this->inverter_.DSP.GlobalState);
  } else if (ok) {
    inv.last_values[reading] = value;
    inv.read_since_boot = true;
    publish_(inv.sensors[reading], value);
    if (reading == READING_POWER_IN_1) {
      this->power_in_1_ = value;
    } else if (reading == READING_POWER_IN_2) {
      this->power_in_2_ = value;
    } else if (reading == READING_GRID_POWER) {
      this->grid_power_ = value;
    }
  } else {
    ESP_LOGD(TAG, "Inverter %s: reading %u failed", inv.name.c_str(), this->reading_index_);
  }

  this->reading_index_++;
  if (!this->skip_to_needed_reading_(inv))
    this->finish_readings_(inv);
}

uint8_t AuroraMonitor::transmission_state_() const {
  if (this->state_ == PollState::PROBE)
    return this->inverter_.State.TransmissionState;
  if (this->state_ == PollState::EXTRA) {
    // The P/N and serial number answers are six characters, with no state.
    if (this->extra_index_ == EXTRA_PART_NUMBER || this->extra_index_ == EXTRA_SERIAL_NUMBER)
      return 0;
    return this->inverter_.ReceiveData[0];
  }
  if (READING_SPECS[this->reading_index_].energy)
    return this->inverter_.CumulatedEnergy.TransmissionState;
  return this->inverter_.DSP.TransmissionState;
}

float AuroraMonitor::input_power_(const Inverter &inv) const {
  // Both inputs must come from this cycle; a single-input inverter only has
  // channel 1.
  if (inv.mppt_inputs == 1)
    return this->power_in_1_;
  if (std::isnan(this->power_in_1_) || std::isnan(this->power_in_2_))
    return NAN;
  return this->power_in_1_ + this->power_in_2_;
}

bool AuroraMonitor::skip_to_needed_reading_(Inverter &inv) {
  while (this->reading_index_ < READING_COUNT && !inv.needed[this->reading_index_])
    this->reading_index_++;
  return this->reading_index_ < READING_COUNT;
}

void AuroraMonitor::finish_readings_(Inverter &inv) {
  // Publish the total only when every input was read in this cycle.
  const float input_power = this->input_power_(inv);
  if (!std::isnan(input_power))
    publish_(inv.power_in_total, input_power);
  this->publish_efficiency_(inv);

  this->state_ = PollState::EXTRA;
  this->extra_index_ = 0;
  if (!this->skip_to_pending_extra_(inv))
    this->end_inverter_(inv);
}

bool AuroraMonitor::has_extra_sensor_(const Inverter &inv, InverterExtra extra) {
  return inv.extra_texts[extra] != nullptr || inv.extra_values[extra] != nullptr;
}

bool AuroraMonitor::skip_to_pending_extra_(Inverter &inv) {
  while (this->extra_index_ < EXTRA_COUNT && !(inv.extras_pending & (1 << this->extra_index_)))
    this->extra_index_++;
  return this->extra_index_ < EXTRA_COUNT;
}

void AuroraMonitor::on_extra_done_(Inverter &inv, bool ok) {
  const InverterExtra extra = (InverterExtra) this->extra_index_;
  // Done either way: a failed request is retried at the next connection.
  inv.extras_pending &= ~(1 << extra);
  if (ok) {
    this->decode_extra_(inv, extra);
  } else {
    ESP_LOGD(TAG, "Inverter %s: extra request %u/%u failed", inv.name.c_str(), EXTRA_SPECS[extra].command,
             EXTRA_SPECS[extra].param);
  }

  this->extra_index_++;
  if (!this->skip_to_pending_extra_(inv))
    this->end_inverter_(inv);
}

// Printable characters of an ASCII answer, without padding.
static std::string ascii_field(const byte *data, uint8_t length) {
  std::string text;
  for (uint8_t i = 0; i < length; i++) {
    if (data[i] > ' ' && data[i] < 127)
      text += (char) data[i];
    else if (data[i] == ' ' && !text.empty())
      text += ' ';
  }
  while (!text.empty() && text.back() == ' ')
    text.pop_back();
  return text;
}

void AuroraMonitor::decode_extra_(Inverter &inv, InverterExtra extra) {
  const byte *data = this->inverter_.ReceiveData;
  text_sensor::TextSensor *text = inv.extra_texts[extra];
  sensor::Sensor *value = inv.extra_values[extra];

  switch (extra) {
    case EXTRA_MODEL: {
      std::string model = ABBAuroraStrings::VersionPart1(data[2]).c_str();
      model += ", ";
      model += ABBAuroraStrings::VersionPart2(data[3]).c_str();
      model += ", ";
      model += ABBAuroraStrings::VersionPart3(data[4]).c_str();
      model += ", ";
      model += ABBAuroraStrings::VersionPart4(data[5]).c_str();
      publish_if_changed_(text, model);
      break;
    }
    case EXTRA_PART_NUMBER:
    case EXTRA_SERIAL_NUMBER:
      publish_if_changed_(text, ascii_field(data, 6));
      break;
    case EXTRA_MANUFACTURING:
      // Two-digit year and week, e.g. "2009-W23".
      publish_if_changed_(text, "20" + ascii_field(data + 4, 2) + "-W" + ascii_field(data + 2, 2));
      break;
    case EXTRA_FIRMWARE: {
      std::string release;
      for (uint8_t i = 2; i < 6; i++) {
        if (i > 2)
          release += '.';
        release += (char) data[i];
      }
      publish_if_changed_(text, release);
      break;
    }
    case EXTRA_CLOCK: {
      const uint32_t seconds =
          ((uint32_t) data[2] << 24) | ((uint32_t) data[3] << 16) | ((uint32_t) data[4] << 8) | (uint32_t) data[5];
      // Difference from the local time of Home Assistant, in seconds.
      const ESPTime now = this->time_->now();
      if (now.is_valid()) {
        const int64_t local_seconds =
            (int64_t) (days_from_civil(now.year, now.month, now.day_of_month) - CLOCK_EPOCH_DAYS) * 86400 +
            now.hour * 3600 + now.minute * 60 + now.second;
        publish_(value, (float) ((int64_t) seconds - local_seconds));
      } else {
        // Typically right after boot, before Home Assistant has sent the
        // time: read the clock again in the next cycle.
        inv.extras_pending |= 1 << EXTRA_CLOCK;
      }
      break;
    }
    case EXTRA_LAST_ALARMS: {
      // AL1 is the oldest alarm, AL4 the most recent; reading empties the queue.
      for (uint8_t i = 2; i < 6; i++) {
        if (data[i] == 0)
          continue;
        for (uint8_t h = ALARM_HISTORY_SIZE - 1; h > 0; h--)
          inv.alarm_history[h] = inv.alarm_history[h - 1];
        inv.alarm_history[0] = data[i];
        if (inv.alarm_history_size < ALARM_HISTORY_SIZE)
          inv.alarm_history_size++;
      }
      bool changed = false;
      for (uint8_t i = 2; i < 6; i++)
        changed |= data[i] != 0;
      if (changed) {
        SavedAlarms saved;
        memcpy(saved.codes, inv.alarm_history, sizeof(saved.codes));
        saved.size = inv.alarm_history_size;
        // Written to flash at the next preferences sync; alarms are rare.
        inv.alarms_pref.save(&saved);
      }
      this->publish_alarm_history_(inv);
      break;
    }
    case EXTRA_ISOLATION_RESISTANCE:
    case EXTRA_LEAKAGE_CURRENT_DC_DC:
    case EXTRA_LEAKAGE_CURRENT_INVERTER: {
      // DSP measure: big-endian IEEE 754 float, as for command 59 readings.
      union {
        uint8_t bytes[4];
        float value;
      } measure;
      measure.bytes[0] = data[5];
      measure.bytes[1] = data[4];
      measure.bytes[2] = data[3];
      measure.bytes[3] = data[2];
      if (!std::isfinite(measure.value)) {
        ESP_LOGD(TAG, "Inverter %s: extra request %u/%u discarded (not a number)", inv.name.c_str(),
                 EXTRA_SPECS[extra].command, EXTRA_SPECS[extra].param);
        break;
      }
      publish_(value, measure.value);
      break;
    }
    default:
      break;
  }
}

void AuroraMonitor::publish_alarm_history_(Inverter &inv) {
  text_sensor::TextSensor *text = inv.extra_texts[EXTRA_LAST_ALARMS];
  if (inv.alarm_history_size == 0) {
    if (text != nullptr && !text->has_state())
      publish_(text, "None");
    return;
  }
  std::string alarms;
  for (uint8_t h = 0; h < inv.alarm_history_size; h++) {
    if (h > 0)
      alarms += ", ";
    alarms += ABBAuroraStrings::AlarmState(inv.alarm_history[h]).c_str();
  }
  publish_if_changed_(text, alarms);
}

void AuroraMonitor::end_inverter_(Inverter &inv) {
  if (this->responses_ > 0) {
    ESP_LOGD(TAG, "Inverter %s: %u answers, response time min %u ms, max %u ms", inv.name.c_str(), this->responses_,
             (unsigned) this->response_min_ms_, (unsigned) this->response_max_ms_);
  }

  uint32_t today;
  if (this->current_day_(&today))
    inv.values_day = today;

  if (this->initialized_)
    this->set_activity_led_(false);
  this->next_inverter_(INTER_INVERTER_DELAY_MS);
}

void AuroraMonitor::publish_efficiency_(Inverter &inv) {
  if (inv.efficiency == nullptr)
    return;
  // Input and grid powers must come from this cycle: mixing cycles would
  // compare values taken up to update_interval apart.
  const float input_power = this->input_power_(inv);
  if (std::isnan(input_power) || std::isnan(this->grid_power_))
    return;

  // At low power (dawn, dusk, clouds) the ratio is not meaningful.
  if (input_power < EFFICIENCY_MIN_INPUT_POWER) {
    publish_(inv.efficiency, NAN);
    return;
  }

  const float efficiency = this->grid_power_ / input_power * 100.0f;
  // The input and grid powers are read a few tens of milliseconds apart: a
  // sudden change of irradiance in between can give impossible values.
  if (efficiency <= 0.0f || efficiency > 100.0f) {
    ESP_LOGD(TAG, "Inverter %s: efficiency %.1f%% discarded (input %.0f W, grid %.0f W)", inv.name.c_str(), efficiency,
             input_power, this->grid_power_);
    return;
  }
  publish_(inv.efficiency, efficiency);
}

void AuroraMonitor::record_response_time_(uint32_t ms) {
  if (this->responses_ == 0 || ms < this->response_min_ms_)
    this->response_min_ms_ = ms;
  if (this->responses_ == 0 || ms > this->response_max_ms_)
    this->response_max_ms_ = ms;
  this->responses_++;
}

void AuroraMonitor::open_serial_() {
  ABBAurora::setup(rs485_serial(), this->rx_pin_, this->tx_pin_, this->tx_control_pin_);
}

void AuroraMonitor::reopen_serial_() {
  this->open_serial_();
  this->next_inverter_(SERIAL_SETTLE_MS);
}

void AuroraMonitor::next_inverter_(uint32_t delay_ms) {
  this->next_action_ms_ = millis() + delay_ms;
  this->inverter_index_++;
  if (this->inverter_index_ < this->inverters_.size()) {
    this->state_ = PollState::PROBE;
    return;
  }

  this->inverter_index_ = 0;
  this->state_ = PollState::IDLE;
  this->high_freq_.stop();
  if (!this->initialized_) {
    this->initialized_ = true;
    this->set_activity_led_(false);
  }
}

void AuroraMonitor::clear_instant_values_(Inverter &inv) {
  for (InverterReading reading : INSTANT_READINGS)
    publish_(inv.sensors[reading], NAN);
  publish_(inv.power_in_total, NAN);
  publish_(inv.efficiency, NAN);
  publish_if_changed_(inv.global_state, STATE_OFFLINE);
  publish_if_changed_(inv.alarm_state, STATE_OFFLINE);
  publish_if_changed_(inv.inverter_state, STATE_OFFLINE);
  publish_if_changed_(inv.input_states[0], STATE_OFFLINE);
  publish_if_changed_(inv.input_states[1], STATE_OFFLINE);
  // The leakage currents are live values; the isolation resistance, measured
  // when the inverter starts, keeps its last value.
  publish_(inv.extra_values[EXTRA_LEAKAGE_CURRENT_DC_DC], NAN);
  publish_(inv.extra_values[EXTRA_LEAKAGE_CURRENT_INVERTER], NAN);
}

void AuroraMonitor::load_saved_values_(Inverter &inv) {
  const uint32_t key = fnv1_hash(std::string(SAVED_VALUES_KEY) + "_" + std::to_string(inv.address));
  inv.saved_values_pref = global_preferences->make_preference<SavedValues>(key, true);

  if (!inv.saved_values_pref.load(&inv.restored)) {
    ESP_LOGD(TAG, "Inverter %s: no saved values to restore", inv.name.c_str());
    return;
  }
  inv.has_restored = true;
  this->restore_pending_ = true;

  // Lifetime values never reset: publish them straight away.
  for (uint8_t s = 0; s < SAVED_READING_COUNT; s++) {
    const InverterReading reading = SAVED_READINGS[s];
    if (reading != READING_ENERGY_TOTAL && reading != READING_POWER_PEAK_MAX)
      continue;
    const float value = inv.restored.values[s];
    if (std::isnan(value))
      continue;
    inv.last_values[reading] = value;
    publish_(inv.sensors[reading], value);
  }
}

void AuroraMonitor::apply_restored_values_() {
  this->restore_pending_ = false;

  uint32_t today = 0;
  const bool date_known = this->current_day_(&today);
  if (!date_known)
    ESP_LOGW(TAG, "Current date unknown, restoring saved values without checking their date");

  for (Inverter &inv : this->inverters_) {
    // Nothing saved, or already read since boot: the inverter values are more recent.
    if (!inv.has_restored || inv.connected || inv.read_since_boot)
      continue;

    for (uint8_t s = 0; s < SAVED_READING_COUNT; s++) {
      const float value = inv.restored.values[s];
      if (!std::isnan(value))
        inv.last_values[SAVED_READINGS[s]] = value;
    }
    inv.values_day = inv.restored.day;

    if (date_known && inv.values_day != 0 && inv.values_day != today) {
      this->reset_period_values_(inv, inv.values_day, today);
      inv.values_day = today;
    }

    for (InverterReading reading : SAVED_READINGS) {
      if (!std::isnan(inv.last_values[reading]))
        publish_(inv.sensors[reading], inv.last_values[reading]);
    }
    ESP_LOGI(TAG, "Inverter %s: restored saved energy counters and power peaks", inv.name.c_str());
  }
}

bool AuroraMonitor::current_day_(uint32_t *day) {
  const ESPTime now = this->time_->now();
  if (!now.is_valid())
    return false;
  *day = (uint32_t) days_from_civil(now.year, now.month, now.day_of_month) + 1;
  return true;
}

void AuroraMonitor::check_day_change_() {
  uint32_t today = 0;
  const bool date_known = this->current_day_(&today);

  if (this->restore_pending_) {
    if (date_known || millis() - this->setup_ms_ >= RESTORE_WAIT_MS)
      this->apply_restored_values_();
    return;
  }

  if (!date_known)
    return;

  for (Inverter &inv : this->inverters_) {
    // While the inverter answers, its own counters are authoritative.
    if (inv.connected || inv.values_day == 0 || inv.values_day == today)
      continue;
    this->reset_period_values_(inv, inv.values_day, today);
    inv.values_day = today;
  }
}

void AuroraMonitor::reset_period_values_(Inverter &inv, uint32_t from_day, uint32_t to_day) {
  // Day numbers are stored as days since 1970-01-01 plus one.
  const int32_t from = (int32_t) from_day - 1;
  const int32_t to = (int32_t) to_day - 1;
  int32_t from_year, to_year;
  uint32_t from_month, to_month;
  civil_from_days(from, &from_year, &from_month);
  civil_from_days(to, &to_year, &to_month);

  InverterReading resets[5];
  uint8_t count = 0;
  resets[count++] = READING_POWER_PEAK_TODAY;
  resets[count++] = READING_ENERGY_TODAY;
  if (week_from_days(from) != week_from_days(to))
    resets[count++] = READING_ENERGY_WEEK;
  if (from_year != to_year || from_month != to_month)
    resets[count++] = READING_ENERGY_MONTH;
  if (from_year != to_year)
    resets[count++] = READING_ENERGY_YEAR;

  ESP_LOGI(TAG, "Inverter %s: new day, resetting the values of the periods that ended", inv.name.c_str());

  for (uint8_t r = 0; r < count; r++) {
    const InverterReading reading = resets[r];
    if (std::isnan(inv.last_values[reading]))
      continue;
    inv.last_values[reading] = 0;
    publish_(inv.sensors[reading], 0);
  }
}

void AuroraMonitor::save_values_(Inverter &inv) {
  SavedValues saved;
  saved.day = inv.values_day;
  for (uint8_t s = 0; s < SAVED_READING_COUNT; s++)
    saved.values[s] = inv.last_values[SAVED_READINGS[s]];

  // Written to flash by ESPHome at the next preferences sync.
  if (inv.saved_values_pref.save(&saved)) {
    ESP_LOGD(TAG, "Inverter %s: energy counters and power peaks saved", inv.name.c_str());
  } else {
    ESP_LOGW(TAG, "Inverter %s: unable to save energy counters and power peaks", inv.name.c_str());
  }
}

void AuroraMonitor::set_activity_led_(bool on) {
  if (this->activity_led_ != nullptr)
    this->activity_led_->digital_write(on);
}

void AuroraMonitor::publish_(sensor::Sensor *sensor, float value) {
  if (sensor != nullptr)
    sensor->publish_state(value);
}

void AuroraMonitor::publish_(text_sensor::TextSensor *sensor, const char *value) {
  if (sensor != nullptr)
    sensor->publish_state(value);
}

void AuroraMonitor::publish_if_changed_(text_sensor::TextSensor *sensor, const std::string &value) {
  if (sensor != nullptr && (!sensor->has_state() || sensor->get_state() != value))
    sensor->publish_state(value);
}

void AuroraMonitor::reset_serial() {
  if (this->reset_pin_ == nullptr) {
    ESP_LOGW(TAG, "No reset pin configured");
    return;
  }
  ESP_LOGI(TAG, "Pulsing the reset pin for %u ms", (unsigned) RESET_PULSE_MS);
  this->reset_pin_->digital_write(true);
  this->set_timeout("reset_serial", RESET_PULSE_MS, [this]() { this->reset_pin_->digital_write(false); });
}

}  // namespace aurora_monitor
}  // namespace esphome
