#include "V1495_tpc.hh"
#include "MongoLog.hh"

V1495_TPC::V1495_TPC(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& opts, int bid, int handle, unsigned int address) :
  V1495(log, opts, bid, handle, address),
  fModeReg(0x100C),
  fFracLtOnAReg(0x100E), fFracLtOnBReg(0x1010),
  fFracLtOffAReg(0x1012), fFracLtOffBReg(0x1014),
  fAntiVetoDelayAReg(0x1016), fAntiVetoDelayBReg(0x1018),
  fAntiVetoDurationAReg(0x101A), fAntiVetoDurationBReg(0x101C) {
  fFracLTVetoOn_clk = fFracLTVetoOff_clk = 0;
  fAntiVetoDelay_clk = fAntiVetoDuration_clk = 0;
  fMode = 0x0;
}

V1495_TPC::~V1495_TPC() {}

int V1495_TPC::Arm(std::map<std::string, int>& opts) {
  int clocks_per_us = 40;
  unsigned int is_busy_he_used = 1;
  unsigned int is_hev_on = opts["is_hev_on"];
  unsigned int is_hev_start_stop_on = 1;
  unsigned int is_frac_lt_mode_on = opts["is_frac_lt_mode_on"];
  unsigned int is_led_start_stop_active = opts["is_led_start_stop_active"];
  unsigned int is_anti_veto_active = opts["is_anti_veto_active"];
  unsigned int is_anti_veto_start_stop_active = 0;
  unsigned int use_legacy_port_hev = opts["_use_legacy_port_hev"];
  unsigned int use_regular_port_trg = opts["_use_regular_port_trg"];
  unsigned int use_legacy_port_trg = opts["_use_legacy_port_trg"];
  unsigned int use_NG_input = opts["_use_NG_input"];

  // High Energy Busy always used for Busy Veto
  if (is_busy_he_used == 0){  // this is currently forced to be zero, but in case we want to still use this someday.
     fLog->Entry(MongoLog::Message, "V1495: Busy from high energy ADCs are set to be ignored, "
     "however this is probably not a good idea. You have been warned! :)");
    }

  // Anti Veto Time conversion, duration > 0
  if (is_anti_veto_active == 1) {
    fAntiVetoDelay_clk = opts["anti_veto_delay_us"] * clocks_per_us;
    fAntiVetoDuration_clk = opts["anti_veto_duration_us"] * clocks_per_us;
    if (fAntiVetoDuration_clk == 0) {
      fLog->Entry(MongoLog::Message, "V1495: Neutron Generator anti-veto duration is zero. Turning anti-veto off.");
      is_anti_veto_active = 0;
    } else {
      fLog->Entry(MongoLog::Local, "V1495 Neutron Generator anti-veto mode active: delay %ius, duration %ius",
          opts["anti_veto_delay_us"], opts["anti_veto_duration_us"]);
    }
  }

  // Fractional Lifetime Veto On & Off durations conversion, both > 0
  if (is_frac_lt_mode_on == 1) {
    fFracLTVetoOn_clk = opts["fractional_lifetime_veto_on_us"] * clocks_per_us;
    fFracLTVetoOff_clk = opts["fractional_lifetime_veto_off_us"] * clocks_per_us;
    if (fFracLTVetoOn_clk * fFracLTVetoOff_clk == 0) {
      fLog->Entry(MongoLog::Message, "V1495: at least one value is zero, check the config: %i/%i",
          opts["fractional_lifetime_veto_on_us"], opts["fractional_lifetime_veto_off_us"]);
      is_frac_lt_mode_on = 0;
    } else {
      fLog->Entry(MongoLog::Local, "V1495 fractional mode active: on %i, off %i",
          opts["fractional_lifetime_veto_on_us"], opts["fractional_lifetime_veto_off_us"]);
    }
  }

  // Only for testing, that of Anti-Veto start/stop is reconstructed corretcly in straxen from NG start/stop
  // Anti-Veto start/stop on, if HEV is not used (they are on the same output; Anti-Veto can be reconstructed otherwise)
  if ((is_anti_veto_active == 1) && (is_hev_on == 0)) {
      fLog->Entry(MongoLog::Message, "V1495: Anti-Veto on and HEV off: putting Anti-Veto start/stops on HEV start/stop output");
      is_anti_veto_start_stop_active = 1;
      is_hev_start_stop_on = 0;
  }

  // Fractional Lifetime mode: no HEV at the same time. Turn off HEV start/stops (they are on the same output)
  if (is_frac_lt_mode_on == 1){
      if (is_hev_on == 1) {
          fLog->Entry(MongoLog::Message, "V1495: HEV and Fractional Lifetime set to on. Assuming that was a mistake. Switching off FracLT.");
          is_frac_lt_mode_on = 0;
      } else {
          fLog->Entry(MongoLog::Message, "V1495: Fractional Lifetime set to on. Switching off HEV start/stop to avoid confusion.");
          is_hev_start_stop_on = 0;
        }
  }

  fMode = (is_busy_he_used << 0) |
          (is_hev_on << 2) |
          (is_hev_start_stop_on << 3) |
          (is_frac_lt_mode_on << 4) |
          (is_led_start_stop_active << 5) |
          (is_anti_veto_active << 6) |
          (is_anti_veto_start_stop_active << 7) |
          (use_legacy_port_hev << 8) |
          (use_regular_port_trg << 9) |
          (use_legacy_port_trg << 10) |
          (use_NG_input << 11);

  fLog->Entry(MongoLog::Local, "V1495: Mode register will be:  0x%X", fMode);
  fLog->Entry(MongoLog::Message, "V1495: Final Mode: "
                                 "hebusy %i, hev %i, hevss %i, "
                                 "fraclt %i, LEDss_on %i, antiv %i, antivss %i, "
                                 "hev_leg %i, reg_trg %i, leg_trg %i, NG_inp %i",
              is_busy_he_used, is_hev_on, is_hev_start_stop_on,
              is_frac_lt_mode_on, is_led_start_stop_active, is_anti_veto_active, is_anti_veto_start_stop_active,
              use_legacy_port_hev, use_regular_port_trg, use_legacy_port_trg, use_NG_input);
  return 0;
}



