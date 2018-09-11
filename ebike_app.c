/*
 * TongSheng TSDZ2 motor controller firmware/
 *
 * Copyright (C) Casainho, 2018.
 *
 * Released under the GPL License, Version 3
 */

#include "ebike_app.h"

#include <stdint.h>
#include <stdio.h>
#include "stm8s.h"
#include "stm8s_gpio.h"
#include "main.h"
#include "interrupts.h"
#include "adc.h"
#include "utils.h"
#include "motor.h"
#include "pwm.h"
#include "uart.h"
#include "brake.h"
#include "eeprom.h"
#include "config.h"
#include "utils.h"
#include "lights.h"

#define STATE_NO_PEDALLING                0
#define STATE_STARTUP_PEDALLING           1
#define STATE_PEDALLING                   2

#define BOOST_STATE_BOOST_DISABLED        0
#define BOOST_STATE_START_BOOST           1
#define BOOST_STATE_BOOST                 2
#define BOOST_STATE_END_BOOST             3
#define BOOST_STATE_FADE                  4
#define BOOST_STATE_BOOST_WAIT_TO_RESTART 5

uint8_t ui8_adc_battery_max_current = ADC_BATTERY_CURRENT_MAX;
uint8_t ui8_target_battery_max_power_x10 = ADC_BATTERY_CURRENT_MAX;

volatile uint8_t ui8_throttle = 0;
volatile uint8_t ui8_torque_sensor_value1 = 0;
volatile uint8_t ui8_torque_sensor = 0;
volatile uint8_t ui8_torque_sensor_raw = 0;
volatile uint8_t ui8_adc_torque_sensor_min_value;
volatile uint8_t ui8_adc_torque_sensor_max_value;
volatile uint8_t ui8_adc_battery_current_offset;
volatile uint8_t ui8_ebike_app_state = EBIKE_APP_STATE_MOTOR_STOP;
volatile uint8_t ui8_adc_target_battery_max_current;
uint8_t ui8_adc_battery_current_max;

volatile uint16_t ui16_pas_pwm_cycles_ticks = (uint16_t) PAS_ABSOLUTE_MIN_CADENCE_PWM_CYCLE_TICKS;
volatile uint8_t ui8_pas_direction = 0;
uint8_t ui8_pas_cadence_rpm = 0;
uint8_t ui8_pedal_human_power = 0;
uint8_t ui8_startup_boost_enable = 0;
uint8_t ui8_startup_boost_fade_enable = 0;
uint8_t ui8_startup_boost_state_machine = 0;
uint8_t ui8_startup_boost_no_torque = 0;
uint8_t ui8_startup_boost_timer = 0;
uint8_t ui8_startup_boost_fade_steps = 0;
uint16_t ui16_startup_boost_fade_variable_x256;
uint16_t ui16_startup_boost_fade_variable_step_amount_x256;

// wheel speed
volatile uint16_t ui16_wheel_speed_sensor_pwm_cycles_ticks = (uint16_t) WHEEL_SPEED_SENSOR_MAX_PWM_CYCLE_TICKS;
uint8_t ui8_wheel_speed_max = 0;
float f_wheel_speed_x10;
uint16_t ui16_wheel_speed_x10;

volatile struct_configuration_variables configuration_variables;

// UART
volatile uint8_t ui8_received_package_flag = 0;
volatile uint8_t ui8_rx_buffer[10];
volatile uint8_t ui8_rx_counter = 0;
volatile uint8_t ui8_tx_buffer[22];
volatile uint8_t ui8_tx_counter = 0;
volatile uint8_t ui8_i;
volatile uint8_t ui8_checksum;
volatile uint8_t ui8_byte_received;
volatile uint8_t ui8_state_machine = 0;
volatile uint8_t ui8_uart_received_first_package = 0;
static uint16_t ui16_crc_rx;
static uint16_t ui16_crc_tx;
static uint8_t ui8_last_package_id;

uint8_t ui8_tstr_state_machine = STATE_NO_PEDALLING;
uint8_t ui8_rtst_counter = 0;

uint16_t ui16_adc_motor_temperatured_accumulated = 0;

uint8_t ui8_adc_battery_target_current;

struct_pi_controller_state walk_assist_wheel_speed_pi_controller_state;

