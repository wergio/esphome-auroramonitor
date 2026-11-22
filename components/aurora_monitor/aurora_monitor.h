#pragma once

#include <string>
#include <vector>

#include "esphome/components/button/button.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "ABBAurora.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace aurora_monitor {

// Values read from each inverter, in the order they are requested. The grid
// power follows the input powers, so that the efficiency is computed from
// readings taken as close in time as possible.
enum InverterReading : uint8_t {
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
  READING_POWER_PEAK_TODAY,
  READING_POWER_PEAK_MAX,
  READING_TEMPERATURE_INVERTER,
  READING_TEMPERATURE_BOOSTER,
  READING_ENERGY_TODAY,
  READING_ENERGY_WEEK,
  READING_ENERGY_MONTH,
  READING_ENERGY_YEAR,
  READING_ENERGY_TOTAL,
  READING_COUNT
};

// Requests made only now and then, after the readings of a cycle: all of
// them when the inverter connects (at boot and every morning), the alarm
// history also when the alarm state changes, the slow measurements also
// every hour.
enum InverterExtra : uint8_t {
  EXTRA_MODEL,                     // command 58
  EXTRA_PART_NUMBER,               // command 52
  EXTRA_SERIAL_NUMBER,             // command 63
  EXTRA_MANUFACTURING,             // command 65
  EXTRA_FIRMWARE,                  // command 72
  EXTRA_CLOCK,                     // command 70
  EXTRA_LAST_ALARMS,               // command 86
  EXTRA_ISOLATION_RESISTANCE,      // command 59, measure 30 (hourly)
  EXTRA_LEAKAGE_CURRENT_DC_DC,     // command 59, measure 6 (hourly)
  EXTRA_LEAKAGE_CURRENT_INVERTER,  // command 59, measure 7 (hourly)
  EXTRA_COUNT
};

// Number of alarms kept in the alarm history.
static const uint8_t ALARM_HISTORY_SIZE = 4;

// Number of values saved to flash for each inverter (energy counters and
// power peaks), see SAVED_READINGS in aurora_monitor.cpp.
static const uint8_t SAVED_READING_COUNT = 7;

// Alarm history of one inverter saved to flash; change ALARM_HISTORY_KEY in
// aurora_monitor.cpp if the layout changes.
struct SavedAlarms {
  uint8_t codes[ALARM_HISTORY_SIZE];
  uint8_t size;
};

// Values of one inverter saved to flash; change SAVED_VALUES_KEY in
// aurora_monitor.cpp if the layout changes.
struct SavedValues {
  uint32_t day;  // day the values refer to (0: unknown)
  float values[SAVED_READING_COUNT];
};

// Configuration and state of one inverter on the RS485 bus.
struct Inverter {
  std::string name;  // entity prefix, or "address N" when there is none
  uint8_t address{0};
  uint8_t mppt_inputs{2};  // 1: the second input (channel 2) is not read
  bool connected{false};
  text_sensor::TextSensor *connection_status{nullptr};
  text_sensor::TextSensor *global_state{nullptr};
  text_sensor::TextSensor *alarm_state{nullptr};
  text_sensor::TextSensor *inverter_state{nullptr};
  text_sensor::TextSensor *input_states[2]{};  // DC/DC state of each input
  sensor::Sensor *sensors[READING_COUNT]{};
  sensor::Sensor *power_in_total{nullptr};
  sensor::Sensor *efficiency{nullptr};
  // Text and numeric sensor of each extra request (inverter_time for
  // EXTRA_CLOCK, with clock_offset as its numeric sensor).
  text_sensor::TextSensor *extra_texts[EXTRA_COUNT]{};
  sensor::Sensor *extra_values[EXTRA_COUNT]{};
  uint16_t extras_pending{0};  // bit mask of InverterExtra
  uint32_t last_hourly_ms{0};
  uint8_t last_alarm_state{0};
  // Alarms read from the inverter, most recent first. The inverter empties
  // its own queue when it is read, so the history is kept here and in flash.
  uint8_t alarm_history[ALARM_HISTORY_SIZE]{};
  uint8_t alarm_history_size{0};
  ESPPreferenceObject alarms_pref;
  // Readings requested in each cycle: those with a sensor, plus those needed
  // by the computed sensors.
  bool needed[READING_COUNT]{};

  // Last value read for every reading, used to save and restore the values
  // that must survive a reboot (energy counters and power peaks).
  float last_values[READING_COUNT];
  // Day the values refer to, as days since 1970-01-01 plus one (0: unknown).
  // Used to reset the daily, weekly, monthly and yearly values when the
  // period changes while the inverter is off.
  uint32_t values_day{0};
  bool read_since_boot{false};
  ESPPreferenceObject saved_values_pref;
  SavedValues restored{};  // values loaded at boot
  bool has_restored{false};
};