int V1495_TPC::ArmSoft(std::map<std::string, int>& opts) {
  int clocks_per_us = 40;
  unsigned int is_busy_he_used = 1;
  unsigned int is_hev_on = opts["is_hev_on"];
  unsigned int is_hev_start_stop_on = 1;
  unsigned int is_frac_lt_mode_on = opts["is_frac_lt_mode_on"];
  unsigned int is_led_start_stop_active = opts["is_led_start_stop_active"];
  unsigned int is_anti_veto_active = opts["is_anti_veto_active"];
  unsigned int is_anti_veto_start_stop_active = 0;
  unsigned int use_legacy_port_hev = opts["_use_legacy_port_hev"];
  unsigned int use_regular_port_trg = opts["_use_regular_port_trg"];
  unsigned int use_legacy_port_trg = opts["_use_legacy_port_trg"];
  unsigned int use_NG_input = opts["_use_NG_input"];

  // Only for testing, that of Anti-Veto start/stop is reconstructed corretcly in straxen from NG start/stop
  // Anti-Veto start/stop on, if HEV is not used (they are on the same output; Anti-Veto can be reconstructed otherwise)
  if ((is_anti_veto_active == 1) && (is_hev_on == 0)) {
      fLog->Entry(MongoLog::Message, "V1495: Anti-Veto on and HEV off: putting Anti-Veto start/stops on HEV start/stop output");
      is_anti_veto_start_stop_active = 1;
      is_hev_start_stop_on = 0;
  }

  // Fractional Lifetime mode: no HEV at the same time. Turn off HEV start/stops (they are on the same output)
  if (is_frac_lt_mode_on == 1){
      if (is_hev_on == 1) {
          fLog->Entry(MongoLog::Message, "V1495: HEV and Fractional Lifetime set to on. Please activate only 1 of them at a time.");
          return 1;
      } else {
          fLog->Entry(MongoLog::Message, "V1495: Fractional Lifetime set to on. Switching off HEV start/stop to avoid confusion.");
          is_hev_start_stop_on = 0;
        }
  }

  // Check numerical options, if we run in modes that require those
  // Anti Veto Time conversion, duration > 0
  if (is_anti_veto_active == 1) {
    fAntiVetoDelay_clk = opts["anti_veto_delay_us"] * clocks_per_us;
    fAntiVetoDuration_clk = opts["anti_veto_duration_us"] * clocks_per_us;
    if (fAntiVetoDuration_clk == 0) {
      fLog->Entry(MongoLog::Message, "V1495: Neutron Generator anti-veto duration is zero. Turning anti-veto off.");
      is_anti_veto_active = 0;
      return 1;
    } else {
      fLog->Entry(MongoLog::Local, "V1495 Neutron Generator anti-veto mode active: delay %ius, duration %ius",
          opts["anti_veto_delay_us"], opts["anti_veto_duration_us"]);
    }
  }

  // Fractional Lifetime Veto On & Off durations conversion, both > 0
  if (is_frac_lt_mode_on == 1) {
    fFracLTVetoOn_clk = opts["fractional_lifetime_veto_on_us"] * clocks_per_us;
    fFracLTVetoOff_clk = opts["fractional_lifetime_veto_off_us"] * clocks_per_us;
    if (fFracLTVetoOn_clk * fFracLTVetoOff_clk == 0) {
      fLog->Entry(MongoLog::Message, "V1495: at least one value is zero, check the config: %i/%i",
          opts["fractional_lifetime_veto_on_us"], opts["fractional_lifetime_veto_off_us"]);
      return 1;
    } else {
      fLog->Entry(MongoLog::Local, "V1495 fractional mode active: on %i, off %i",
          opts["fractional_lifetime_veto_on_us"], opts["fractional_lifetime_veto_off_us"]);
    }
  }

  fMode = (is_busy_he_used << 0) |
          (is_hev_on << 2) |
          (is_hev_start_stop_on << 3) |
          (is_frac_lt_mode_on << 4) |
          (is_led_start_stop_active << 5) |
          (is_anti_veto_active << 6) |
          (is_anti_veto_start_stop_active << 7) |
          (use_legacy_port_hev << 8) |
          (use_regular_port_trg << 9) |
          (use_legacy_port_trg << 10) |
          (use_NG_input << 11);

  fLog->Entry(MongoLog::Local, "V1495: Mode register will be:  0x%X", fMode);
  fLog->Entry(MongoLog::Message, "V1495: Final Mode: "
                                 "hebusy %i, hev %i, hevss %i, "
                                 "fraclt %i, LEDss_on %i, antiv %i, antivss %i, "
                                 "hev_leg %i, reg_trg %i, leg_trg %i, NG_inp %i",
              is_busy_he_used, is_hev_on, is_hev_start_stop_on,
              is_frac_lt_mode_on, is_led_start_stop_active, is_anti_veto_active, is_anti_veto_start_stop_active,
              use_legacy_port_hev, use_regular_port_trg, use_legacy_port_trg, use_NG_input);
  return 0;
}