// function prototypes
static void ebike_control_motor (void);
void ebike_app_set_battery_max_current (uint8_t ui8_value);
void ebike_app_set_target_adc_battery_max_current (uint8_t ui8_value);
void communications_controller (void);
void uart_send_package (void);
void calc_wheel_speed (void);
void throttle_read (void);
void torque_sensor_read (void);
void startup_boost (void);
void calc_motor_temperature (void);

void read_pas_cadence (void)
{
  // cadence in RPM =  60 / (ui16_pas_timer2_ticks * PAS_NUMBER_MAGNETS * 0.000064)
  if (ui16_pas_pwm_cycles_ticks >= ((uint16_t) PAS_ABSOLUTE_MIN_CADENCE_PWM_CYCLE_TICKS)) { ui8_pas_cadence_rpm = 0; }
  else
  {
    ui8_pas_cadence_rpm = (uint8_t) (60 / (((float) ui16_pas_pwm_cycles_ticks) * ((float) PAS_NUMBER_MAGNETS) * 0.000064));

    if (ui8_pas_cadence_rpm > configuration_variables.ui8_pas_max_cadence)
    {
      ui8_pas_cadence_rpm = configuration_variables.ui8_pas_max_cadence;
    }
  }
}

void torque_sensor_read (void)
{
  // map value from 0 up to 255
  // map value from 0 up to 255
  ui8_torque_sensor_raw = (uint8_t) (map (
      UI8_ADC_TORQUE_SENSOR,
      (uint8_t) ui8_adc_torque_sensor_min_value,
      (uint8_t) ui8_adc_torque_sensor_max_value,
      (uint8_t) 0,
      (uint8_t) 255));

  switch (ui8_tstr_state_machine)
  {
    // ebike is stopped, wait for throttle signal
    case STATE_NO_PEDALLING:
    if ((ui8_torque_sensor_raw > 0) &&
        (!brake_is_set()))
    {
      ui8_tstr_state_machine = STATE_STARTUP_PEDALLING;
    }
    break;

    // now count 2 seconds
    case STATE_STARTUP_PEDALLING:
    if (ui8_rtst_counter++ > 20) // 2 seconds
    {
      ui8_rtst_counter = 0;
      ui8_tstr_state_machine = STATE_PEDALLING;
    }

    // ebike is not moving, let's return to begin
    if (ui16_wheel_speed_x10 == 0)
    {
      ui8_rtst_counter = 0;
      ui8_tstr_state_machine = 0;
    }
    break;

    // wait on this state and reset when ebike stops
    case STATE_PEDALLING:
    if ((ui16_wheel_speed_x10 == 0) && (ui8_torque_sensor_raw == 0))
    {
      ui8_tstr_state_machine = STATE_NO_PEDALLING;
    }
    break;

    default:
    break;
  }

  // bike is moving but user doesn't pedal, disable torque sensor signal because user can be resting the feet on the pedals
  if ((ui8_tstr_state_machine == STATE_PEDALLING) && (ui8_pas_cadence_rpm == 0))
  {
    ui8_torque_sensor = 0;
  }
  else
  {
    ui8_torque_sensor = ui8_torque_sensor_raw;
  }
}

void throttle_read (void)
{
  // map value from 0 up to 255
  ui8_throttle = (uint8_t) (map (
      UI8_ADC_THROTTLE,
      (uint8_t) ADC_THROTTLE_MIN_VALUE,
      (uint8_t) ADC_THROTTLE_MAX_VALUE,
      (uint8_t) 0,
      (uint8_t) 255));
}

