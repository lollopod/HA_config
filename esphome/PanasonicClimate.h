#include "esphome.h"

static const char *TAG = "Panasonic.climate";

typedef enum IRPanasonicMode {
    IRPanasonicModeOff  = 0b000,
    IRPanasonicModeFan  = 0b001,
    IRPanasonicModeCool = 0b010,
    IRPanasonicModeDry  = 0b011,
    IRPanasonicModeHeat = 0b100,
    IRPanasonicModeAuto  = 0b110
} IRPanasonicMode;

typedef enum IRPanasonicFan {
    IRPanasonicFanMin    = 0b0010,
    IRPanasonicFanLow    = 0b0011,
    IRPanasonicFanMedium = 0b0100,
    IRPanasonicFanHigh   = 0b0101,
    IRPanasonicFanMax   = 0b0110,
    IRPanasonicFanAuto  = 0b1000
} IRPanasonicFan;

// That configuration has a total of 34 bits
//    33: Power bit, if this bit is ON, the A/C will toggle it's power.
// 32-30: Mode - Cool, heat etc.
// 29-28: Fan - Low, medium etc.
// 27-26: Zeros
//    25: Swing On/Off
//    24: iFeel On/Off
//    23: Zero
// 22-19: Temperature, where 15 is 0000, 30 is 1111
//    18: Sleep mode On/Off
// 17- 2: Zeros
//     1: One
//     0: Zero
typedef union PanasonicCode {
    uint32_t num;  //State in raw form
    struct {
      // Byte 0
        uint8_t zeros : 3;
        uint8_t SwingH : 1;
        uint8_t SwingV : 1;
        uint8_t one : 1; //Always appears to be one
      // Byte 1
       uint8_t fixed :8; //Always seems to be 0x36
      // Byte 2
        uint8_t temperature : 4;
        uint8_t fan : 4;
      // Byte 2
        uint8_t mode : 3;
        uint8_t powerToggle : 1; 
    };
} PanasonicCode;

const uint8_t Panasonic_TEMP_MIN = 16;  // Celsius
const uint8_t Panasonic_TEMP_MAX = 30;  // Celsius

const uint16_t PanasonicFreq = 36700;

#define Panasonic_TIME_UNIT 1000
#define Panasonic_NUM_BITS 32

class PanasonicClimate : public climate::Climate, public Component {
 public:
  void setup() override
  {
    if (this->sensor_) {
      this->sensor_->add_on_state_callback([this](float state) {
        this->current_temperature = state;

        // current temperature changed, publish state
        this->publish_state();
      });
      this->current_temperature = this->sensor_->state;
    } else
      this->current_temperature = NAN;

    // restore set points
    auto restore = this->restore_state_();
    if (restore.has_value()) {
      restore->apply(this);
    } else {
      // restore from defaults
      this->mode = climate::CLIMATE_MODE_AUTO;

      // initialize target temperature to some value so that it's not NAN
      this->target_temperature = roundf(this->current_temperature);
    }

    this->active_mode_ = this->mode;
  }

  void set_transmitter(remote_transmitter::RemoteTransmitterComponent *transmitter) {
    this->transmitter_ = transmitter;
  }

  void set_supports_cool(bool supports_cool) { this->supports_cool_ = supports_cool; }
  void set_supports_heat(bool supports_heat) { this->supports_heat_ = supports_heat; }
  void set_sensor(sensor::Sensor *sensor) { this->sensor_ = sensor; }

  /// Override control to change settings of the climate device
  void control(const climate::ClimateCall &call) override
  {
    if (call.get_mode().has_value())
      this->mode = *call.get_mode();
    if (call.get_target_temperature().has_value())
      this->target_temperature = *call.get_target_temperature();

    this->transmit_state_();
    this->publish_state();

    this->active_mode_ = this->mode;
  }