int V1495_TPC::BeforeSINStart() {
  int ret = 0;
    ret += WriteReg(fModeReg, fMode);
    ret += WriteReg(fFracLtOnBReg, (fFracLTVetoOn_clk & 0xFFFF0000) >> 16);
    ret += WriteReg(fFracLtOnAReg, fFracLTVetoOn_clk & 0xFFFF);
    ret += WriteReg(fFracLtOffBReg, (fFracLTVetoOff_clk & 0xFFFF0000) >> 16);
    ret += WriteReg(fFracLtOffAReg, fFracLTVetoOff_clk & 0xFFFF);
    ret += WriteReg(fAntiVetoDelayBReg, (fAntiVetoDelay_clk & 0xFFFF0000) >> 16);
    ret += WriteReg(fAntiVetoDelayAReg, fAntiVetoDelay_clk & 0xFFFF);
    ret += WriteReg(fAntiVetoDurationBReg, (fAntiVetoDuration_clk & 0xFFFF0000) >> 16);
    ret += WriteReg(fAntiVetoDurationAReg, fAntiVetoDuration_clk & 0xFFFF);
  return ret;
}

int V1495_TPC::BeforeSINStop() {
    int ret = 0;
    ret += WriteReg(fModeReg, 0xD);
    ret += WriteReg(fFracLtOnBReg, 0x0000);
    ret += WriteReg(fFracLtOnAReg, 0x0000);
    ret += WriteReg(fFracLtOffBReg, 0x0000);
    ret += WriteReg(fFracLtOffAReg, 0x0000);
    ret += WriteReg(fAntiVetoDelayBReg, 0x0000);
    ret += WriteReg(fAntiVetoDelayAReg, 0x0000);
    ret += WriteReg(fAntiVetoDurationBReg, 0x0000);
    ret += WriteReg(fAntiVetoDurationAReg, 0x0000);
    return ret;
}