void startup_boost (void)
{
  if (configuration_variables.ui8_startup_motor_power_boost_time > 0)
  {
    switch (ui8_startup_boost_state_machine)
    {
      // ebike is stopped, wait for throttle signal to startup boost
      case BOOST_STATE_BOOST_DISABLED:
        if ((ui8_torque_sensor > 0) &&
            (!brake_is_set()))
        {
          ui8_startup_boost_state_machine = BOOST_STATE_START_BOOST;
        }
      break;

      case BOOST_STATE_START_BOOST:
        ui8_startup_boost_enable = 1;
        ui8_startup_boost_timer = configuration_variables.ui8_startup_motor_power_boost_time;
        ui8_startup_boost_state_machine = BOOST_STATE_BOOST;
      break;

      case BOOST_STATE_BOOST:
        // decrement timer
        if (ui8_startup_boost_timer > 0) { ui8_startup_boost_timer--; }

        // disable boost if
        if ((ui8_torque_sensor == 0) ||
            (ui8_startup_boost_timer == 0))
        {
          ui8_startup_boost_state_machine = BOOST_STATE_END_BOOST;
        }
      break;

      case BOOST_STATE_END_BOOST:
        ui8_startup_boost_enable = 0;

        // setup variables for fade
        ui8_startup_boost_fade_steps = configuration_variables.ui8_startup_motor_power_boost_fade_time;
        ui16_startup_boost_fade_variable_x256 = ((uint16_t) ui8_adc_battery_target_current << 8);
        ui16_startup_boost_fade_variable_step_amount_x256 = (ui16_startup_boost_fade_variable_x256 / ((uint16_t) ui8_startup_boost_fade_steps));
        ui8_startup_boost_fade_enable = 1;

        ui8_startup_boost_state_machine = BOOST_STATE_FADE;
      break;

      case BOOST_STATE_FADE:
        if (ui8_startup_boost_fade_steps > 0) { ui8_startup_boost_fade_steps--; }

        // disable fade if
        if ((ui8_torque_sensor_raw == 0) ||
            (ui8_pas_cadence_rpm == 0) ||
            (ui8_startup_boost_fade_steps == 0))
        {
          ui8_startup_boost_fade_enable = 0;
          ui8_startup_boost_fade_steps = 0;
          ui8_startup_boost_state_machine = BOOST_STATE_BOOST_WAIT_TO_RESTART;
        }
      break;

      // restart when user is not pressing the pedals AND/OR wheel speed = 0
      case BOOST_STATE_BOOST_WAIT_TO_RESTART:
        // wheel speed must be 0 as also torque sensor
        if ((configuration_variables.ui8_startup_motor_power_boost_state & 1) == 0)
        {
          if ((ui16_wheel_speed_x10 == 0) && (ui8_torque_sensor_raw == 0))
          {
            ui8_startup_boost_state_machine = BOOST_STATE_BOOST_DISABLED;
          }
        }
        // torque sensor must be 0
        if ((configuration_variables.ui8_startup_motor_power_boost_state & 1) > 0)
        {
          if ((ui8_torque_sensor_raw == 0) || (ui8_pas_cadence_rpm == 0))
          {
            ui8_startup_boost_state_machine = BOOST_STATE_BOOST_DISABLED;
          }
        }
      break;

      default:
      break;
    }
  }
}

void ebike_app_init (void)
{
  // init variables with the stored value on EEPROM
  eeprom_init_variables ();
  ebike_app_set_battery_max_current (ADC_BATTERY_CURRENT_MAX);

  walk_assist_wheel_speed_pi_controller_state.ui8_kp_dividend = WHEEL_SPEED_PI_CONTROLLER_KP_DIVIDEND;
  walk_assist_wheel_speed_pi_controller_state.ui8_kp_divisor = WHEEL_SPEED_PI_CONTROLLER_KP_DIVISOR;
  walk_assist_wheel_speed_pi_controller_state.ui8_ki_dividend = WHEEL_SPEED_PI_CONTROLLER_KI_DIVIDEND;
  walk_assist_wheel_speed_pi_controller_state.ui8_ki_divisor = WHEEL_SPEED_PI_CONTROLLER_KI_DIVISOR;
  walk_assist_wheel_speed_pi_controller_state.i16_i_term = 0;
}

void ebike_app_controller (void)
{
  throttle_read ();
  torque_sensor_read ();
  read_pas_cadence ();
  calc_wheel_speed ();
  calc_motor_temperature ();
  ebike_control_motor ();
  communications_controller ();
}

