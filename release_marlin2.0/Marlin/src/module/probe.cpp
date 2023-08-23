/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * module/probe.cpp
 */

#include "../inc/MarlinConfig.h"

#if HAS_BED_PROBE

#include "probe.h"

#include "../libs/buzzer.h"
#include "motion.h"
#include "temperature.h"
#include "endstops.h"

#include "../gcode/gcode.h"
#include "../lcd/marlinui.h"

#include "../MarlinCore.h" // for stop(), disable_e_steppers(), wait_for_user_response()

#if HAS_LEVELING
  #include "../feature/bedlevel/bedlevel.h"
#endif

#if ENABLED(DELTA)
  #include "delta.h"
#endif

#if ENABLED(BABYSTEP_ZPROBE_OFFSET)
  #include "planner.h"
#endif

#if ENABLED(MEASURE_BACKLASH_WHEN_PROBING)
  #include "../feature/backlash.h"
#endif

#if ENABLED(BLTOUCH)
  #include "../feature/bltouch.h"
#endif

#if ENABLED(HOST_PROMPT_SUPPORT)
  #include "../feature/host_actions.h" // for PROMPT_USER_CONTINUE
#endif

#if HAS_Z_SERVO_PROBE
  #include "servo.h"
#endif

#if EITHER(SENSORLESS_PROBING, SENSORLESS_HOMING)
  #include "stepper.h"
  #include "../feature/tmc_util.h"
#endif

#if HAS_QUIET_PROBING
  #include "stepper/indirection.h"
#endif

#if ENABLED(EXTENSIBLE_UI)
  #include "../lcd/extui/ui_api.h"
#endif

#if ENABLED(ANKER_PROBE_SET)
  #include "../feature/anker/anker_z_offset.h"
#endif

#define DEBUG_OUT ENABLED(DEBUG_LEVELING_FEATURE)
#include "../core/debug_out.h"

#if ADAPT_DETACHED_NOZZLE
#include "../feature/interactive/uart_nozzle_tx.h"
#endif

#if ENABLED(ANKER_PROBE_DETECT_TIMES)
  xy_pos_t M3032_Get_move_away(uint8_t position);
#endif

Probe probe;

xyz_pos_t Probe::offset; // Initialized by settings.load()

#if HAS_PROBE_XY_OFFSET
  const xy_pos_t &Probe::offset_xy = Probe::offset;
#endif

#if ENABLED(SENSORLESS_PROBING)
  Probe::sense_bool_t Probe::test_sensitivity;
#endif

#if ENABLED(ANKER_LEVEING)
    bool anker_leve_pause =true;
#endif

#if ENABLED(Z_PROBE_SLED)

  #ifndef SLED_DOCKING_OFFSET
    #define SLED_DOCKING_OFFSET 0
  #endif

  /**
   * Method to dock/undock a sled designed by Charles Bell.
   *
   * stow[in]     If false, move to MAX_X and engage the solenoid
   *              If true, move to MAX_X and release the solenoid
   */
  static void dock_sled(const bool stow) {
    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("dock_sled(", stow, ")");

    // Dock sled a bit closer to ensure proper capturing
    do_blocking_move_to_x(X_MAX_POS + SLED_DOCKING_OFFSET - ((stow) ? 1 : 0));

    #if HAS_SOLENOID_1 && DISABLED(EXT_SOLENOID)
      WRITE(SOL1_PIN, !stow); // switch solenoid
    #endif
  }

#elif ENABLED(TOUCH_MI_PROBE)

  // Move to the magnet to unlock the probe
  inline void run_deploy_moves_script() {
    #ifndef TOUCH_MI_DEPLOY_XPOS
      #define TOUCH_MI_DEPLOY_XPOS X_MIN_POS
    #elif TOUCH_MI_DEPLOY_XPOS > X_MAX_BED
      TemporaryGlobalEndstopsState unlock_x(false);
    #endif
    #if TOUCH_MI_DEPLOY_YPOS > Y_MAX_BED
      TemporaryGlobalEndstopsState unlock_y(false);
    #endif

    #if ENABLED(TOUCH_MI_MANUAL_DEPLOY)

      const screenFunc_t prev_screen = ui.currentScreen;
      LCD_MESSAGEPGM(MSG_MANUAL_DEPLOY_TOUCHMI);
      ui.return_to_status();

      TERN_(HOST_PROMPT_SUPPORT, host_prompt_do(PROMPT_USER_CONTINUE, PSTR("Deploy TouchMI"), CONTINUE_STR));
      wait_for_user_response();
      ui.reset_status();
      ui.goto_screen(prev_screen);

    #elif defined(TOUCH_MI_DEPLOY_XPOS) && defined(TOUCH_MI_DEPLOY_YPOS)
      do_blocking_move_to_xy(TOUCH_MI_DEPLOY_XPOS, TOUCH_MI_DEPLOY_YPOS);
    #elif defined(TOUCH_MI_DEPLOY_XPOS)
      do_blocking_move_to_x(TOUCH_MI_DEPLOY_XPOS);
    #elif defined(TOUCH_MI_DEPLOY_YPOS)
      do_blocking_move_to_y(TOUCH_MI_DEPLOY_YPOS);
    #endif
  }

  // Move down to the bed to stow the probe
  inline void run_stow_moves_script() {
    const xyz_pos_t oldpos = current_position;
    endstops.enable_z_probe(false);
    do_blocking_move_to_z(TOUCH_MI_RETRACT_Z, homing_feedrate(Z_AXIS));
    do_blocking_move_to(oldpos, homing_feedrate(Z_AXIS));
  }