  /// Return the traits of this controller
  climate::ClimateTraits traits() override
  {
    auto traits = climate::ClimateTraits();
    traits.set_supports_current_temperature(this->sensor_ != nullptr);
    traits.set_supports_auto_mode(true);
    traits.set_supports_cool_mode(this->supports_cool_);
    traits.set_supports_heat_mode(this->supports_heat_);
    traits.set_supports_two_point_target_temperature(false);
    traits.set_supports_away(false);
    traits.set_visual_min_temperature(Panasonic_TEMP_MIN);
    traits.set_visual_max_temperature(Panasonic_TEMP_MAX);
    traits.set_visual_temperature_step(1);
    return traits;
  }

  /// Transmit the state of this climate controller via IR
  void transmit_state_()
  {
    PanasonicCode code = { 0 };
    code.zeros = 0;
    code.one = 1;
    code.fixed = 0x36;
    code.fan = IRPanasonicFan::IRPanasonicFanAuto;

    switch (this->mode) {
      case climate::CLIMATE_MODE_COOL:
        code.mode = IRPanasonicMode::IRPanasonicModeCool;
        code.powerToggle = this->active_mode_ == climate::CLIMATE_MODE_OFF ? 1 : 0;
        break;
      case climate::CLIMATE_MODE_HEAT:
        code.mode = IRPanasonicMode::IRPanasonicModeHeat;
        code.powerToggle = this->active_mode_ == climate::CLIMATE_MODE_OFF ? 1 : 0;
        break;
      case climate::CLIMATE_MODE_AUTO:
        code.mode = IRPanasonicMode::IRPanasonicModeAuto;
        code.powerToggle = this->active_mode_ == climate::CLIMATE_MODE_OFF ? 1 : 0;
        break;
      case climate::CLIMATE_MODE_OFF:
      default:
        code.mode = IRPanasonicMode::IRPanasonicModeOff;
        break;
    }

    auto temp = (uint8_t) roundf(clamp(this->target_temperature, Panasonic_TEMP_MIN, Panasonic_TEMP_MAX));
    code.temperature = temp - 15;

    ESP_LOGD(TAG, "Sending Panasonic code: %lld", code.num);

    auto transmit = this->transmitter_->transmit();
    auto data = transmit.get_data();

    data->set_carrier_frequency(PanasonicFreq);
    uint16_t repeat = 3;

    for (uint16_t r = 0; r < repeat; r++) {
      // Header
      data->mark(3 * Panasonic_TIME_UNIT);
      uint16_t next_value = 3 * Panasonic_TIME_UNIT;
      bool is_next_space = true;

      // Data
      for (int j = Panasonic_NUM_BITS - 1; j>=0; j--)
      {
        uint8_t bit = (code.num >> j) & 1;

        // if current index is SPACE
        if (is_next_space) {
          // one is one unit low, then one unit up
          // since we're pointing at SPACE, we should increase it by a unit
          // then add another MARK unit
          if (bit == 1) {
            data->space(next_value + Panasonic_TIME_UNIT);
            next_value = Panasonic_TIME_UNIT;
            is_next_space = false;

          } else {
            // we need a MARK unit, then SPACE unit
            data->space(next_value);
            data->mark(Panasonic_TIME_UNIT);
            next_value = Panasonic_TIME_UNIT;
            is_next_space = true;
          }

        } else {
          // current index is MARK
          
          // one is one unit low, then one unit up
          if (bit == 1) {
            data->mark(next_value);
            data->space(Panasonic_TIME_UNIT);
            next_value = Panasonic_TIME_UNIT;
            is_next_space = false;

          } else {
            data->mark(next_value + Panasonic_TIME_UNIT);
            next_value = Panasonic_TIME_UNIT;
            is_next_space = true;
          }
        }
      }

      // Last value must be SPACE
      data->space(next_value);
    }

    // Footer
    data->mark(4 * Panasonic_TIME_UNIT);

    transmit.perform();
  }

  ClimateMode active_mode_;

  bool supports_cool_{true};
  bool supports_heat_{true};

  remote_transmitter::RemoteTransmitterComponent *transmitter_;
  sensor::Sensor *sensor_{nullptr};
};