void communications_controller (void)
{
  uint32_t ui32_temp;

#ifndef DEBUG_UART
  if (ui8_received_package_flag)
  {
    // verify crc of the package
    ui16_crc_rx = 0xffff;
    for (ui8_i = 0; ui8_i < 8; ui8_i++)
    {
      crc16 (ui8_rx_buffer[ui8_i], &ui16_crc_rx);
    }

    // see if CRC is ok...
    if (((((uint16_t) ui8_rx_buffer [9]) << 8) + ((uint16_t) ui8_rx_buffer [8])) == ui16_crc_rx)
    {
      // assist level
      configuration_variables.ui8_power_regular_state_div25 = ui8_rx_buffer [1];
      // head light
      configuration_variables.ui8_lights = ValBit(ui8_rx_buffer [2], 0);
      lights_set_state (configuration_variables.ui8_lights);
      // walk assist
      configuration_variables.ui8_walk_assist = ValBit(ui8_rx_buffer [2], 1);
      // battery max current
      configuration_variables.ui8_battery_max_current = ui8_rx_buffer [3];
      ebike_app_set_battery_max_current (configuration_variables.ui8_battery_max_current);
      // target battery max power
      configuration_variables.ui8_target_battery_max_power_div25 = ui8_rx_buffer [4];

      // now get data depending on variable ID for each package sent
      switch (ui8_rx_buffer [5])
      {
        case 0:
          // battery low voltage cut-off
          configuration_variables.ui16_battery_low_voltage_cut_off_x10 = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
          // calc the value in ADC steps and set it up
          ui32_temp = ((uint32_t) configuration_variables.ui16_battery_low_voltage_cut_off_x10 << 8) / ((uint32_t) ADC8BITS_BATTERY_VOLTAGE_PER_ADC_STEP_INVERSE_X256);
          ui32_temp /= 10;
          motor_set_adc_battery_voltage_cut_off ((uint8_t) ui32_temp);
        break;

        case 1:
          // wheel perimeter
          configuration_variables.ui16_wheel_perimeter = (((uint16_t) ui8_rx_buffer [7]) << 8) + ((uint16_t) ui8_rx_buffer [6]);
        break;

        case 2:
          // wheel max speed
          configuration_variables.ui8_wheel_max_speed = ui8_rx_buffer [6];
          // PAS max cadence RPM
          configuration_variables.ui8_pas_max_cadence = ui8_rx_buffer [7];
        break;

        case 3:
          configuration_variables.ui8_cruise_control = ui8_rx_buffer [6] & 1;
          configuration_variables.ui8_motor_voltage_type = (ui8_rx_buffer [6] & 2) >> 1;
          configuration_variables.ui8_motor_assistance_startup_without_pedal_rotation = (ui8_rx_buffer [6] & 4) >> 2;
          configuration_variables.ui8_throttle_adc_measures_motor_temperature = (ui8_rx_buffer [6] & 8) >> 3;
          configuration_variables.ui8_motor_over_temperature_limit_current = (ui8_rx_buffer [6] & 16) >> 4;

          configuration_variables.ui8_startup_motor_power_boost_state = ui8_rx_buffer [7] & 1;
          configuration_variables.ui8_startup_motor_power_boost_limit_to_max_power = (ui8_rx_buffer [7] & 2) >> 1;
        break;

        case 4:
          // startup motor power boost
          configuration_variables.ui8_startup_motor_power_boost_div25 = ui8_rx_buffer [6];
          // startup motor power boost time
          configuration_variables.ui8_startup_motor_power_boost_time = ui8_rx_buffer [7];
        break;

        case 5:
          // startup motor power boost fade time
          configuration_variables.ui8_startup_motor_power_boost_fade_time = ui8_rx_buffer [6];
        break;

        case 6:
          // motor temperature min and max values to limit
          configuration_variables.ui8_motor_temperature_min_value_to_limit = ui8_rx_buffer [6];
          configuration_variables.ui8_motor_temperature_max_value_to_limit = ui8_rx_buffer [7];
        break;
      }

      // store last package ID
      ui8_last_package_id = ui8_rx_buffer [5];

      // verify if any configuration_variables did change and if so, save all of them in the EEPROM
      eeprom_write_if_values_changed ();

      // signal that we processed the full package
      ui8_received_package_flag = 0;

      ui8_uart_received_first_package = 1;
    }

    // enable UART2 receive interrupt as we are now ready to receive a new package
    UART2->CR2 |= (1 << 5);
  }

  uart_send_package ();
#endif
}