#elif ENABLED(Z_PROBE_ALLEN_KEY)

  inline void run_deploy_moves_script() {
    #ifdef Z_PROBE_ALLEN_KEY_DEPLOY_1
      #ifndef Z_PROBE_ALLEN_KEY_DEPLOY_1_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_DEPLOY_1_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t deploy_1 = Z_PROBE_ALLEN_KEY_DEPLOY_1;
      do_blocking_move_to(deploy_1, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_DEPLOY_1_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_DEPLOY_2
      #ifndef Z_PROBE_ALLEN_KEY_DEPLOY_2_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_DEPLOY_2_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t deploy_2 = Z_PROBE_ALLEN_KEY_DEPLOY_2;
      do_blocking_move_to(deploy_2, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_DEPLOY_2_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_DEPLOY_3
      #ifndef Z_PROBE_ALLEN_KEY_DEPLOY_3_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_DEPLOY_3_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t deploy_3 = Z_PROBE_ALLEN_KEY_DEPLOY_3;
      do_blocking_move_to(deploy_3, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_DEPLOY_3_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_DEPLOY_4
      #ifndef Z_PROBE_ALLEN_KEY_DEPLOY_4_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_DEPLOY_4_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t deploy_4 = Z_PROBE_ALLEN_KEY_DEPLOY_4;
      do_blocking_move_to(deploy_4, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_DEPLOY_4_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_DEPLOY_5
      #ifndef Z_PROBE_ALLEN_KEY_DEPLOY_5_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_DEPLOY_5_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t deploy_5 = Z_PROBE_ALLEN_KEY_DEPLOY_5;
      do_blocking_move_to(deploy_5, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_DEPLOY_5_FEEDRATE));
    #endif
  }

  inline void run_stow_moves_script() {
    #ifdef Z_PROBE_ALLEN_KEY_STOW_1
      #ifndef Z_PROBE_ALLEN_KEY_STOW_1_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_STOW_1_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t stow_1 = Z_PROBE_ALLEN_KEY_STOW_1;
      do_blocking_move_to(stow_1, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_STOW_1_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_STOW_2
      #ifndef Z_PROBE_ALLEN_KEY_STOW_2_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_STOW_2_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t stow_2 = Z_PROBE_ALLEN_KEY_STOW_2;
      do_blocking_move_to(stow_2, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_STOW_2_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_STOW_3
      #ifndef Z_PROBE_ALLEN_KEY_STOW_3_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_STOW_3_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t stow_3 = Z_PROBE_ALLEN_KEY_STOW_3;
      do_blocking_move_to(stow_3, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_STOW_3_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_STOW_4
      #ifndef Z_PROBE_ALLEN_KEY_STOW_4_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_STOW_4_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t stow_4 = Z_PROBE_ALLEN_KEY_STOW_4;
      do_blocking_move_to(stow_4, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_STOW_4_FEEDRATE));
    #endif
    #ifdef Z_PROBE_ALLEN_KEY_STOW_5
      #ifndef Z_PROBE_ALLEN_KEY_STOW_5_FEEDRATE
        #define Z_PROBE_ALLEN_KEY_STOW_5_FEEDRATE 0.0
      #endif
      constexpr xyz_pos_t stow_5 = Z_PROBE_ALLEN_KEY_STOW_5;
      do_blocking_move_to(stow_5, MMM_TO_MMS(Z_PROBE_ALLEN_KEY_STOW_5_FEEDRATE));
    #endif
  }

#endif // Z_PROBE_ALLEN_KEY

#if HAS_QUIET_PROBING

  #ifndef DELAY_BEFORE_PROBING
    #define DELAY_BEFORE_PROBING 25
  #endif

  void Probe::set_probing_paused(const bool dopause) {
    TERN_(PROBING_HEATERS_OFF, thermalManager.pause_heaters(dopause));
    TERN_(PROBING_FANS_OFF, thermalManager.set_fans_paused(dopause));
    TERN_(PROBING_ESTEPPERS_OFF, if (dopause) disable_e_steppers());
    #if ENABLED(PROBING_STEPPERS_OFF) && DISABLED(DELTA)
      static uint8_t old_trusted;
      if (dopause) {
        old_trusted = axis_trusted;
        DISABLE_AXIS_X();
        DISABLE_AXIS_Y();
      }
      else {
        if (TEST(old_trusted, X_AXIS)) ENABLE_AXIS_X();
        if (TEST(old_trusted, Y_AXIS)) ENABLE_AXIS_Y();
        axis_trusted = old_trusted;
      }
    #endif
    if (dopause) safe_delay(_MAX(DELAY_BEFORE_PROBING, 25));
  }
   #if ENABLED(ANKER_LEVEING)
    void Probe::anker_level_set_probing_paused(const bool dopause,uint16_t ms) {
    TERN_(PROBING_HEATERS_OFF, thermalManager.pause_heaters(dopause));
    TERN_(PROBING_FANS_OFF, thermalManager.set_fans_paused(dopause));
    TERN_(PROBING_ESTEPPERS_OFF, if (dopause) disable_e_steppers());
    #if ENABLED(PROBING_STEPPERS_OFF) && DISABLED(DELTA)
      static uint8_t old_trusted;
      if (dopause) {
        old_trusted = axis_trusted;
        DISABLE_AXIS_X();
        DISABLE_AXIS_Y();
      }
      else {
        if (TEST(old_trusted, X_AXIS)) ENABLE_AXIS_X();
        if (TEST(old_trusted, Y_AXIS)) ENABLE_AXIS_Y();
        axis_trusted = old_trusted;
      }
    #endif
    if (dopause) safe_delay(ms);
  }
  #endif

#endif // HAS_QUIET_PROBING

  #if ENABLED(ANKER_LEVEING)
    void Probe::anker_level_set_probing_paused(const bool dopause,uint16_t ms) {
    TERN_(PROBING_HEATERS_OFF, thermalManager.pause_heaters(dopause));
    TERN_(PROBING_FANS_OFF, thermalManager.set_fans_paused(dopause));
    TERN_(PROBING_ESTEPPERS_OFF, if (dopause) disable_e_steppers());
    #if ENABLED(PROBING_STEPPERS_OFF) && DISABLED(DELTA)
      static uint8_t old_trusted;
      if (dopause) {
        old_trusted = axis_trusted;
        DISABLE_AXIS_X();
        DISABLE_AXIS_Y();
      }
      else {
        if (TEST(old_trusted, X_AXIS)) ENABLE_AXIS_X();
        if (TEST(old_trusted, Y_AXIS)) ENABLE_AXIS_Y();
        axis_trusted = old_trusted;
      }
    #endif
    if (dopause) safe_delay(ms);
  }
  #endif
/**
 * Raise Z to a minimum height to make room for a probe to move
 */
void Probe::do_z_raise(const float z_raise) {
  if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("Probe::do_z_raise(", z_raise, ")");
  float z_dest = z_raise;
  if (offset.z < 0) z_dest -= offset.z;
    #if ENABLED(WS1_HOMING_5X)
     WS1_do_z_clearance(z_dest);
    #else
     do_z_clearance(z_dest);
    #endif
}

FORCE_INLINE void probe_specific_action(const bool deploy) {
  #if ENABLED(PAUSE_BEFORE_DEPLOY_STOW)
    do {
      #if ENABLED(PAUSE_PROBE_DEPLOY_WHEN_TRIGGERED)
        if (deploy != PROBE_TRIGGERED()) break;
      #endif

      BUZZ(100, 659);
      BUZZ(100, 698);

      PGM_P const ds_str = deploy ? GET_TEXT(MSG_MANUAL_DEPLOY) : GET_TEXT(MSG_MANUAL_STOW);
      ui.return_to_status();       // To display the new status message
      ui.set_status_P(ds_str, 99);
      SERIAL_ECHOLNPGM_P(ds_str);

      TERN_(HOST_PROMPT_SUPPORT, host_prompt_do(PROMPT_USER_CONTINUE, PSTR("Stow Probe"), CONTINUE_STR));
      TERN_(EXTENSIBLE_UI, ExtUI::onUserConfirmRequired_P(PSTR("Stow Probe")));

      wait_for_user_response();
      ui.reset_status();

    } while (ENABLED(PAUSE_PROBE_DEPLOY_WHEN_TRIGGERED));

  #endif // PAUSE_BEFORE_DEPLOY_STOW

  #if ENABLED(SOLENOID_PROBE)

    #if HAS_SOLENOID_1
      WRITE(SOL1_PIN, deploy);
    #endif

  #elif ENABLED(Z_PROBE_SLED)

    dock_sled(!deploy);

  #elif ENABLED(BLTOUCH)

    deploy ? bltouch.deploy() : bltouch.stow();

  #elif HAS_Z_SERVO_PROBE

    MOVE_SERVO(Z_PROBE_SERVO_NR, servo_angles[Z_PROBE_SERVO_NR][deploy ? 0 : 1]);

  #elif EITHER(TOUCH_MI_PROBE, Z_PROBE_ALLEN_KEY)

    deploy ? run_deploy_moves_script() : run_stow_moves_script();

  #elif ENABLED(RACK_AND_PINION_PROBE)

    do_blocking_move_to_x(deploy ? Z_PROBE_DEPLOY_X : Z_PROBE_RETRACT_X);

  #elif DISABLED(PAUSE_BEFORE_DEPLOY_STOW)

    UNUSED(deploy);

  #endif
}

#if EITHER(PREHEAT_BEFORE_PROBING, PREHEAT_BEFORE_LEVELING)

  #if ENABLED(PREHEAT_BEFORE_PROBING)
    #ifndef PROBING_NOZZLE_TEMP
      #define PROBING_NOZZLE_TEMP 0
    #endif
    #ifndef PROBING_BED_TEMP
      #define PROBING_BED_TEMP 0
    #endif
  #endif

  /**
   * Do preheating as required before leveling or probing.
   *  - If a preheat input is higher than the current target, raise the target temperature.
   *  - If a preheat input is higher than the current temperature, wait for stabilization.
   */
  void Probe::preheat_for_probing(const celsius_t hotend_temp, const celsius_t bed_temp) {
    #if HAS_HOTEND && (PROBING_NOZZLE_TEMP || LEVELING_NOZZLE_TEMP)
      #define WAIT_FOR_NOZZLE_HEAT
    #endif
    #if HAS_HEATED_BED && (PROBING_BED_TEMP || LEVELING_BED_TEMP)
      #define WAIT_FOR_BED_HEAT
    #endif

    DEBUG_ECHOPGM("Preheating ");

    #if ENABLED(WAIT_FOR_NOZZLE_HEAT)
      const celsius_t hotendPreheat = hotend_temp > thermalManager.degTargetHotend(0) ? hotend_temp : 0;
      if (hotendPreheat) {
        DEBUG_ECHOPAIR("hotend (", hotendPreheat, ")");
        thermalManager.setTargetHotend(hotendPreheat, 0);
      }
    #elif ENABLED(WAIT_FOR_BED_HEAT)
      constexpr celsius_t hotendPreheat = 0;
    #endif

    #if ENABLED(WAIT_FOR_BED_HEAT)
      const celsius_t bedPreheat = bed_temp > thermalManager.degTargetBed() ? bed_temp : 0;
      if (bedPreheat) {
        if (hotendPreheat) DEBUG_ECHOPGM(" and ");
        DEBUG_ECHOPAIR("bed (", bedPreheat, ")");
        thermalManager.setTargetBed(bedPreheat);
      }
    #endif

    DEBUG_EOL();

    TERN_(WAIT_FOR_NOZZLE_HEAT, if (hotend_temp > thermalManager.wholeDegHotend(0) + (TEMP_WINDOW)) thermalManager.wait_for_hotend(0));
    TERN_(WAIT_FOR_BED_HEAT,    if (bed_temp    > thermalManager.wholeDegBed() + (TEMP_BED_WINDOW)) thermalManager.wait_for_bed_heating());
  }

#endif

/**
 * Attempt to deploy or stow the probe
 *
 * Return TRUE if the probe could not be deployed/stowed
 */
bool Probe::set_deployed(const bool deploy) {

  if (DEBUGGING(LEVELING)) {
    DEBUG_POS("Probe::set_deployed", current_position);
    DEBUG_ECHOLNPAIR("deploy: ", deploy);
  }

  if (endstops.z_probe_enabled == deploy) return false;

  // Make room for probe to deploy (or stow)
  // Fix-mounted probe should only raise for deploy
  // unless PAUSE_BEFORE_DEPLOY_STOW is enabled
  #if EITHER(FIX_MOUNTED_PROBE, NOZZLE_AS_PROBE) && DISABLED(PAUSE_BEFORE_DEPLOY_STOW)
    const bool z_raise_wanted = deploy;
  #else
    constexpr bool z_raise_wanted = true;
  #endif

  if (z_raise_wanted)
    do_z_raise(_MAX(Z_CLEARANCE_BETWEEN_PROBES, Z_CLEARANCE_DEPLOY_PROBE));

  #if EITHER(Z_PROBE_SLED, Z_PROBE_ALLEN_KEY)
    if (homing_needed_error(TERN_(Z_PROBE_SLED, _BV(X_AXIS)))) {
      SERIAL_ERROR_MSG(STR_STOP_UNHOMED);
      stop();
      return true;
    }
  #endif

  const xy_pos_t old_xy = current_position;

  #if ENABLED(PROBE_TRIGGERED_WHEN_STOWED_TEST)

    // Only deploy/stow if needed
    if (PROBE_TRIGGERED() == deploy) {
      if (!deploy) endstops.enable_z_probe(false); // Switch off triggered when stowed probes early
                                                   // otherwise an Allen-Key probe can't be stowed.
      probe_specific_action(deploy);
    }

    if (PROBE_TRIGGERED() == deploy) {             // Unchanged after deploy/stow action?
      if (IsRunning()) {
        SERIAL_ERROR_MSG("Z-Probe failed");
        LCD_ALERTMESSAGEPGM_P(PSTR("Err: ZPROBE"));
      }
      stop();
      return true;
    }

  #else

    probe_specific_action(deploy);
  #endif

  // If preheating is required before any probing...
  TERN_(PREHEAT_BEFORE_PROBING, if (deploy) preheat_for_probing(PROBING_NOZZLE_TEMP, PROBING_BED_TEMP));

  do_blocking_move_to(old_xy);
  endstops.enable_z_probe(deploy);
  return false;
}

#if ENABLED(ANKER_Z_OFFSET_FUNC)
bool Probe::anker_set_deployed(const bool deploy) {

  if (DEBUGGING(LEVELING)) {
    DEBUG_POS("Probe::set_deployed", current_position);
    DEBUG_ECHOLNPAIR("deploy: ", deploy);
  }

  if (endstops.z_probe_enabled == deploy) return false;

  // Make room for probe to deploy (or stow)
  // Fix-mounted probe should only raise for deploy
  // unless PAUSE_BEFORE_DEPLOY_STOW is enabled
  #if EITHER(FIX_MOUNTED_PROBE, NOZZLE_AS_PROBE) && DISABLED(PAUSE_BEFORE_DEPLOY_STOW)
    //const bool z_raise_wanted = deploy;
  #else
    constexpr bool z_raise_wanted = true;
  #endif

  // if (z_raise_wanted)
  //   do_z_raise(_MAX(Z_CLEARANCE_BETWEEN_PROBES, Z_CLEARANCE_DEPLOY_PROBE));

  #if EITHER(Z_PROBE_SLED, Z_PROBE_ALLEN_KEY)
    if (homing_needed_error(TERN_(Z_PROBE_SLED, _BV(X_AXIS)))) {
      SERIAL_ERROR_MSG(STR_STOP_UNHOMED);
      stop();
      return true;
    }
  #endif

  const xy_pos_t old_xy = current_position;

  #if ENABLED(PROBE_TRIGGERED_WHEN_STOWED_TEST)

    // Only deploy/stow if needed
    if (PROBE_TRIGGERED() == deploy) {
      if (!deploy) endstops.enable_z_probe(false); // Switch off triggered when stowed probes early
                                                   // otherwise an Allen-Key probe can't be stowed.
      probe_specific_action(deploy);
    }

    if (PROBE_TRIGGERED() == deploy) {             // Unchanged after deploy/stow action?
      if (IsRunning()) {
        SERIAL_ERROR_MSG("Z-Probe failed");
        LCD_ALERTMESSAGEPGM_P(PSTR("Err: ZPROBE"));
      }
      stop();
      return true;
    }

  #else

    probe_specific_action(deploy);
  #endif

  // If preheating is required before any probing...
  TERN_(PREHEAT_BEFORE_PROBING, if (deploy) preheat_for_probing(PROBING_NOZZLE_TEMP, PROBING_BED_TEMP));

  do_blocking_move_to(old_xy);
  endstops.enable_z_probe(deploy);
  return false;
}

#endif

#if ENABLED(WS1_HOMING_5X)
/**
 * Attempt to deploy or stow the probe
 *
 * Return TRUE if the probe could not be deployed/stowed
 */
bool Probe::anker_set_deployed(const bool deploy) {

  if (DEBUGGING(LEVELING)) {
    DEBUG_POS("Probe::set_deployed", current_position);
    DEBUG_ECHOLNPAIR("deploy: ", deploy);
  }

  if (endstops.z_probe_enabled == deploy) return false;

  // Make room for probe to deploy (or stow)
  // Fix-mounted probe should only raise for deploy
  // unless PAUSE_BEFORE_DEPLOY_STOW is enabled
  #if EITHER(FIX_MOUNTED_PROBE, NOZZLE_AS_PROBE) && DISABLED(PAUSE_BEFORE_DEPLOY_STOW)
    const bool z_raise_wanted = deploy;
  #else
    constexpr bool z_raise_wanted = true;
  #endif

  if (z_raise_wanted)
    //do_z_raise(_MAX(Z_CLEARANCE_BETWEEN_PROBES, Z_CLEARANCE_DEPLOY_PROBE));
    do_z_raise(HOMING_PROBE_Z_RISE);

  #if EITHER(Z_PROBE_SLED, Z_PROBE_ALLEN_KEY)
    if (homing_needed_error(TERN_(Z_PROBE_SLED, _BV(X_AXIS)))) {
      SERIAL_ERROR_MSG(STR_STOP_UNHOMED);
      stop();
      return true;
    }
  #endif

  const xy_pos_t old_xy = current_position;

  #if ENABLED(PROBE_TRIGGERED_WHEN_STOWED_TEST)

    // Only deploy/stow if needed
    if (PROBE_TRIGGERED() == deploy) {
      if (!deploy) endstops.enable_z_probe(false); // Switch off triggered when stowed probes early
                                                   // otherwise an Allen-Key probe can't be stowed.
      probe_specific_action(deploy);
    }

    if (PROBE_TRIGGERED() == deploy) {             // Unchanged after deploy/stow action?
      if (IsRunning()) {
        SERIAL_ERROR_MSG("Z-Probe failed");
        LCD_ALERTMESSAGEPGM_P(PSTR("Err: ZPROBE"));
      }
      stop();
      return true;
    }

  #else

    probe_specific_action(deploy);
  #endif

  // If preheating is required before any probing...
  TERN_(PREHEAT_BEFORE_PROBING, if (deploy) preheat_for_probing(PROBING_NOZZLE_TEMP, PROBING_BED_TEMP));

  do_blocking_move_to(old_xy);
  endstops.enable_z_probe(deploy);
  return false;
}

#endif
/**
 * @brief Used by run_z_probe to do a single Z probe move.
 *
 * @param  z        Z destination
 * @param  fr_mm_s  Feedrate in mm/s
 * @return true to indicate an error
 */

/**
 * @brief Move down until the probe triggers or the low limit is reached
 *
 * @details Used by run_z_probe to get each bed Z height measurement.
 *          Sets current_position.z to the height where the probe triggered
 *          (according to the Z stepper count). The float Z is propagated
 *          back to the planner.position to preempt any rounding error.
 *
 * @return TRUE if the probe failed to trigger.
 */
bool Probe::probe_down_to_z(const_float_t z, const_feedRate_t fr_mm_s) {
  DEBUG_SECTION(log_probe, "Probe::probe_down_to_z", DEBUGGING(LEVELING));

  #if BOTH(HAS_HEATED_BED, WAIT_FOR_BED_HEATER)
    thermalManager.wait_for_bed_heating();
  #endif

  #if BOTH(HAS_TEMP_HOTEND, WAIT_FOR_HOTEND)
    thermalManager.wait_for_hotend_heating(active_extruder);
  #endif

  if (TERN0(BLTOUCH_SLOW_MODE, bltouch.deploy())) return true; // Deploy in LOW SPEED MODE on every probe action
  // Disable stealthChop if used. Enable diag1 pin on driver.
  #if ENABLED(SENSORLESS_PROBING)
    sensorless_t stealth_states { false };
    #if ENABLED(DELTA)
      if (probe.test_sensitivity.x) stealth_states.x = tmc_enable_stallguard(stepperX);  // Delta watches all DIAG pins for a stall
      if (probe.test_sensitivity.y) stealth_states.y = tmc_enable_stallguard(stepperY);
    #endif
    if (probe.test_sensitivity.z) 
    {
       #if ENABLED(USE_Z_SENSORLESS)
        anker_tmc2209.tmc_enable_stallguard(stepperZ,anker_tmc2209.thrs_z1); 
       #else
        stealth_states.z = tmc_enable_stallguard(stepperZ); 
       #endif
    }   // All machines will check Z-DIAG for stall
    endstops.enable(true);
    set_homing_current(true);                                 // The "homing" current also applies to probing
  #endif
  #if ENABLED(USE_Z_SENSORLESS_AS_PROBE)
    TERN_(ANKER_FIX_ENDSTOPR, endstops.set_anker_endstop(2));

    #if ENABLED(USE_Z_SENSORLESS)
      anker_tmc2209.tmc_enable_stallguard(stepperZ,anker_tmc2209.thrs_z1); 
      #ifdef Z2_STALL_SENSITIVITY
        anker_tmc2209.tmc_enable_stallguard(stepperZ2,anker_tmc2209.thrs_z2); 
      #endif
    #endif
      endstops.enable(true);
      set_homing_current(true);  
  #endif
  
  #if ENABLED(PROVE_CONTROL)
      digitalWrite(PROVE_CONTROL_PIN, !PROVE_CONTROL_STATE);
  #endif
  #if ENABLED(ANKER_LEVEING)
    if(anker_leve_pause) 
     TERN_(HAS_QUIET_PROBING, anker_level_set_probing_paused(true,ANKER_LEVEING_DELAY_BEFORE_PROBING));
    else
     TERN_(HAS_QUIET_PROBING, set_probing_paused(true));
  #else
    TERN_(HAS_QUIET_PROBING, set_probing_paused(true));
  #endif
  #if ENABLED(PROVE_CONTROL)
      digitalWrite(PROVE_CONTROL_PIN, PROVE_CONTROL_STATE);
  #endif

  // Move down until the probe is triggered
  do_blocking_move_to_z(z, fr_mm_s);

  // Check to see if the probe was triggered
  const bool probe_triggered =
    #if BOTH(DELTA, SENSORLESS_PROBING)
      endstops.trigger_state() & (_BV(X_MAX) | _BV(Y_MAX) | _BV(Z_MAX))
    #else
      TEST(endstops.trigger_state(), Z_MIN_PROBE)
    #endif
  ;

  #if ENABLED(PROVE_CONTROL)
    digitalWrite(PROVE_CONTROL_PIN, !PROVE_CONTROL_STATE);
  #endif

  TERN_(HAS_QUIET_PROBING, set_probing_paused(false));

  // Re-enable stealthChop if used. Disable diag1 pin on driver.
  #if ENABLED(SENSORLESS_PROBING)
    endstops.not_homing();
    #if ENABLED(DELTA)
      if (probe.test_sensitivity.x) tmc_disable_stallguard(stepperX, stealth_states.x);
      if (probe.test_sensitivity.y) tmc_disable_stallguard(stepperY, stealth_states.y);
    #endif
    if (probe.test_sensitivity.z) 
    {
       #if ENABLED(USE_Z_SENSORLESS)
          anker_tmc2209.tmc_disable_stallguard(stepperZ, stealth_states.z); 
       #else
         tmc_disable_stallguard(stepperZ, stealth_states.z);
       #endif
    }
    set_homing_current(false);
  #endif
  
  #if ENABLED(USE_Z_SENSORLESS_AS_PROBE)
    endstops.not_homing();
    set_homing_current(false);
  #endif

  if (probe_triggered && TERN0(BLTOUCH_SLOW_MODE, bltouch.stow())) // Stow in LOW SPEED MODE on every trigger
    return true;
  
  // Clear endstop flags
  endstops.hit_on_purpose();

  // Get Z where the steppers were interrupted
  set_current_from_steppers_for_axis(Z_AXIS);

  // Tell the planner where we actually are
  sync_plan_position();

  return !probe_triggered;
}
#if ENABLED(ANKER_Z_OFFSET_FUNC)
  bool Probe::anker_z_offset_probe_down_to_z(const_float_t z, const_feedRate_t fr_mm_s) {
    DEBUG_SECTION(log_probe, "Probe::probe_down_to_z", DEBUGGING(LEVELING));

    // Move down until the probe is triggered
    do_blocking_move_to_z(z, fr_mm_s);

    // Get Z where the steppers were interrupted
    set_current_from_steppers_for_axis(Z_AXIS);

    // Tell the planner where we actually are
    sync_plan_position();

    return false;
  }
#endif
#if ENABLED(PROBE_TARE)

  /**
   * @brief Init the tare pin
   *
   * @details Init tare pin to ON state for a strain gauge, otherwise OFF
   */
  void Probe::tare_init() {
    #if ENABLED(PROVE_CONTROL)
      OUT_WRITE(PROBE_TARE_PIN, PROBE_TARE_STATE);
    #else
      OUT_WRITE(PROBE_TARE_PIN, !PROBE_TARE_STATE);
    #endif
  }

  /**
   * @brief Tare the Z probe
   *
   * @details Signal to the probe to tare itself
   *
   * @return TRUE if the tare cold not be completed
   */
  bool Probe::tare() {
    // #if BOTH(PROBE_ACTIVATION_SWITCH, PROBE_TARE_ONLY_WHILE_INACTIVE)
    //   if (endstops.probe_switch_activated()) {
    //     SERIAL_ECHOLNPGM("Cannot tare an active probe");
    //     return true;
    //   }
    // #endif
    
    SERIAL_ECHOLNPGM("Taring probe");
    WRITE(PROBE_TARE_PIN, PROBE_TARE_STATE);
    delay(PROBE_TARE_TIME);
    WRITE(PROBE_TARE_PIN, !PROBE_TARE_STATE);
    delay(PROBE_TARE_DELAY);

    endstops.hit_on_purpose();
    return false;
  }
#endif

#if ENABLED(ANKER_PROBE_DETECT_TIMES)

void insertion_sort(float arr[], int16_t len){
  int16_t i, j;
  float key;
  for (i = 1; i < len; i++){
    key = arr[i];
    j = i-1;
    while((j >= 0) && (arr[j] > key)) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

#endif
/**
 * @brief Probe at the current XY (possibly more than once) to find the bed Z.
 *
 * @details Used by probe_at_point to get the bed Z height at the current XY.
 *          Leaves current_position.z at the height where the probe triggered.
 *
 * @return The Z position of the bed at the current XY or NAN on error.
 */
float Probe::run_z_probe(const bool sanity_check/*=true*/) {
  DEBUG_SECTION(log_probe, "Probe::run_z_probe", DEBUGGING(LEVELING));

  auto try_to_probe = [&](PGM_P const plbl, const_float_t z_probe_low_point, const feedRate_t fr_mm_s, const bool scheck, const float clearance) -> bool {
    // Tare the probe, if supported
    if (TERN0(PROBE_TARE, tare())) return true;

    // Do a first probe at the fast speed
    const bool probe_fail = probe_down_to_z(z_probe_low_point, fr_mm_s),            // No probe trigger?
               early_fail = (scheck && current_position.z > -offset.z + clearance); // Probe triggered too high?
    #if ENABLED(DEBUG_LEVELING_FEATURE)
      if (probe_fail || early_fail) {
        DEBUG_ECHOPGM_P(plbl);
        SERIAL_ECHO(" Probe fail! -");
        if (probe_fail) SERIAL_ECHO(" No trigger.");
        if (early_fail) SERIAL_ECHO(" Triggered early.");
        SERIAL_EOL();
      }
    #else
      UNUSED(plbl);
    #endif
    return probe_fail || early_fail;
  };

  // Stop the probe before it goes too low to prevent damage.
  // If Z isn't known then probe to -10mm.
  const float z_probe_low_point = axis_is_trusted(Z_AXIS) ? -offset.z + Z_PROBE_LOW_POINT : -10.0;

  // Double-probing does a fast probe followed by a slow probe
  #if TOTAL_PROBING == 2

    // Attempt to tare the probe
    if (TERN0(PROBE_TARE, tare())) return NAN;
    
    #if ENABLED(ANKER_PROBE_SET)
      anker_probe_set.probe_start(anker_probe_set.leveing_value);
    #endif

    // Do a first probe at the fast speed
    if (try_to_probe(PSTR("FAST"), z_probe_low_point, z_probe_fast_mm_s,
                     sanity_check, Z_CLEARANCE_BETWEEN_PROBES) ) return NAN;
    
    #if ENABLED(ANKER_PROBE_DETECT_TIMES)
      float first_probe_z = current_position.z;
    #else
      const float first_probe_z = current_position.z;
    #endif

    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("1st Probe Z:", first_probe_z);
    
    // Raise to give the probe clearance
    #if ENABLED(PROVE_CONTROL)||ENABLED(ANKER_PROBE_SET)
     do_blocking_move_to_z(current_position.z + Z_CLEARANCE_MULTI_PROBE, MMM_TO_MMS(HOMING_RISE_SPEED));
    #else
     do_blocking_move_to_z(current_position.z + Z_CLEARANCE_MULTI_PROBE, z_probe_fast_mm_s);
    #endif

  #elif Z_PROBE_FEEDRATE_FAST != Z_PROBE_FEEDRATE_SLOW

    // If the nozzle is well over the travel height then
    // move down quickly before doing the slow probe
    const float z = Z_CLEARANCE_DEPLOY_PROBE + 5.0 + (offset.z < 0 ? -offset.z : 0);
    if (current_position.z > z) {
      // Probe down fast. If the probe never triggered, raise for probe clearance
      if (!probe_down_to_z(z, z_probe_fast_mm_s))
        do_blocking_move_to_z(current_position.z + Z_CLEARANCE_BETWEEN_PROBES, z_probe_fast_mm_s);
    }
  #endif

  #if EXTRA_PROBING > 0
    float probes[TOTAL_PROBING];
  #endif

  #if TOTAL_PROBING > 2
    float probes_z_sum = 0;
    for (
      #if EXTRA_PROBING > 0
        uint8_t p = 0; p < TOTAL_PROBING; p++
      #else
        uint8_t p = TOTAL_PROBING; p--;
      #endif
    )
  #endif
    {
      // If the probe won't tare, return
      if (TERN0(PROBE_TARE, tare())) return true;
      
      // Probe downward slowly to find the bed
      #if ENABLED(ANKER_PROBE_DETECT_TIMES)

        //#define Z_PROBE_DETECTION_DEVIATION 0.06f  // Acceptable deviation between detections
        uint8_t insert_count = 0;
        uint8_t try_again;
        //const xy_pos_t move_away[4] = {{-1.0, -1.0}, {2.0, 0.0}, {0.0, 2.0}, {-2.0, 0.0}};
        static float buff_insert[12];
        buff_insert[insert_count] = first_probe_z;
        for(try_again = 0; try_again <= 4; try_again++)
        {
            TERN_(ANKER_PROBE_SET, anker_probe_set.probe_start(anker_probe_set.leveing_value));

            if (try_to_probe(PSTR("SLOW"), z_probe_low_point, MMM_TO_MMS(Z_PROBE_FEEDRATE_SLOW),
                            sanity_check, Z_CLEARANCE_MULTI_PROBE) ) return NAN;
            const float second_probe_z = current_position.z;
            MYSERIAL2.printLine("echo: num:%d Probe Z:%3.5f %3.5f diff:%3.5f\r\n", insert_count, first_probe_z, second_probe_z, (first_probe_z - second_probe_z));
            insert_count++;
            buff_insert[insert_count] = second_probe_z;
            if(ABS(first_probe_z - second_probe_z) < Z_PROBE_DETECTION_DEVIATION || try_again >= 4)
              {break;} // OK or tried many times.
            else{ // try again
              do_blocking_move_to_z(current_position.z + Z_CLEARANCE_MULTI_PROBE, MMM_TO_MMS(HOMING_RISE_SPEED));
              safe_delay(50);
            }
          
            // Slightly move the X/Y axis and try again.
            xyze_pos_t dest;
            const xy_pos_t move_away = M3032_Get_move_away(try_again);
            dest.x = move_away.x + current_position[X_AXIS];
            dest.y = move_away.y + current_position[Y_AXIS];
            do_blocking_move_to_xy((xy_pos_t)dest, MMM_TO_MMS(HOMING_RISE_SPEED));
            TERN_(ANKER_PROBE_SET, anker_probe_set.probe_start(anker_probe_set.leveing_value));
            // Do a first probe at the fast speed
            if (try_to_probe(PSTR("FAST"), z_probe_low_point, z_probe_fast_mm_s,
                     sanity_check, Z_CLEARANCE_BETWEEN_PROBES) ) return NAN;
            first_probe_z = current_position.z;
            insert_count++;
            buff_insert[insert_count] = first_probe_z;
            do_blocking_move_to_z(current_position.z + Z_CLEARANCE_MULTI_PROBE, MMM_TO_MMS(HOMING_RISE_SPEED));
            safe_delay(50);
        }

        insertion_sort(buff_insert, insert_count+1);
        
        if(try_again >= 4){
          MYSERIAL2.printLine("echo: run_z_probe-attempts_err=%3.5f\r\n", current_position.z);
          safe_delay(50);
          float probes_z_sum = 0;
          if(insert_count > 4){
            for(uint8_t i=2; i<=insert_count-2; i++)
            {
              probes_z_sum += buff_insert[i];
            }
            const float measured_z = probes_z_sum * RECIPROCAL(insert_count-3);
            MYSERIAL2.printLine("insert_count > 4 measured_z=%3.5f\r\n", measured_z);
            return measured_z;
          }else{
            MYSERIAL2.printLine("insert_count =< 4 measured_z=%3.5f\r\n", buff_insert[insert_count]);
            return buff_insert[insert_count];
          }
        }

      #else // ! ENABLED(ANKER_PROBE_DETECT_TIMES)
        #if ENABLED(ANKER_PROBE_SET)
          anker_probe_set.probe_start(anker_probe_set.leveing_value);
        #endif
        // Probe downward slowly to find the bed
        if (try_to_probe(PSTR("SLOW"), z_probe_low_point, MMM_TO_MMS(Z_PROBE_FEEDRATE_SLOW),
                        sanity_check, Z_CLEARANCE_MULTI_PROBE) ) return NAN;
      #endif

      
      if(anker_probe_set.point_test_flag)
      {
        anker_probe_set.point_test_idle();
      }

      TERN_(MEASURE_BACKLASH_WHEN_PROBING, backlash.measure_with_probe());

      const float z = current_position.z;

      #if EXTRA_PROBING > 0
        // Insert Z measurement into probes[]. Keep it sorted ascending.
        LOOP_LE_N(i, p) {                            // Iterate the saved Zs to insert the new Z
          if (i == p || probes[i] > z) {                              // Last index or new Z is smaller than this Z
            for (int8_t m = p; --m >= i;) probes[m + 1] = probes[m];  // Shift items down after the insertion point
            probes[i] = z;                                            // Insert the new Z measurement
            break;                                                    // Only one to insert. Done!
          }
        }
      #elif TOTAL_PROBING > 2
        probes_z_sum += z;
      #else
        UNUSED(z);
      #endif

      #if TOTAL_PROBING > 2
        // Small Z raise after all but the last probe
        if (p
          #if EXTRA_PROBING > 0
            < TOTAL_PROBING - 1
          #endif
        ) do_blocking_move_to_z(z + Z_CLEARANCE_MULTI_PROBE, z_probe_fast_mm_s);
      #endif
    }

  #if TOTAL_PROBING > 2

    #if EXTRA_PROBING > 0
      // Take the center value (or average the two middle values) as the median
      static constexpr int PHALF = (TOTAL_PROBING - 1) / 2;
      const float middle = probes[PHALF],
                  median = ((TOTAL_PROBING) & 1) ? middle : (middle + probes[PHALF + 1]) * 0.5f;

      // Remove values farthest from the median
      uint8_t min_avg_idx = 0, max_avg_idx = TOTAL_PROBING - 1;
      for (uint8_t i = EXTRA_PROBING; i--;)
        if (ABS(probes[max_avg_idx] - median) > ABS(probes[min_avg_idx] - median))
          max_avg_idx--; else min_avg_idx++;

      // Return the average value of all remaining probes.
      LOOP_S_LE_N(i, min_avg_idx, max_avg_idx)
        probes_z_sum += probes[i];

    #endif

    const float measured_z = probes_z_sum * RECIPROCAL(MULTIPLE_PROBING);

  #elif TOTAL_PROBING == 2

    const float z2 = current_position.z;

    if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPAIR("2nd Probe Z:", z2, " Discrepancy:", first_probe_z - z2);
    
    #if ENABLED(ANKER_PROBE_SET)
        // Return a weighted average of the fast and slow probes
        const float measured_z = z2;
    #else
      // Return a weighted average of the fast and slow probes
        const float measured_z = (z2 * 3.0 + first_probe_z * 2.0) * 0.2;
    #endif

  #else

    // Return the single probe result
    const float measured_z = current_position.z;

  #endif
  return measured_z;
}
#if ENABLED(ANKER_Z_OFFSET_FUNC)
#include "../feature/anker/anker_z_offset.h"
float Probe::anker_z_offset_run_z_probe(const bool sanity_check/*=true*/) {
  DEBUG_SECTION(log_probe, "Probe::run_z_probe", DEBUGGING(LEVELING));

  auto try_to_probe = [&](PGM_P const plbl, const_float_t z_probe_low_point, const feedRate_t fr_mm_s, const bool scheck, const float clearance) -> bool {

    // Do a first probe at the fast speed
    const bool probe_fail = anker_z_offset_probe_down_to_z(z_probe_low_point, fr_mm_s),            // No probe trigger?
               early_fail = (scheck && current_position.z > -offset.z + clearance); // Probe triggered too high?
    #if ENABLED(DEBUG_LEVELING_FEATURE)
      if (DEBUGGING(LEVELING) && (probe_fail || early_fail)) {
        DEBUG_ECHOPGM_P(plbl);
        DEBUG_ECHOPGM(" Probe fail! -");
        if (probe_fail) DEBUG_ECHOPGM(" No trigger.");
        if (early_fail) DEBUG_ECHOPGM(" Triggered early.");
        DEBUG_EOL();
      }
    #else
      UNUSED(plbl);
    #endif
    return probe_fail || early_fail;
  };

  // Stop the probe before it goes too low to prevent damage.
  // If Z isn't known then probe to -10mm.
  const float z_probe_low_point = axis_is_trusted(Z_AXIS) ? -offset.z + Z_PROBE_LOW_POINT : -10.0;
  
  if(anker_z_offset.cs1237_start_convert)
  {
  // Probe downward slowly to find the bed
  if (try_to_probe(PSTR("SLOW"), z_probe_low_point, MMM_TO_MMS(ANKER_Z_PROBE_FEEDRATE_SLOW),
                    sanity_check, Z_CLEARANCE_MULTI_PROBE) ) return NAN;
  }
  else
  {
  // Probe downward slowly to find the bed
  if (try_to_probe(PSTR("SLOW"), z_probe_low_point, MMM_TO_MMS(Z_PROBE_FEEDRATE_FAST),
                    sanity_check, Z_CLEARANCE_MULTI_PROBE) ) return NAN;
  }

  const float z = current_position.z;

  UNUSED(z);

  // Return the single probe result
  const float measured_z = current_position.z;

  return measured_z;
}
#endif

/**
 * - Move to the given XY
 * - Deploy the probe, if not already deployed
 * - Probe the bed, get the Z position
 * - Depending on the 'stow' flag
 *   - Stow the probe, or
 *   - Raise to the BETWEEN height
 * - Return the probed Z position
 */
float Probe::probe_at_point(const_float_t rx, const_float_t ry, const ProbePtRaise raise_after/*=PROBE_PT_NONE*/, const uint8_t verbose_level/*=0*/, const bool probe_relative/*=true*/, const bool sanity_check/*=true*/) {
  DEBUG_SECTION(log_probe, "Probe::probe_at_point", DEBUGGING(LEVELING));

  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOLNPAIR(
      "...(", LOGICAL_X_POSITION(rx), ", ", LOGICAL_Y_POSITION(ry),
      ", ", raise_after == PROBE_PT_RAISE ? "raise" : raise_after == PROBE_PT_LAST_STOW ? "stow (last)" : raise_after == PROBE_PT_STOW ? "stow" : "none",
      ", ", verbose_level,
      ", ", probe_relative ? "probe" : "nozzle", "_relative)"
    );
    DEBUG_POS("", current_position);
  }

  #if BOTH(BLTOUCH, BLTOUCH_HS_MODE)
    if (bltouch.triggered()) bltouch._reset();
  #endif

  // On delta keep Z below clip height or do_blocking_move_to will abort
  xyz_pos_t npos = { rx, ry, _MIN(TERN(DELTA, delta_clip_start_height, current_position.z), current_position.z) };
  if (probe_relative) {                                     // The given position is in terms of the probe
    if (!can_reach(npos)) {
      if (DEBUGGING(LEVELING)) DEBUG_ECHOLNPGM("Position Not Reachable");
      return NAN;
    }
    npos -= offset_xy;                                      // Get the nozzle position
  }
  else if (!position_is_reachable(npos)) return NAN;        // The given position is in terms of the nozzle

  // Move the probe to the starting XYZ
  do_blocking_move_to(npos, feedRate_t(XY_PROBE_FEEDRATE_MM_S));

  #if ENABLED(PROVE_CONTROL)
    digitalWrite(PROVE_CONTROL_PIN, !PROVE_CONTROL_STATE);
    //_delay_ms(200);
  #endif

  float measured_z = NAN;
  if (!deploy()) measured_z = run_z_probe(sanity_check) + offset.z;

  if (!isnan(measured_z)) {
    const bool big_raise = raise_after == PROBE_PT_BIG_RAISE;
    if (big_raise || raise_after == PROBE_PT_RAISE)
    {
      #if ENABLED(PROVE_CONTROL)|| ENABLED(ANKER_PROBE_SET)
       do_blocking_move_to_z(current_position.z + (big_raise ? 25 : Z_CLEARANCE_BETWEEN_PROBES),  MMM_TO_MMS(HOMING_RISE_SPEED));
      #else
       do_blocking_move_to_z(current_position.z + (big_raise ? 25 : Z_CLEARANCE_BETWEEN_PROBES), z_probe_fast_mm_s);
      #endif
      
    }
    else if (raise_after == PROBE_PT_STOW || raise_after == PROBE_PT_LAST_STOW)
      if (stow()) measured_z = NAN;   // Error on stow?

    if (verbose_level > 2)
      SERIAL_ECHOLNPAIR("Bed X: ", LOGICAL_X_POSITION(rx), " Y: ", LOGICAL_Y_POSITION(ry), " Z: ", measured_z);
  }

  if (isnan(measured_z) || ANKER_OVERPRESSURE_TRIGGER()) {
    if (ANKER_OVERPRESSURE_TRIGGER()) {
      ANKER_CLOSED_OVERPRESSURE_TRIGGER();
      measured_z = NAN;
    }

    stow();
    LCD_MESSAGEPGM(MSG_LCD_PROBING_FAILED);
    #if DISABLED(G29_RETRY_AND_RECOVER)
      #if ADAPT_DETACHED_NOZZLE
      uart_nozzle_tx_notify_error();
      #endif
      SERIAL_ERROR_MSG(STR_ERR_PROBING_FAILED);
    #endif
  }

  return measured_z;
}
#if ENABLED(ANKER_Z_OFFSET_FUNC)
 #include "../feature/anker/anker_z_offset.h"
float Probe::anker_z_ofset_probe_at_point(const_float_t rx, const_float_t ry, const ProbePtRaise raise_after/*=PROBE_PT_NONE*/, const uint8_t verbose_level/*=0*/, const bool probe_relative/*=true*/, const bool sanity_check/*=true*/,const bool cs1237_en) {
  DEBUG_SECTION(log_probe, "Probe::probe_at_point", DEBUGGING(LEVELING));

  if (DEBUGGING(LEVELING)) {
    DEBUG_ECHOLNPAIR(
      "...(", LOGICAL_X_POSITION(rx), ", ", LOGICAL_Y_POSITION(ry),
      ", ", raise_after == PROBE_PT_RAISE ? "raise" : raise_after == PROBE_PT_LAST_STOW ? "stow (last)" : raise_after == PROBE_PT_STOW ? "stow" : "none",
      ", ", verbose_level,
      ", ", probe_relative ? "probe" : "nozzle", "_relative)"
    );
    DEBUG_POS("", current_position);
  }

  // On delta keep Z below clip height or do_blocking_move_to will abort
  xyz_pos_t npos = { rx, ry, _MIN(TERN(DELTA, delta_clip_start_height, current_position.z), current_position.z) };

  position_is_reachable(npos);  // The given position is in terms of the nozzle
  // Move the probe to the starting XYZ
  do_blocking_move_to(npos, feedRate_t(XY_PROBE_FEEDRATE_MM_S));
  
  float measured_z = NAN;

  if(cs1237_en)
  {
    gcode.process_subcommands_now_P(PSTR("M109 S0\nM140 S0\n"));
    anker_z_offset.reset_init();
    anker_z_offset.cs1237_start_convert=true;
    SERIAL_ECHO(" \r\n!!s1237_en=true!!\r\n");
    safe_delay(200);
    //cs1237_init_value();
    measured_z = anker_z_offset_run_z_probe(sanity_check);
  }
  else
  {
    if (!anker_deploy()) measured_z = anker_z_offset_run_z_probe(sanity_check);
    anker_z_offset.cs1237_start_convert=false;
    SERIAL_ECHO(" \r\n!!s1237_en=false!!\r\n");
  }

  if (!isnan(measured_z)) {
    const bool big_raise = raise_after == PROBE_PT_BIG_RAISE;
    if (big_raise || raise_after == PROBE_PT_RAISE)
    {
       do_blocking_move_to_z(current_position.z + (big_raise ? 25 : Z_CLEARANCE_BETWEEN_PROBES), z_probe_fast_mm_s);
    }
    else if (raise_after == PROBE_PT_STOW || raise_after == PROBE_PT_LAST_STOW)
      if (stow()) measured_z = NAN;   // Error on stow?

    if (verbose_level > 2)
      SERIAL_ECHOLNPAIR("Bed X: ", LOGICAL_X_POSITION(rx), " Y: ", LOGICAL_Y_POSITION(ry), " Z: ", measured_z);
  }

  if (isnan(measured_z)) {
    stow();
    LCD_MESSAGEPGM(MSG_LCD_PROBING_FAILED);
    #if DISABLED(G29_RETRY_AND_RECOVER)
      SERIAL_ERROR_MSG(STR_ERR_PROBING_FAILED);
    #endif
  }

  return measured_z;
}

#endif

#if HAS_Z_SERVO_PROBE

  void Probe::servo_probe_init() {
    /**
     * Set position of Z Servo Endstop
     *
     * The servo might be deployed and positioned too low to stow
     * when starting up the machine or rebooting the board.
     * There's no way to know where the nozzle is positioned until
     * homing has been done - no homing with z-probe without init!
     */
    STOW_Z_SERVO();
  }

#endif // HAS_Z_SERVO_PROBE

#if EITHER(SENSORLESS_PROBING, SENSORLESS_HOMING)

  sensorless_t stealth_states { false };

  /**
   * Disable stealthChop if used. Enable diag1 pin on driver.
   */
  void Probe::enable_stallguard_diag1() {
    #if ENABLED(SENSORLESS_PROBING)
      #if ENABLED(DELTA)
        stealth_states.x = tmc_enable_stallguard(stepperX);
        stealth_states.y = tmc_enable_stallguard(stepperY);
      #endif
       #if ENABLED(USE_Z_SENSORLESS)
        anker_tmc2209.tmc_enable_stallguard(stepperZ,anker_tmc2209.thrs_z1); 
       #else
        stealth_states.z = tmc_enable_stallguard(stepperZ); 
       #endif
      endstops.enable(true);
    #endif
  }

  /**
   * Re-enable stealthChop if used. Disable diag1 pin on driver.
   */
  void Probe::disable_stallguard_diag1() {
    #if ENABLED(SENSORLESS_PROBING)
      endstops.not_homing();
      #if ENABLED(DELTA)
        tmc_disable_stallguard(stepperX, stealth_states.x);
        tmc_disable_stallguard(stepperY, stealth_states.y);
      #endif
       #if ENABLED(USE_Z_SENSORLESS)
          anker_tmc2209.tmc_disable_stallguard(stepperZ, stealth_states.z); 
       #else
         tmc_disable_stallguard(stepperZ, stealth_states.z);
       #endif
    #endif
  }

  /**
   * Change the current in the TMC drivers to N##_CURRENT_HOME. And we save the current configuration of each TMC driver.
   */
  void Probe::set_homing_current(const bool onoff) {
    #define HAS_CURRENT_HOME(N) (defined(N##_CURRENT_HOME) && N##_CURRENT_HOME != N##_CURRENT)
    #if HAS_CURRENT_HOME(X) || HAS_CURRENT_HOME(Y) || HAS_CURRENT_HOME(Z)
      #if ENABLED(DELTA)
        static int16_t saved_current_X, saved_current_Y;
      #endif
      #if HAS_CURRENT_HOME(Z)
        static int16_t saved_current_Z;
      #endif
      #if ((ENABLED(DELTA) && (HAS_CURRENT_HOME(X) || HAS_CURRENT_HOME(Y))) || HAS_CURRENT_HOME(Z))
        auto debug_current_on = [](PGM_P const s, const int16_t a, const int16_t b) {
          if (DEBUGGING(LEVELING)) { DEBUG_ECHOPGM_P(s); DEBUG_ECHOLNPAIR(" current: ", a, " -> ", b); }
        };
      #endif
      if (onoff) {
        #if ENABLED(DELTA)
          #if HAS_CURRENT_HOME(X)
            saved_current_X = stepperX.getMilliamps();
            stepperX.rms_current(X_CURRENT_HOME);
            debug_current_on(PSTR("X"), saved_current_X, X_CURRENT_HOME);
          #endif
          #if HAS_CURRENT_HOME(Y)
            saved_current_Y = stepperY.getMilliamps();
            stepperY.rms_current(Y_CURRENT_HOME);
            debug_current_on(PSTR("Y"), saved_current_Y, Y_CURRENT_HOME);
          #endif
        #endif
        #if HAS_CURRENT_HOME(Z)
          saved_current_Z = stepperZ.getMilliamps();
          stepperZ.rms_current(Z_CURRENT_HOME);
          debug_current_on(PSTR("Z"), saved_current_Z, Z_CURRENT_HOME);
        #endif
        TERN_(IMPROVE_HOMING_RELIABILITY, planner.enable_stall_prevention(true));
        TERN_(SENSORLESS_STALLGUARD_DELAY, safe_delay(SENSORLESS_STALLGUARD_DELAY));// Short delay needed to settleD
      }
      else {
        #if ENABLED(DELTA)
          #if HAS_CURRENT_HOME(X)
            stepperX.rms_current(saved_current_X);
            debug_current_on(PSTR("X"), X_CURRENT_HOME, saved_current_X);
          #endif
          #if HAS_CURRENT_HOME(Y)
            stepperY.rms_current(saved_current_Y);
            debug_current_on(PSTR("Y"), Y_CURRENT_HOME, saved_current_Y);
          #endif
        #endif
        #if HAS_CURRENT_HOME(Z)
          stepperZ.rms_current(saved_current_Z);
          debug_current_on(PSTR("Z"), Z_CURRENT_HOME, saved_current_Z);
        #endif
        TERN_(IMPROVE_HOMING_RELIABILITY, planner.enable_stall_prevention(false));
        TERN_(SENSORLESS_STALLGUARD_DELAY, safe_delay(SENSORLESS_STALLGUARD_DELAY));// Short delay needed to settle
      }
    #endif
  }

#endif // SENSORLESS_PROBING

#endif // HAS_BED_PROBE