class AuroraMonitor : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Pulses the reset pin, e.g. to power-cycle the RS485 adapter.
  void reset_serial();

  // Adds an inverter; the following set_*_sensor() calls use its index.
  void add_inverter(uint8_t address, const std::string &prefix, uint8_t mppt_inputs);
  void set_uart_pins(uint8_t rx_pin, uint8_t tx_pin) {
    this->rx_pin_ = rx_pin;
    this->tx_pin_ = tx_pin;
  }
  void set_tx_control_pin(GPIOPin *pin) { this->tx_control_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_tx_enable_delay(uint32_t ms) { this->tx_enable_delay_ms_ = ms; }
  void set_response_timeout(uint32_t ms) { this->response_timeout_ms_ = ms; }
  void set_activity_led_pin(GPIOPin *pin) { this->activity_led_ = pin; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *clock) { this->time_ = clock; }
#endif

  void set_sensor(uint8_t inverter, InverterReading reading, sensor::Sensor *sensor) {
    this->inverters_[inverter].sensors[reading] = sensor;
  }
  void set_power_in_total_sensor(uint8_t inverter, sensor::Sensor *sensor) {
    this->inverters_[inverter].power_in_total = sensor;
  }
  void set_extra_text_sensor(uint8_t inverter, InverterExtra extra, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].extra_texts[extra] = sensor;
  }
  void set_extra_value_sensor(uint8_t inverter, InverterExtra extra, sensor::Sensor *sensor) {
    this->inverters_[inverter].extra_values[extra] = sensor;
  }
  void set_inverter_state_sensor(uint8_t inverter, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].inverter_state = sensor;
  }
  void set_input_state_sensor(uint8_t inverter, uint8_t input, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].input_states[input] = sensor;
  }
  void set_retries(uint8_t retries) { this->retries_ = retries; }
  void set_efficiency_sensor(uint8_t inverter, sensor::Sensor *sensor) {
    this->inverters_[inverter].efficiency = sensor;
  }
  void set_connection_status_sensor(uint8_t inverter, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].connection_status = sensor;
  }
  void set_global_state_sensor(uint8_t inverter, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].global_state = sensor;
  }
  void set_alarm_state_sensor(uint8_t inverter, text_sensor::TextSensor *sensor) {
    this->inverters_[inverter].alarm_state = sensor;
  }

 protected:
  // Polling cycle, run one step per loop() call so that the main loop (WiFi,
  // API, OTA) keeps running while the inverters are being read.
  enum class PollState : uint8_t {
    IDLE,    // waiting for the next update()
    PROBE,   // ask the current inverter for its state
    READ,    // read the next value of the current inverter
    EXTRA,   // make the next pending extra request of the current inverter
    REOPEN,  // reopen the serial port after an inverter did not answer
  };

  bool start_request_(Inverter &inverter);
  bool skip_to_needed_reading_(Inverter &inverter);
  uint8_t transmission_state_() const;
  float input_power_(const Inverter &inverter) const;
  void finish_readings_(Inverter &inverter);
  bool skip_to_pending_extra_(Inverter &inverter);
  void on_extra_done_(Inverter &inverter, bool ok);
  void decode_extra_(Inverter &inverter, InverterExtra extra);
  void end_inverter_(Inverter &inverter);
  void publish_alarm_history_(Inverter &inverter);
  static bool has_extra_sensor_(const Inverter &inverter, InverterExtra extra);
  void publish_efficiency_(Inverter &inverter);
  void on_probe_done_(Inverter &inverter, bool ok);
  void on_reading_done_(Inverter &inverter, bool ok);
  void record_response_time_(uint32_t ms);
  void open_serial_();
  void reopen_serial_();
  void next_inverter_(uint32_t delay_ms);
  void clear_instant_values_(Inverter &inverter);
  void load_saved_values_(Inverter &inverter);
  void apply_restored_values_();
  void save_values_(Inverter &inverter);
  bool current_day_(uint32_t *day);
  void check_day_change_();
  void reset_period_values_(Inverter &inverter, uint32_t from_day, uint32_t to_day);
  void set_activity_led_(bool on);

  static void publish_(sensor::Sensor *sensor, float value);
  static void publish_(text_sensor::TextSensor *sensor, const char *value);
  static void publish_if_changed_(text_sensor::TextSensor *sensor, const std::string &value);

  ABBAurora inverter_{0};
  std::vector<Inverter> inverters_;

  uint8_t rx_pin_{0};
  uint8_t tx_pin_{0};
  GPIOPin *tx_control_pin_{nullptr};
  GPIOPin *reset_pin_{nullptr};
  GPIOPin *activity_led_{nullptr};
  uint32_t tx_enable_delay_ms_{50};
  uint32_t response_timeout_ms_{1000};

  PollState state_{PollState::IDLE};
  bool initialized_{false};     // true after the first polling cycle
  bool request_active_{false};  // a non-blocking request is in progress
  uint8_t inverter_index_{0};
  uint8_t reading_index_{0};
  uint8_t extra_index_{0};
  uint8_t retries_{1};   // extra attempts after a request without a valid answer
  uint8_t attempts_{0};  // attempts made for the current request
  uint32_t next_action_ms_{0};
  // Values read in the current cycle, used for the computed sensors.
  float power_in_1_{NAN};
  float power_in_2_{NAN};
  float grid_power_{NAN};
  HighFrequencyLoopRequester high_freq_;

  // Response time statistics of the current inverter, logged once per cycle.
  uint8_t responses_{0};
  uint32_t response_min_ms_{0};
  uint32_t response_max_ms_{0};

  // Saved values loaded at boot wait for the clock before being published.
  bool restore_pending_{false};
  uint32_t setup_ms_{0};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
};

// Button that pulses the reset pin of the RS485 adapter.
class ResetButton : public button::Button, public Parented<AuroraMonitor> {
 protected:
  void press_action() override { this->parent_->reset_serial(); }
};

}  // namespace aurora_monitor
}  // namespace esphome