void uart_send_package (void)
{
  uint16_t ui16_temp;

  //send the data to the LCD
  // start up byte
  ui8_tx_buffer[0] = 0x43;

  ui8_tx_buffer[1] = ui8_last_package_id;

  ui16_temp = motor_get_adc_battery_voltage_filtered_10b ();
  // adc 10 bits battery voltage
  ui8_tx_buffer[2] = (ui16_temp & 0xff);
  ui8_tx_buffer[3] = ((uint8_t) (ui16_temp >> 4)) & 0x30;

  // battery current x5
  ui8_tx_buffer[4] = (uint8_t) ((float) motor_get_adc_battery_current_filtered_10b () * 0.826);

  // wheel speed
  ui8_tx_buffer[5] = (uint8_t) (ui16_wheel_speed_x10 & 0xff);
  ui8_tx_buffer[6] = (uint8_t) (ui16_wheel_speed_x10 >> 8);

  // brake state
  if (motor_controller_state_is_set (MOTOR_CONTROLLER_STATE_BRAKE))
  {
    ui8_tx_buffer[7] |= 1;
  }
  else
  {
    ui8_tx_buffer[7] &= ~1;
  }

  // error states
  ui8_tx_buffer[8] = 0;

  if (configuration_variables.ui8_throttle_adc_measures_motor_temperature)
  {
    ui8_tx_buffer[9] = UI8_ADC_THROTTLE;
    ui8_tx_buffer[10] = configuration_variables.ui8_motor_temperature;
  }
  else
  {
    // ADC throttle
    ui8_tx_buffer[9] = UI8_ADC_THROTTLE;
    // throttle value with offset removed and mapped to 255
    ui8_tx_buffer[10] = ui8_throttle;
  }

  // ADC torque_sensor
  ui8_tx_buffer[11] = UI8_ADC_TORQUE_SENSOR;
  // torque sensor value with offset removed and mapped to 255
  ui8_tx_buffer[12] = ui8_torque_sensor;
  // PAS cadence
  ui8_tx_buffer[13] = ui8_pas_cadence_rpm;
  // pedal human power mapped to 255
  ui8_tx_buffer[14] = ui8_pedal_human_power;
  // PWM duty_cycle
  ui8_tx_buffer[15] = ui8_duty_cycle;
  // motor speed in ERPS
  ui16_temp = ui16_motor_get_motor_speed_erps ();
  ui8_tx_buffer[16] = (uint8_t) (ui16_temp & 0xff);
  ui8_tx_buffer[17] = (uint8_t) (ui16_temp >> 8);
  // FOC angle
  ui8_tx_buffer[18] = ui8_foc_angle;
  // temperature actual limiting value
  ui8_tx_buffer[19] = configuration_variables.ui8_temperature_current_limiting_value;

  // prepare crc of the package
  ui16_crc_tx = 0xffff;
  for (ui8_i = 0; ui8_i <= 19; ui8_i++)
  {
    crc16 (ui8_tx_buffer[ui8_i], &ui16_crc_tx);
  }
  ui8_tx_buffer[20] = (uint8_t) (ui16_crc_tx & 0xff);
  ui8_tx_buffer[21] = (uint8_t) (ui16_crc_tx >> 8) & 0xff;

  // send the full package to UART
  for (ui8_i = 0; ui8_i <= 21; ui8_i++)
  {
    putchar (ui8_tx_buffer[ui8_i]);
  }
}

static void ebike_control_motor (void)
{
  static uint16_t ui16_temp;
  uint8_t ui8_temp;
  uint8_t _ui8_pas_cadence_rpm;
  uint16_t ui16_battery_voltage_filtered;
  uint32_t ui32_adc_max_battery_current_boost_state_x4;
  uint32_t ui32_adc_max_battery_current_regular_state_x4;
  uint32_t ui32_adc_max_battery_current_x4;
  uint8_t ui8_adc_max_battery_current;
  uint8_t ui8_startup_enable;
  uint16_t ui16_adc_battery_target_current_x256;
  uint8_t ui8_boost_enable;
  uint32_t ui32_temp;

  // walk assist
  if (configuration_variables.ui8_walk_assist == 1)
  {
    walk_assist_wheel_speed_pi_controller_state.ui8_current_value = (uint8_t) ui16_wheel_speed_x10;
    walk_assist_wheel_speed_pi_controller_state.ui8_target_value = 6 * 10;
    pi_controller (&walk_assist_wheel_speed_pi_controller_state);

    ebike_app_set_target_adc_battery_max_current (ui8_adc_battery_current_max);

    if (!brake_is_set ())
    {
      // motor_set_pwm_duty_cycle_target (128);
      motor_set_pwm_duty_cycle_target (walk_assist_wheel_speed_pi_controller_state.ui8_controller_output_value);
    }
    else
    {
      pi_controller_reset (&walk_assist_wheel_speed_pi_controller_state);
    }
  }
  else
  {
    // calc battery voltage
    ui16_battery_voltage_filtered = (uint16_t) motor_get_adc_battery_voltage_filtered_10b () * ADC10BITS_BATTERY_VOLTAGE_PER_ADC_STEP_X512;
    ui16_battery_voltage_filtered = ui16_battery_voltage_filtered >> 9;

    // calc max battery current for boost state
    // calc max battery current for regular state
    // calc max battery current (defined by user on LCD3)
    ui32_adc_max_battery_current_boost_state_x4 = 0;
    ui32_adc_max_battery_current_regular_state_x4 = 0;
    if (ui16_battery_voltage_filtered > 15)
    {
      // 1.6 = 1 / 0.625 (each adc step for current)
      // 25 * 1.6 = 40
      // 40 * 4 = 160
      if (configuration_variables.ui8_startup_motor_power_boost_div25 > 0)
      {
        ui32_adc_max_battery_current_boost_state_x4 = (((uint32_t) configuration_variables.ui8_startup_motor_power_boost_div25) * 160) / ((uint32_t) ui16_battery_voltage_filtered);
      }

      if (configuration_variables.ui8_power_regular_state_div25 > 0)
      {
        ui32_adc_max_battery_current_regular_state_x4 = (((uint32_t) configuration_variables.ui8_power_regular_state_div25) * 160) / ((uint32_t) ui16_battery_voltage_filtered);
      }

      if (configuration_variables.ui8_target_battery_max_power_div25 > 0)
      {
        ui32_adc_max_battery_current_x4 = (((uint32_t) configuration_variables.ui8_target_battery_max_power_div25) * 160) / ((uint32_t) ui16_battery_voltage_filtered);
        ui8_adc_max_battery_current = ui32_adc_max_battery_current_x4 >> 2;
      }
    }

    // start with disabled
    ui8_startup_enable = 0;
    // start when we press the pedals
    if ((configuration_variables.ui8_power_regular_state_div25 && ui8_torque_sensor)) { ui8_startup_enable = 1; }

    _ui8_pas_cadence_rpm = ui8_pas_cadence_rpm;
    if (configuration_variables.ui8_motor_assistance_startup_without_pedal_rotation)
    {
      if (ui8_pas_cadence_rpm < 10) { _ui8_pas_cadence_rpm = 10; }
    }
    else
    {
      if (ui8_pas_cadence_rpm < 10) { _ui8_pas_cadence_rpm = 0; }
    }

    // startup boost state machine
    startup_boost ();

    if (ui8_startup_boost_enable &&
        configuration_variables.ui8_power_regular_state_div25 &&
        (_ui8_pas_cadence_rpm > 0))
    {
      ui8_boost_enable = 1;
    }
    else
    {
      ui8_boost_enable = 0;
    }

    if (ui8_boost_enable)
    {
      ui32_temp = map ((uint32_t) ui8_torque_sensor,
            (uint32_t) 0,
            (uint32_t) 255,
            (uint32_t) 0,
            (uint32_t) ui32_adc_max_battery_current_boost_state_x4);
      ui32_temp >>= 2;
      if (ui32_temp > 255) { ui8_adc_battery_target_current = 255; }
      else { ui8_adc_battery_target_current = (uint8_t) ui32_temp; }
    }
    else
    {
      // cadence percentage (in x256)
      ui16_temp = (uint16_t) (map (((uint32_t) _ui8_pas_cadence_rpm),
            (uint32_t) 0,
            (uint32_t) configuration_variables.ui8_pas_max_cadence,
            (uint32_t) 0,
            (uint32_t) 255));

      // human power: pedal torque * pedal cadence
      ui8_pedal_human_power = ((((uint16_t) ui8_torque_sensor) * ui16_temp) >> 8);

      ui32_temp = map ((uint32_t) ui8_pedal_human_power,
            (uint32_t) 0,
            (uint32_t) 254, // 254 because max of (255 * 255) >> 8 is 254
            (uint32_t) 0,
            (uint32_t) ui32_adc_max_battery_current_regular_state_x4);
      ui32_temp >>= 2;
      if (ui32_temp > 255) { ui8_adc_battery_target_current = 255; }
      else { ui8_adc_battery_target_current = (uint8_t) ui32_temp; }
    }

    // ***********************************************************************************
    // make transition from boost to regular level
    //
    if (ui8_startup_boost_fade_enable)
    {
      // here we try to converge to the regular value, ramping down or up step by step
      ui16_adc_battery_target_current_x256 = ((uint16_t) ui8_adc_battery_target_current) << 8;
      if (ui16_startup_boost_fade_variable_x256 > ui16_adc_battery_target_current_x256)
      {
        ui16_startup_boost_fade_variable_x256 -= ui16_startup_boost_fade_variable_step_amount_x256;
      }
      else if (ui16_startup_boost_fade_variable_x256 < ui16_adc_battery_target_current_x256)
      {
        ui16_startup_boost_fade_variable_x256 += ui16_startup_boost_fade_variable_step_amount_x256;
      }

      ui8_adc_battery_target_current = (uint8_t) (ui16_startup_boost_fade_variable_x256 >> 8);
    }
    // ***********************************************************************************

    // ***********************************************************************************
    // throttle: if throttle has higher value, then use it!
    //
    ui8_temp = (uint8_t) (map ((uint32_t) ui8_throttle,
          (uint32_t) 0,
          (uint32_t) 255,
          (uint32_t) 0,
          (uint32_t) ui8_adc_battery_current_max));
    ui8_adc_battery_target_current = ui8_max (ui8_adc_battery_target_current, ui8_temp);
    // ***********************************************************************************

    // ***********************************************************************************
    // speed limit
    //
    ui8_adc_battery_target_current = (uint8_t) (map ((uint32_t) ui16_wheel_speed_x10,
          (uint32_t) ((configuration_variables.ui8_wheel_max_speed * 10) - 20),
          (uint32_t) ((configuration_variables.ui8_wheel_max_speed * 10) + 20),
          (uint32_t) ui8_adc_battery_target_current,
          (uint32_t) 0));
    // ***********************************************************************************

    // ***********************************************************************************
    // limit the current to max value defined by user on LCD max power, if:
    // - user defined to make that limitation
    // - we are not on boost or fade state
    if ((configuration_variables.ui8_startup_motor_power_boost_limit_to_max_power == 1) ||
        (!((ui8_boost_enable == 1) || (ui8_startup_boost_fade_enable == 1))))
    {
      // now let's limit the target battery current to battery max current (use min value of both)
      ui8_adc_battery_target_current = ui8_min (ui8_adc_battery_target_current, ui8_adc_max_battery_current);
    }
    // ***********************************************************************************

    // ***********************************************************************************
    // reduce battery current if motor over temperature
    //
    if (configuration_variables.ui8_throttle_adc_measures_motor_temperature &&
        configuration_variables.ui8_motor_over_temperature_limit_current)
    {
      // min temperature value can't be equal or higher than max temperature value...
      if (configuration_variables.ui8_motor_temperature_min_value_to_limit >= configuration_variables.ui8_motor_temperature_max_value_to_limit)
      {
        ui8_adc_battery_target_current = 0;
        configuration_variables.ui8_temperature_current_limiting_value = 0;
      }
      else
      {
        // reduce motor current if over temperature
        ui8_adc_battery_target_current = (uint8_t) (map ((uint32_t) configuration_variables.ui16_motor_temperature_x2,
              (uint32_t) (((uint16_t) configuration_variables.ui8_motor_temperature_min_value_to_limit) << 1),
              (uint32_t) (((uint16_t) configuration_variables.ui8_motor_temperature_max_value_to_limit) << 1),
              (uint32_t) ui8_adc_battery_target_current,
              (uint32_t) 0));

        // get a value linear to the current limitation, just to show to user
        configuration_variables.ui8_temperature_current_limiting_value = (uint8_t) (map ((uint32_t) configuration_variables.ui16_motor_temperature_x2,
              (uint32_t) (((uint16_t) configuration_variables.ui8_motor_temperature_min_value_to_limit) << 1),
              (uint32_t) (((uint16_t) configuration_variables.ui8_motor_temperature_max_value_to_limit) << 1),
              (uint32_t) 255,
              (uint32_t) 0));
      }
    }
    else
    {
      // keep ui8_temperature_current_limiting_value = 255 because 255 means no current limiting happening
      configuration_variables.ui8_temperature_current_limiting_value = 255;
    }
    // ***********************************************************************************

    // finally set the target battery current to the current controller
    ebike_app_set_target_adc_battery_max_current (ui8_adc_battery_target_current);

    // set the target duty_cycle to max, as the battery current controller will manage it
    // if battery_target_current == 0, put duty_cycle at 0
    // if ui8_startup_enable == 0, put duty_cycle at 0
    if (ui8_adc_battery_target_current &&
        ui8_startup_enable &&
        (!brake_is_set()))
    {
      motor_set_pwm_duty_cycle_target (255);
    }
    else
    {
      motor_set_pwm_duty_cycle_target (0);
    }
  }  
}

// each 1 unit = 0.625 amps
void ebike_app_set_target_adc_battery_max_current (uint8_t ui8_value)
{
  // limit max number of amps
  if (ui8_value > ui8_adc_battery_current_max)
    ui8_value = ui8_adc_battery_current_max;

  ui8_adc_target_battery_max_current = ui8_adc_battery_current_offset + ui8_value;
}

// in amps
void ebike_app_set_battery_max_current (uint8_t ui8_value)
{
  // each 1 unit = 0.625 amps (0.625 * 256 = 160)
  ui8_adc_battery_current_max = ((((uint16_t) ui8_value) << 8) / 160);

  if (ui8_adc_battery_current_max > ADC_BATTERY_CURRENT_MAX)
    ui8_adc_battery_current_max = ADC_BATTERY_CURRENT_MAX;
}

// This is the interrupt that happens when UART2 receives data. We need it to be the fastest possible and so
// we do: receive every byte and assembly as a package, finally, signal that we have a package to process (on main slow loop)
// and disable the interrupt. The interrupt should be enable again on main loop, after the package being processed
void UART2_IRQHandler(void) __interrupt(UART2_IRQHANDLER)
{
  if(UART2_GetFlagStatus(UART2_FLAG_RXNE) == SET)
  {
    UART2->SR &= (uint8_t)~(UART2_FLAG_RXNE); // this may be redundant

    ui8_byte_received = UART2_ReceiveData8 ();

    switch (ui8_state_machine)
    {
      case 0:
      if (ui8_byte_received == 0x59) // see if we get start package byte
      {
        ui8_rx_buffer[ui8_rx_counter] = ui8_byte_received;
        ui8_rx_counter++;
        ui8_state_machine = 1;
      }
      else
      {
        ui8_rx_counter = 0;
        ui8_state_machine = 0;
      }
      break;

      case 1:
      ui8_rx_buffer[ui8_rx_counter] = ui8_byte_received;
      ui8_rx_counter++;

      // see if is the last byte of the package
      if (ui8_rx_counter > 11)
      {
        ui8_rx_counter = 0;
        ui8_state_machine = 0;
        ui8_received_package_flag = 1; // signal that we have a full package to be processed
        UART2->CR2 &= ~(1 << 5); // disable UART2 receive interrupt
      }
      break;

      default:
      break;
    }
  }
}

void calc_wheel_speed (void)
{
  // calc wheel speed in km/h
  if (ui16_wheel_speed_sensor_pwm_cycles_ticks < WHEEL_SPEED_SENSOR_MIN_PWM_CYCLE_TICKS)
  {
    f_wheel_speed_x10 = ((float) PWM_CYCLES_SECOND) / ((float) ui16_wheel_speed_sensor_pwm_cycles_ticks); // rps
    f_wheel_speed_x10 *= configuration_variables.ui16_wheel_perimeter; // millimeters per second
    f_wheel_speed_x10 *= 0.036; // ((3600 / (1000 * 1000)) * 10) kms per hour * 10
    ui16_wheel_speed_x10 = (uint16_t) f_wheel_speed_x10;
  }
  else
  {
    ui16_wheel_speed_x10 = 0;
  }
}

struct_configuration_variables* get_configuration_variables (void)
{
  return &configuration_variables;
}

void calc_motor_temperature (void)
{
  uint16_t ui16_adc_motor_temperatured_filtered_10b;

  // low pass filter to avoid possible fast spikes/noise
  ui16_adc_motor_temperatured_accumulated -= ui16_adc_motor_temperatured_accumulated >> READ_MOTOR_TEMPERATURE_FILTER_COEFFICIENT;
  ui16_adc_motor_temperatured_accumulated += ui16_adc_read_throttle_10b ();
  ui16_adc_motor_temperatured_filtered_10b = ui16_adc_motor_temperatured_accumulated >> READ_MOTOR_TEMPERATURE_FILTER_COEFFICIENT;

  configuration_variables.ui16_motor_temperature_x2 = (uint16_t) ((float) ui16_adc_motor_temperatured_filtered_10b / 1.024);
  configuration_variables.ui8_motor_temperature = (uint8_t) (configuration_variables.ui16_motor_temperature_x2 >> 1);
}
