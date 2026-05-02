#include <CAENVMElib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t REG_AQ_CTRL = 0x8100;
constexpr uint32_t REG_AQ_STATUS = 0x8104;
constexpr uint32_t REG_SW_TRIG = 0x8108;
constexpr uint32_t REG_TRIG_SRC_MASK = 0x810C;
constexpr uint32_t REG_POST_TRIG = 0x8114;
constexpr uint32_t REG_CH_ENABLE_MASK = 0x8120;
constexpr uint32_t REG_CH_DAC = 0x1098;
constexpr uint32_t REG_CH_TRIG = 0x1060;
constexpr uint32_t REG_SN_MSB = 0xF080;
constexpr uint32_t REG_SN_LSB = 0xF084;
constexpr uint32_t REG_BOARD_FAIL = 0x8178;
constexpr uint32_t REG_READOUT_STATUS = 0xEF04;
constexpr uint32_t REG_BOARD_ERR = 0xEF00;
constexpr uint32_t REG_RESET = 0xEF24;

constexpr uint32_t STATUS_EVENT_READY = 0x8;
constexpr uint32_t STATUS_EVENT_FULL = 0x10;
constexpr uint32_t STATUS_RUN = 0x4;

struct Options {
  int link = -1;
  int crate = -1;
  int board_id = -1;
  uint32_t base_address = 0;

  uint32_t channel_mask = 0xFF;
  bool apply_channel_mask = false;
  uint16_t fixed_dac = 7000;
  std::vector<uint16_t> thresholds = std::vector<uint16_t>(8, 0xA);
  std::optional<uint32_t> trigger_mask;
  std::optional<uint32_t> post_trigger;
  std::vector<std::pair<uint32_t, uint32_t>> reg_writes;

  std::string start_mode = "software";
  bool mode10_explicit_start_stop = false;
  bool stop_only = false;
  bool do_reset = true;
  bool do_sn_check = false;
  bool continue_on_read_error = false;
  bool save_packets = false;
  std::optional<std::string> dump_invalid_prefix;
  std::string save_packets_prefix = "packet";

  int read_sleep_us = 10;
  int status_period = 1000;
  int64_t buffer_bytes = 0x800000;
  double duration_s = 30.0;
  int64_t max_packets = 0;
};

struct Stats {
  int64_t loops = 0;
  int64_t packets = 0;
  int64_t packets_with_data = 0;
  int64_t bytes = 0;
  int64_t bus_error_terminations = 0;
  int64_t read_errors = 0;
  int64_t invalid_markers = 0;
  int64_t parsed_events = 0;
  int64_t event_full_seen = 0;
  int64_t saved_packets = 0;
  int64_t header_events = 0;
  int64_t header_invalid_packets = 0;
  int64_t header_word1_board_id_inconsistent = 0;
  int64_t header_word1_reserved_inconsistent = 0;
  int64_t header_word1_pattern_inconsistent = 0;
  int64_t header_word1_channel_mask_inconsistent = 0;
  int64_t header_fail_flag_events = 0;
  int64_t header_counter_jump_events = 0;
  int64_t header_inferred_missing_events = 0;
  int64_t header_counter_backwards = 0;
  int64_t header_counter_repeats = 0;
  int64_t header_time_backwards = 0;
  int64_t header_time_repeats = 0;

  std::array<int64_t, 8> channel_wave_bytes_total{};
  int64_t rate_windows_1s = 0;
  int64_t channel_wave_bytes_windows_total = 0;
  int64_t max_channel_wave_bytes_1s = 0;
};

struct PacketCheck {
  int events = 0;
  bool invalid = false;
  uint32_t bad_word = 0;
  uint32_t advertised_words = 0;
  size_t bad_index = 0;
  size_t remaining_words = 0;
};

struct PacketWork {
  int64_t packet_id = 0;
  int64_t nbytes = 0;
  std::vector<uint8_t> data;
};

struct HeaderState {
  bool have_word1_reference = false;
  uint32_t word1_board_id_ref = 0;
  uint32_t word1_reserved_ref = 0;
  uint32_t word1_pattern_ref = 0;
  uint32_t word1_channel_mask_ref = 0;
  bool have_counter = false;
  uint32_t previous_counter = 0;
  bool have_time_tag = false;
  uint32_t previous_time_tag = 0;
};

struct HeaderCheck {
  int events = 0;
  bool invalid = false;
  uint32_t bad_word = 0;
  uint32_t advertised_words = 0;
  size_t bad_index = 0;
  size_t remaining_words = 0;
  int64_t word1_board_id_inconsistent = 0;
  int64_t word1_reserved_inconsistent = 0;
  int64_t word1_pattern_inconsistent = 0;
  int64_t word1_channel_mask_inconsistent = 0;
  int64_t fail_flag_events = 0;
  int64_t counter_jump_events = 0;
  int64_t inferred_missing_events = 0;
  int64_t counter_backwards = 0;
  int64_t counter_repeats = 0;
  int64_t time_backwards = 0;
  int64_t time_repeats = 0;
  bool have_word1_sample = false;
  uint32_t word1_board_id_ref_sample = 0;
  uint32_t word1_board_id_seen_sample = 0;
  uint32_t word1_reserved_ref_sample = 0;
  uint32_t word1_reserved_seen_sample = 0;
  uint32_t word1_pattern_ref_sample = 0;
  uint32_t word1_pattern_seen_sample = 0;
  uint32_t word1_channel_mask_ref_sample = 0;
  uint32_t word1_channel_mask_seen_sample = 0;
  bool have_counter_jump_sample = false;
  uint32_t counter_prev_sample = 0;
  uint32_t counter_now_sample = 0;
  bool have_counter_backwards_sample = false;
  uint32_t counter_prev_backwards_sample = 0;
  uint32_t counter_now_backwards_sample = 0;
  bool have_time_backwards_sample = false;
  uint32_t time_prev_sample = 0;
  uint32_t time_now_sample = 0;
};

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "\n"
      << "Required:\n"
      << "  --link <int>                Optical link number\n"
      << "  --crate <int>               Crate number\n"
      << "  --base <hex|dec>            VME base address (e.g. 0xFFFF0000)\n"
      << "\n"
      << "Board setup:\n"
      << "  --board-id <int>            Board ID for logging only\n"
      << "  --channel-mask <hex|dec>    Channel enable mask (written only if specified)\n"
      << "  --fixed-dac <hex|dec>       Fixed DAC for enabled channels (default 7000)\n"
      << "  --threshold <hex|dec>       Same threshold for all 8 channels\n"
      << "  --thresholds a,b,c,d,e,f,g,h  Per-channel thresholds\n"
      << "  --trigger-mask <hex|dec>    Write Trigger Source Enable Mask (0x810C)\n"
      << "  --post-trigger <hex|dec>    Write Post Trigger Setting (0x8114)\n"
      << "  --reg <REG=VAL>             Extra register write, repeatable\n"
      << "  --no-reset                  Skip reset sequence\n"
      << "  --do-sn-check               Read SN registers and print SN\n"
      << "\n"
      << "Run control:\n"
      << "  --start-mode software|sin   Acquisition start mode (default software)\n"
      << "  --mode10-explicit-start-stop  Force AQ_CTRL mode[1:0]=10 and explicit SW start/stop\n"
      << "  --stop-only                 Issue SW stop (0x8100=0x100) and exit\n"
      << "  --duration-s <float>        Run duration in seconds (default 30)\n"
      << "  --max-packets <int>         Stop after N packets with data (0 = unlimited)\n"
      << "  --sleep-us <int>            Poll sleep in us (default 10)\n"
      << "  --buffer-bytes <int>        BLT read buffer bytes (default 0x800000)\n"
      << "  --status-period <int>       Print status every N loops (default 1000)\n"
      << "  --continue-on-read-error    Continue after read errors\n"
      << "  --save-packets              Save all non-empty data packets\n"
      << "  --save-packets-prefix <p>   Packet dump prefix (default 'packet')\n"
      << "  --dump-invalid-prefix <p>   Dump invalid packets to binary files\n"
      << "\n";
}

bool ParseU32(const std::string& s, uint32_t* out) {
  auto in_u32_range = [](unsigned long long v) { return v <= 0xFFFFFFFFULL; };
  try {
    size_t idx = 0;
    unsigned long long v = std::stoull(s, &idx, 0);
    if (idx != s.size() || !in_u32_range(v)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    bool all_hex = !s.empty();
    for (char c : s) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        all_hex = false;
        break;
      }
    }
    if (!all_hex) return false;
    try {
      size_t idx = 0;
      unsigned long long v = std::stoull(s, &idx, 16);
      if (idx != s.size() || !in_u32_range(v)) return false;
      *out = static_cast<uint32_t>(v);
      return true;
    } catch (...) {
      return false;
    }
  }
}

bool ParseI32(const std::string& s, int* out) {
  try {
    size_t idx = 0;
    long long v = std::stoll(s, &idx, 0);
    if (idx != s.size() || v < INT32_MIN || v > INT32_MAX) return false;
    *out = static_cast<int>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseI64(const std::string& s, int64_t* out) {
  try {
    size_t idx = 0;
    long long v = std::stoll(s, &idx, 0);
    if (idx != s.size()) return false;
    *out = static_cast<int64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseDouble(const std::string& s, double* out) {
  try {
    size_t idx = 0;
    double v = std::stod(s, &idx);
    if (idx != s.size() || !std::isfinite(v)) return false;
    *out = v;
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseThresholdList(const std::string& s, std::vector<uint16_t>* out) {
  std::vector<uint16_t> vals;
  std::stringstream ss(s);
  std::string token;
  while (std::getline(ss, token, ',')) {
    uint32_t v = 0;
    if (!ParseU32(token, &v) || v > 0xFFFF) return false;
    vals.push_back(static_cast<uint16_t>(v));
  }
  if (vals.size() != 8) return false;
  *out = vals;
  return true;
}

bool ParseRegWrite(const std::string& s, std::pair<uint32_t, uint32_t>* out) {
  const size_t eq = s.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= s.size()) return false;
  uint32_t reg = 0;
  uint32_t val = 0;
  if (!ParseU32(s.substr(0, eq), &reg) || !ParseU32(s.substr(eq + 1), &val)) {
    return false;
  }
  *out = {reg, val};
  return true;
}

bool ParseArgs(int argc, char** argv, Options* opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        std::exit(2);
      }
      return argv[++i];
    };

    if (a == "--help" || a == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (a == "--link") {
      int v = 0;
      if (!ParseI32(need("--link"), &v)) return false;
      opt->link = v;
    } else if (a == "--crate") {
      int v = 0;
      if (!ParseI32(need("--crate"), &v)) return false;
      opt->crate = v;
    } else if (a == "--board-id") {
      int v = 0;
      if (!ParseI32(need("--board-id"), &v)) return false;
      opt->board_id = v;
    } else if (a == "--base") {
      uint32_t v = 0;
      if (!ParseU32(need("--base"), &v)) return false;
      opt->base_address = v;
    } else if (a == "--channel-mask") {
      uint32_t v = 0;
      if (!ParseU32(need("--channel-mask"), &v)) return false;
      opt->channel_mask = v;
      opt->apply_channel_mask = true;
    } else if (a == "--fixed-dac") {
      uint32_t v = 0;
      if (!ParseU32(need("--fixed-dac"), &v) || v > 0xFFFF) return false;
      opt->fixed_dac = static_cast<uint16_t>(v);
    } else if (a == "--threshold") {
      uint32_t v = 0;
      if (!ParseU32(need("--threshold"), &v) || v > 0xFFFF) return false;
      opt->thresholds.assign(8, static_cast<uint16_t>(v));
    } else if (a == "--thresholds") {
      std::vector<uint16_t> vals;
      if (!ParseThresholdList(need("--thresholds"), &vals)) return false;
      opt->thresholds = std::move(vals);
    } else if (a == "--trigger-mask") {
      uint32_t v = 0;
      if (!ParseU32(need("--trigger-mask"), &v)) return false;
      opt->trigger_mask = v;
    } else if (a == "--post-trigger") {
      uint32_t v = 0;
      if (!ParseU32(need("--post-trigger"), &v)) return false;
      opt->post_trigger = v;
    } else if (a == "--reg") {
      std::pair<uint32_t, uint32_t> rv;
      if (!ParseRegWrite(need("--reg"), &rv)) return false;
      opt->reg_writes.push_back(rv);
    } else if (a == "--start-mode") {
      std::string v = need("--start-mode");
      if (v != "software" && v != "sin") return false;
      opt->start_mode = v;
    } else if (a == "--mode10-explicit-start-stop") {
      opt->mode10_explicit_start_stop = true;
    } else if (a == "--stop-only") {
      opt->stop_only = true;
    } else if (a == "--duration-s") {
      double v = 0.0;
      if (!ParseDouble(need("--duration-s"), &v) || v <= 0.0) return false;
      opt->duration_s = v;
    } else if (a == "--max-packets") {
      int64_t v = 0;
      if (!ParseI64(need("--max-packets"), &v) || v < 0) return false;
      opt->max_packets = v;
    } else if (a == "--sleep-us") {
      int v = 0;
      if (!ParseI32(need("--sleep-us"), &v) || v < 0) return false;
      opt->read_sleep_us = v;
    } else if (a == "--status-period") {
      int v = 0;
      if (!ParseI32(need("--status-period"), &v) || v <= 0) return false;
      opt->status_period = v;
    } else if (a == "--buffer-bytes") {
      int64_t v = 0;
      if (!ParseI64(need("--buffer-bytes"), &v) || v <= 0) return false;
      opt->buffer_bytes = v;
    } else if (a == "--dump-invalid-prefix") {
      opt->dump_invalid_prefix = need("--dump-invalid-prefix");
    } else if (a == "--continue-on-read-error") {
      opt->continue_on_read_error = true;
    } else if (a == "--save-packets") {
      opt->save_packets = true;
    } else if (a == "--save-packets-prefix") {
      opt->save_packets_prefix = need("--save-packets-prefix");
      opt->save_packets = true;
    } else if (a == "--no-reset") {
      opt->do_reset = false;
    } else if (a == "--do-sn-check") {
      opt->do_sn_check = true;
    } else {
      std::cerr << "Unknown argument: " << a << "\n";
      return false;
    }
  }
  return true;
}

const char* CvName(int code) {
  switch (code) {
    case cvSuccess:
      return "cvSuccess";
    case cvBusError:
      return "cvBusError";
    default:
      return "cv<other>";
  }
}

bool WriteReg(int handle, uint32_t base, uint32_t reg, uint32_t value) {
  int ret = CAENVME_WriteCycle(handle, base + reg, &value, cvA32_U_DATA, cvD32);
  if (ret != cvSuccess) {
    std::cerr << "write reg 0x" << std::hex << reg << " failed ret=" << std::dec << ret
              << "\n";
    return false;
  }
  return true;
}

bool ReadReg(int handle, uint32_t base, uint32_t reg, uint32_t* out) {
  uint32_t temp = 0;
  int ret = CAENVME_ReadCycle(handle, base + reg, &temp, cvA32_U_DATA, cvD32);
  if (ret != cvSuccess) {
    std::cerr << "read reg 0x" << std::hex << reg << " failed ret=" << std::dec << ret
              << "\n";
    return false;
  }
  *out = temp;
  return true;
}

bool WaitStatusBit(int handle, uint32_t base, uint32_t mask, bool want_set,
                   int ntries, int sleep_us) {
  for (int i = 0; i < ntries; ++i) {
    uint32_t s = 0;
    if (!ReadReg(handle, base, REG_AQ_STATUS, &s)) return false;
    bool is_set = (s & mask) != 0;
    if (is_set == want_set) return true;
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
  return false;
}

PacketCheck CheckPacket(const uint32_t* words, size_t n_words) {
  PacketCheck out;
  size_t i = 0;
  while (i < n_words) {
    uint32_t w = words[i];
    if ((w >> 28U) == 0xAU) {
      uint32_t ev_words = w & 0x0FFFFFFFU;
      size_t remaining = n_words - i;
      if (ev_words == 0 || ev_words > remaining) {
        out.invalid = true;
        out.bad_word = w;
        out.advertised_words = ev_words;
        out.bad_index = i;
        out.remaining_words = remaining;
        break;
      }
      out.events++;
      i += ev_words;
    } else {
      ++i;
    }
  }
  return out;
}

HeaderCheck AnalyzeHeaders(const uint32_t* words, size_t n_words, HeaderState* state) {
  HeaderCheck out;
  size_t i = 0;
  while (i < n_words) {
    const uint32_t w0 = words[i];
    if ((w0 >> 28U) != 0xAU) {
      ++i;
      continue;
    }

    const uint32_t ev_words = w0 & 0x0FFFFFFFU;
    const size_t remaining = n_words - i;
    if (ev_words == 0U || ev_words > remaining || ev_words < 4U) {
      out.invalid = true;
      out.bad_word = w0;
      out.advertised_words = ev_words;
      out.bad_index = i;
      out.remaining_words = remaining;
      break;
    }

    const uint32_t w1 = words[i + 1];
    const uint32_t w2 = words[i + 2];
    const uint32_t w3 = words[i + 3];
    const uint32_t w1_board_id = (w1 >> 27U) & 0x1FU;
    const uint32_t w1_reserved = (w1 >> 24U) & 0x3U;
    const uint32_t w1_pattern = (w1 >> 8U) & 0xFFFFU;
    const uint32_t w1_channel_mask = w1 & 0xFFU;

    out.events++;

    if (state->have_word1_reference) {
      if (w1_board_id != state->word1_board_id_ref) {
        out.word1_board_id_inconsistent++;
      }
      if (w1_reserved != state->word1_reserved_ref) {
        out.word1_reserved_inconsistent++;
      }
      if (w1_pattern != state->word1_pattern_ref) {
        out.word1_pattern_inconsistent++;
      }
      if (w1_channel_mask != state->word1_channel_mask_ref) {
        out.word1_channel_mask_inconsistent++;
      }
      if (out.word1_board_id_inconsistent > 0 || out.word1_reserved_inconsistent > 0 ||
          out.word1_pattern_inconsistent > 0 || out.word1_channel_mask_inconsistent > 0) {
        if (!out.have_word1_sample) {
          out.have_word1_sample = true;
          out.word1_board_id_ref_sample = state->word1_board_id_ref;
          out.word1_board_id_seen_sample = w1_board_id;
          out.word1_reserved_ref_sample = state->word1_reserved_ref;
          out.word1_reserved_seen_sample = w1_reserved;
          out.word1_pattern_ref_sample = state->word1_pattern_ref;
          out.word1_pattern_seen_sample = w1_pattern;
          out.word1_channel_mask_ref_sample = state->word1_channel_mask_ref;
          out.word1_channel_mask_seen_sample = w1_channel_mask;
        }
      }
    } else {
      state->have_word1_reference = true;
      state->word1_board_id_ref = w1_board_id;
      state->word1_reserved_ref = w1_reserved;
      state->word1_pattern_ref = w1_pattern;
      state->word1_channel_mask_ref = w1_channel_mask;
    }

    if ((w1 >> 26U) & 0x1U) {
      out.fail_flag_events++;
    }

    if (state->have_counter) {
      const uint32_t delta = w2 - state->previous_counter;
      if (delta == 0U) {
        out.counter_repeats++;
      } else if (delta == 1U) {
      } else if (delta < 0x80000000U) {
        out.counter_jump_events++;
        out.inferred_missing_events += static_cast<int64_t>(delta - 1U);
        if (!out.have_counter_jump_sample) {
          out.have_counter_jump_sample = true;
          out.counter_prev_sample = state->previous_counter;
          out.counter_now_sample = w2;
        }
      } else {
        out.counter_backwards++;
        if (!out.have_counter_backwards_sample) {
          out.have_counter_backwards_sample = true;
          out.counter_prev_backwards_sample = state->previous_counter;
          out.counter_now_backwards_sample = w2;
        }
      }
    }
    state->have_counter = true;
    state->previous_counter = w2;

    if (state->have_time_tag) {
      const uint32_t delta = w3 - state->previous_time_tag;
      if (delta == 0U) {
        out.time_repeats++;
      } else if (delta > 0x80000000U) {
        out.time_backwards++;
        if (!out.have_time_backwards_sample) {
          out.have_time_backwards_sample = true;
          out.time_prev_sample = state->previous_time_tag;
          out.time_now_sample = w3;
        }
      }
    }
    state->have_time_tag = true;
    state->previous_time_tag = w3;

    i += ev_words;
  }
  return out;
}

bool ParseChannelWaveBytesV1724(const uint32_t* words, size_t n_words,
                                std::array<int64_t, 8>* out_bytes) {
  size_t i = 0;
  while (i < n_words) {
    const uint32_t w = words[i];
    if ((w >> 28U) != 0xAU) {
      ++i;
      continue;
    }

    const uint32_t ev_words = w & 0x0FFFFFFFU;
    const size_t remaining = n_words - i;
    if (ev_words < 4U || ev_words > remaining) return false;

    const size_t ev_end = i + static_cast<size_t>(ev_words);
    const uint32_t channel_mask = words[i + 1] & 0xFFU;
    size_t ev_pos = i + 4;

    for (int ch = 0; ch < 8; ++ch) {
      if ((channel_mask & (1U << ch)) == 0U) continue;
      if (ev_pos >= ev_end) return false;

      const uint32_t ch_words = words[ev_pos] & 0x7FFFFFU;
      if (ch_words < 2U) return false;
      const size_t ch_words_sz = static_cast<size_t>(ch_words);
      if (ev_pos + ch_words_sz > ev_end) return false;

      (*out_bytes)[ch] += static_cast<int64_t>(ch_words - 2U) *
                          static_cast<int64_t>(sizeof(uint32_t));
      ev_pos += ch_words_sz;
    }
    i = ev_end;
  }
  return true;
}

bool DumpPacket(const std::string& prefix, int64_t packet_idx,
                const uint8_t* data, int64_t nbytes) {
  std::ostringstream oss;
  oss << prefix << "_packet_" << packet_idx << ".bin";
  std::ofstream out(oss.str(), std::ios::binary);
  if (!out.is_open()) {
    std::cerr << "failed to open dump file " << oss.str() << "\n";
    return false;
  }
  out.write(reinterpret_cast<const char*>(data), nbytes);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!ParseArgs(argc, argv, &opt)) {
    PrintUsage(argv[0]);
    return 2;
  }

  if (opt.link < 0 || opt.crate < 0 || opt.base_address == 0) {
    std::cerr << "Missing required board connection parameters. Provide --link/--crate/--base\n";
    PrintUsage(argv[0]);
    return 2;
  }
  if (opt.buffer_bytes <= 0 || opt.buffer_bytes > INT_MAX) {
    std::cerr << "--buffer-bytes must be in range [1, " << INT_MAX << "]\n";
    return 2;
  }

  int handle = -1;
  uint32_t link_arg = static_cast<uint32_t>(opt.link);
  int ret = CAENVME_Init2(cvV2718, &link_arg, opt.crate, &handle);
  if (ret != cvSuccess) {
    std::cerr << "CAENVME_Init2 failed ret=" << ret << "\n";
    return 1;
  }

  auto cleanup = [&]() {
    if (handle >= 0) {
      CAENVME_End(handle);
      handle = -1;
    }
  };

  std::cout << "Connected. link=" << opt.link << " crate=" << opt.crate
            << " base=0x" << std::hex << opt.base_address << std::dec;
  if (opt.board_id >= 0) std::cout << " board-id=" << opt.board_id;
  std::cout << "\n";

  if (opt.do_reset) {
    if (!WriteReg(handle, opt.base_address, REG_RESET, 0x1) ||
        !WriteReg(handle, opt.base_address, REG_BOARD_ERR, 0x30)) {
      cleanup();
      return 1;
    }
  }

  if (opt.do_sn_check) {
    uint32_t lsb = 0;
    uint32_t msb = 0;
    if (ReadReg(handle, opt.base_address, REG_SN_LSB, &lsb) &&
        ReadReg(handle, opt.base_address, REG_SN_MSB, &msb)) {
      int sn = static_cast<int>(lsb & 0xFFU) |
               (static_cast<int>(msb & 0xFFU) << 8U);
      std::cout << "SN read: " << sn << "\n";
    }
  }

  if (opt.stop_only) {
    uint32_t st = 0;
    if (ReadReg(handle, opt.base_address, REG_AQ_STATUS, &st)) {
      std::cout << "Pre-stop status=0x" << std::hex << st << std::dec << "\n";
    }
    if (!WriteReg(handle, opt.base_address, REG_AQ_CTRL, 0x100)) {
      cleanup();
      return 1;
    }
    if (!WaitStatusBit(handle, opt.base_address, STATUS_RUN, false, 1000, 1000)) {
      std::cerr << "warning: run bit did not clear after stop-only command\n";
    }
    if (ReadReg(handle, opt.base_address, REG_AQ_STATUS, &st)) {
      std::cout << "Post-stop status=0x" << std::hex << st << std::dec << "\n";
    }
    std::cout << "Stop-only command completed\n";
    cleanup();
    return 0;
  }

  std::vector<std::pair<uint32_t, uint32_t>> startup_regs = opt.reg_writes;
  if (opt.trigger_mask.has_value()) {
    startup_regs.emplace_back(REG_TRIG_SRC_MASK, *opt.trigger_mask);
  }
  if (opt.post_trigger.has_value()) {
    startup_regs.emplace_back(REG_POST_TRIG, *opt.post_trigger);
  }
  if (opt.apply_channel_mask) {
    startup_regs.emplace_back(REG_CH_ENABLE_MASK, opt.channel_mask);
  }

  for (const auto& rv : startup_regs) {
    if (!WriteReg(handle, opt.base_address, rv.first, rv.second)) {
      cleanup();
      return 1;
    }
  }

  for (int ch = 0; ch < 8; ++ch) {
    uint32_t reg_dac = REG_CH_DAC + static_cast<uint32_t>(0x100 * ch);
    uint32_t reg_thr = REG_CH_TRIG + static_cast<uint32_t>(0x100 * ch);
    if (!WriteReg(handle, opt.base_address, reg_dac, opt.fixed_dac) ||
        !WriteReg(handle, opt.base_address, reg_thr, opt.thresholds[ch])) {
      cleanup();
      return 1;
    }
  }

  std::cout << "Configured. fixed_dac=" << opt.fixed_dac
            << " channel_mask_applied=" << (opt.apply_channel_mask ? 1 : 0);
  if (opt.apply_channel_mask) {
    std::cout << " channel_mask=0x" << std::hex << opt.channel_mask << std::dec;
  }
  std::cout << "\n";

  uint32_t st = 0;
  if (ReadReg(handle, opt.base_address, REG_AQ_STATUS, &st)) {
    std::cout << "Initial status=0x" << std::hex << st << std::dec << "\n";
  }

  uint32_t start_word = (opt.start_mode == "sin") ? 0x105 : 0x104;
  uint32_t stop_word = 0x100;
  bool issue_sw_start = false;
  if (opt.mode10_explicit_start_stop) {
    start_word = 0x106;
    stop_word = 0x102;
    issue_sw_start = true;
    std::cout << "Mode10 explicit start/stop enabled. AQ_CTRL start=0x106 stop=0x102\n";
  }

  if (opt.mode10_explicit_start_stop) {
    uint32_t trig_mask = 0;
    if (ReadReg(handle, opt.base_address, REG_TRIG_SRC_MASK, &trig_mask)) {
      const uint32_t new_mask = trig_mask | 0x80000000U;
      if (new_mask != trig_mask) {
        if (!WriteReg(handle, opt.base_address, REG_TRIG_SRC_MASK, new_mask)) {
          cleanup();
          return 1;
        }
        std::cout << "Mode10: enabled SW trigger source bit. trig_mask 0x"
                  << std::hex << trig_mask << " -> 0x" << new_mask << std::dec
                  << "\n";
      } else {
        std::cout << "Mode10: SW trigger source bit already enabled in trig_mask=0x"
                  << std::hex << trig_mask << std::dec << "\n";
      }
    } else {
      std::cerr << "warning: could not read trigger mask before mode10 start\n";
    }
  }

  if (!WriteReg(handle, opt.base_address, REG_AQ_CTRL, start_word)) {
    cleanup();
    return 1;
  }
  if (issue_sw_start) {
    if (!WriteReg(handle, opt.base_address, REG_SW_TRIG, 0x1)) {
      cleanup();
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (!WriteReg(handle, opt.base_address, REG_SW_TRIG, 0x1)) {
      cleanup();
      return 1;
    }
    std::cout << "Issued two explicit SW trigger pulses (0x8108=0x1 x2)\n";
  }
  if (opt.mode10_explicit_start_stop) {
    uint32_t aq_ctrl_rb = 0;
    uint32_t trig_mask_rb = 0;
    (void)ReadReg(handle, opt.base_address, REG_AQ_CTRL, &aq_ctrl_rb);
    (void)ReadReg(handle, opt.base_address, REG_TRIG_SRC_MASK, &trig_mask_rb);
    std::cout << "Mode10 readback: AQ_CTRL=0x" << std::hex << aq_ctrl_rb
              << " TRIG_SRC_MASK=0x" << trig_mask_rb << std::dec << "\n"
              << "Note: in mode10, status bit[2] reports ARMED state until first accepted trigger edge.\n";
  }
  if (!WaitStatusBit(handle, opt.base_address, STATUS_RUN, true, 1000, 1000)) {
    std::cerr << "warning: run bit did not assert\n";
  }

  Stats read_stats;
  Stats parse_stats;
  std::array<int64_t, 8> channel_wave_bytes_window{};
  constexpr auto kRateWindow = std::chrono::seconds(1);
  std::vector<uint8_t> read_buffer(static_cast<size_t>(opt.buffer_bytes), 0);
  std::deque<PacketWork> packet_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_not_empty;
  std::condition_variable queue_not_full;
  constexpr size_t kMaxQueuedPackets = 64;
  bool reader_done = false;
  HeaderState header_state;
  auto t0 = std::chrono::steady_clock::now();
  auto t_end = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(opt.duration_s));
  auto rate_window_start = t0;

  auto flush_rate_windows = [&](std::chrono::steady_clock::time_point now) {
    while (rate_window_start + kRateWindow <= now) {
      for (int ch = 0; ch < 8; ++ch) {
        parse_stats.channel_wave_bytes_windows_total += channel_wave_bytes_window[ch];
        parse_stats.max_channel_wave_bytes_1s =
            std::max(parse_stats.max_channel_wave_bytes_1s, channel_wave_bytes_window[ch]);
        channel_wave_bytes_window[ch] = 0;
      }
      parse_stats.rate_windows_1s++;
      rate_window_start += kRateWindow;
    }
  };

  std::thread parser_thread([&]() {
    while (true) {
      PacketWork work;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_not_empty.wait(lock, [&]() { return !packet_queue.empty() || reader_done; });
        if (packet_queue.empty() && reader_done) {
          break;
        }
        work = std::move(packet_queue.front());
        packet_queue.pop_front();
      }
      queue_not_full.notify_one();

      flush_rate_windows(std::chrono::steady_clock::now());

      const size_t n_words = static_cast<size_t>(work.nbytes / 4);
      const auto* words = reinterpret_cast<const uint32_t*>(work.data.data());
      HeaderCheck hcheck = AnalyzeHeaders(words, n_words, &header_state);
      parse_stats.parsed_events += hcheck.events;
      parse_stats.header_events += hcheck.events;
      parse_stats.header_word1_board_id_inconsistent += hcheck.word1_board_id_inconsistent;
      parse_stats.header_word1_reserved_inconsistent += hcheck.word1_reserved_inconsistent;
      parse_stats.header_word1_pattern_inconsistent += hcheck.word1_pattern_inconsistent;
      parse_stats.header_word1_channel_mask_inconsistent +=
          hcheck.word1_channel_mask_inconsistent;
      parse_stats.header_fail_flag_events += hcheck.fail_flag_events;
      parse_stats.header_counter_jump_events += hcheck.counter_jump_events;
      parse_stats.header_inferred_missing_events += hcheck.inferred_missing_events;
      parse_stats.header_counter_backwards += hcheck.counter_backwards;
      parse_stats.header_counter_repeats += hcheck.counter_repeats;
      parse_stats.header_time_backwards += hcheck.time_backwards;
      parse_stats.header_time_repeats += hcheck.time_repeats;

      if (hcheck.invalid) {
        parse_stats.header_invalid_packets++;
        std::cerr << "invalid event header packet=" << work.packet_id
                  << " idx=" << hcheck.bad_index
                  << " advertised_words=" << hcheck.advertised_words
                  << " remaining_words=" << hcheck.remaining_words
                  << " word0=0x" << std::hex << hcheck.bad_word << std::dec << "\n";
      }

      if (hcheck.word1_board_id_inconsistent > 0 || hcheck.word1_reserved_inconsistent > 0 ||
          hcheck.word1_pattern_inconsistent > 0 || hcheck.word1_channel_mask_inconsistent > 0 ||
          hcheck.counter_jump_events > 0 || hcheck.counter_backwards > 0 ||
          hcheck.time_backwards > 0 || hcheck.fail_flag_events > 0 ||
          hcheck.counter_repeats > 0 || hcheck.time_repeats > 0) {
        std::cerr << "header-anomaly packet=" << work.packet_id
                  << " events=" << hcheck.events
                  << " fail=" << hcheck.fail_flag_events
                  << " word1_board_id_changed=" << hcheck.word1_board_id_inconsistent
                  << " word1_reserved_changed=" << hcheck.word1_reserved_inconsistent
                  << " word1_pattern_changed=" << hcheck.word1_pattern_inconsistent
                  << " word1_chmask_changed=" << hcheck.word1_channel_mask_inconsistent
                  << " counter_jumps=" << hcheck.counter_jump_events
                  << " inferred_missing=" << hcheck.inferred_missing_events
                  << " counter_backwards=" << hcheck.counter_backwards
                  << " counter_repeats=" << hcheck.counter_repeats
                  << " time_backwards=" << hcheck.time_backwards
                  << " time_repeats=" << hcheck.time_repeats;
        if (hcheck.have_word1_sample) {
          std::cerr << " w1_ref{geo=" << hcheck.word1_board_id_ref_sample
                    << ",res=" << hcheck.word1_reserved_ref_sample
                    << ",pat=0x" << std::hex << hcheck.word1_pattern_ref_sample
                    << ",mask=0x" << hcheck.word1_channel_mask_ref_sample << std::dec << "}"
                    << " w1_seen{geo=" << hcheck.word1_board_id_seen_sample
                    << ",res=" << hcheck.word1_reserved_seen_sample
                    << ",pat=0x" << std::hex << hcheck.word1_pattern_seen_sample
                    << ",mask=0x" << hcheck.word1_channel_mask_seen_sample << std::dec << "}";
        }
        if (hcheck.have_counter_jump_sample) {
          std::cerr << " counter_prev=" << hcheck.counter_prev_sample
                    << " counter_now=" << hcheck.counter_now_sample;
        }
        if (hcheck.have_counter_backwards_sample) {
          std::cerr << " counter_prev_back=" << hcheck.counter_prev_backwards_sample
                    << " counter_now_back=" << hcheck.counter_now_backwards_sample;
        }
        if (hcheck.have_time_backwards_sample) {
          std::cerr << " time_prev=" << hcheck.time_prev_sample
                    << " time_now=" << hcheck.time_now_sample;
        }
        std::cerr << "\n";
      }

      PacketCheck check = CheckPacket(words, n_words);

      std::array<int64_t, 8> packet_ch_wave_bytes{};
      bool parsed_channels = ParseChannelWaveBytesV1724(words, n_words, &packet_ch_wave_bytes);
      if (!parsed_channels) {
        if (!check.invalid) {
          std::cerr << "channel parse failed packet=" << work.packet_id << "\n";
        }
      } else {
        for (int ch = 0; ch < 8; ++ch) {
          parse_stats.channel_wave_bytes_total[ch] += packet_ch_wave_bytes[ch];
          channel_wave_bytes_window[ch] += packet_ch_wave_bytes[ch];
        }
      }

      if (check.invalid) {
        parse_stats.invalid_markers++;
        std::cerr << "invalid marker packet=" << work.packet_id
                  << " idx=" << check.bad_index
                  << " advertised_words=" << check.advertised_words
                  << " remaining_words=" << check.remaining_words
                  << " word=0x" << std::hex << check.bad_word << std::dec << "\n";
        if (opt.dump_invalid_prefix.has_value()) {
          DumpPacket(*opt.dump_invalid_prefix, work.packet_id, work.data.data(), work.nbytes);
        }
      }
    }
  });

  while (std::chrono::steady_clock::now() < t_end) {
    read_stats.loops++;
    uint32_t status = 0;
    if (!ReadReg(handle, opt.base_address, REG_AQ_STATUS, &status)) {
      read_stats.read_errors++;
      if (!opt.continue_on_read_error) break;
      std::this_thread::sleep_for(std::chrono::microseconds(opt.read_sleep_us));
      continue;
    }

    if (status & STATUS_EVENT_FULL) read_stats.event_full_seen++;

    if ((status & STATUS_EVENT_READY) == 0) {
      if (read_stats.loops % opt.status_period == 0) {
        std::cout << "loop=" << read_stats.loops << " status=0x" << std::hex << status
                  << std::dec << " ready=0\n";
      }
      std::this_thread::sleep_for(std::chrono::microseconds(opt.read_sleep_us));
      continue;
    }

    int total_bytes = 0;
    int nb = 0;
    int cycle_ret = -999;
    bool overflow = false;
    while (true) {
      const int remaining =
          static_cast<int>(opt.buffer_bytes - static_cast<int64_t>(total_bytes));
      if (remaining <= 0) {
        overflow = true;
        std::cerr << "read overflow total=" << total_bytes
                  << " buf=" << opt.buffer_bytes << "\n";
        break;
      }
      cycle_ret = CAENVME_FIFOBLTReadCycle(
          handle, opt.base_address, read_buffer.data() + static_cast<size_t>(total_bytes),
          remaining, cvA32_U_MBLT, cvD64, &nb);

      if (cycle_ret != cvSuccess && cycle_ret != cvBusError) {
        read_stats.read_errors++;
        std::cerr << "read cycle failed ret=" << cycle_ret << " nb=" << nb << "\n";
        break;
      }
      if (cycle_ret == cvSuccess && nb == 0) {
        read_stats.read_errors++;
        cycle_ret = -998;
        std::cerr << "read cycle returned success with zero bytes\n";
        break;
      }

      if (nb < 0 || total_bytes + nb > opt.buffer_bytes) {
        overflow = true;
        std::cerr << "read overflow total=" << total_bytes << " nb=" << nb
                  << " buf=" << opt.buffer_bytes << "\n";
        break;
      }
      total_bytes += nb;

      if (cycle_ret == cvBusError) {
        read_stats.bus_error_terminations++;
        break;
      }
    }

    if (cycle_ret != cvSuccess && cycle_ret != cvBusError) {
      if (!opt.continue_on_read_error) break;
      continue;
    }
    if (overflow) {
      read_stats.read_errors++;
      if (!opt.continue_on_read_error) break;
      continue;
    }

    read_stats.packets++;
    if (total_bytes <= 0) continue;
    read_stats.packets_with_data++;
    read_stats.bytes += total_bytes;
    if (opt.save_packets) {
      if (DumpPacket(opt.save_packets_prefix, read_stats.packets, read_buffer.data(),
                     total_bytes)) {
        read_stats.saved_packets++;
      }
    }

    PacketWork work;
    work.packet_id = read_stats.packets;
    work.nbytes = total_bytes;
    work.data.assign(read_buffer.begin(), read_buffer.begin() + total_bytes);
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_not_full.wait(lock, [&]() { return packet_queue.size() < kMaxQueuedPackets; });
      packet_queue.emplace_back(std::move(work));
    }
    queue_not_empty.notify_one();

    if (read_stats.packets % opt.status_period == 0) {
      uint32_t pll = 0;
      uint32_t ros = 0;
      (void)ReadReg(handle, opt.base_address, REG_BOARD_FAIL, &pll);
      (void)ReadReg(handle, opt.base_address, REG_READOUT_STATUS, &ros);
      size_t qsize = 0;
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        qsize = packet_queue.size();
      }
      std::cout << "packet=" << read_stats.packets << " bytes=" << total_bytes
                << " cycle_ret=" << CvName(cycle_ret) << "(" << cycle_ret << ")"
                << " q=" << qsize
                << " pll=0x" << std::hex << pll << " ros=0x" << ros << std::dec
                << "\n";
    }

    if (opt.max_packets > 0 && read_stats.packets_with_data >= opt.max_packets) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(opt.read_sleep_us));
  }

  (void)WriteReg(handle, opt.base_address, REG_AQ_CTRL, stop_word);
  (void)WaitStatusBit(handle, opt.base_address, STATUS_RUN, false, 1000, 1000);

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    reader_done = true;
  }
  queue_not_empty.notify_all();
  parser_thread.join();

  auto t1 = std::chrono::steady_clock::now();
  flush_rate_windows(t1);
  double dt = std::chrono::duration<double>(t1 - t0).count();
  double mib = static_cast<double>(read_stats.bytes) / (1024.0 * 1024.0);
  const uint32_t effective_mask = opt.apply_channel_mask ? opt.channel_mask : 0xFFU;
  int enabled_channels = 0;
  for (int ch = 0; ch < 8; ++ch) {
    if (effective_mask & (1U << ch)) enabled_channels++;
  }
  if (enabled_channels <= 0) enabled_channels = 8;

  int64_t max_channel_total_bytes = 0;
  int64_t total_wave_bytes = 0;
  for (int ch = 0; ch < 8; ++ch) {
    max_channel_total_bytes =
        std::max(max_channel_total_bytes, parse_stats.channel_wave_bytes_total[ch]);
    total_wave_bytes += parse_stats.channel_wave_bytes_total[ch];
  }

  const double safe_dt = dt > 0.0 ? dt : 1.0;
  const double avg_runtime_per_channel_kibps =
      static_cast<double>(total_wave_bytes) / safe_dt /
      static_cast<double>(enabled_channels) / 1024.0;
  const double max_runtime_per_channel_kibps =
      static_cast<double>(max_channel_total_bytes) / safe_dt / 1024.0;
  const double avg_1s_over_time_channels_kib =
      (parse_stats.rate_windows_1s > 0)
          ? (static_cast<double>(parse_stats.channel_wave_bytes_windows_total) /
             static_cast<double>(parse_stats.rate_windows_1s * enabled_channels) / 1024.0)
          : 0.0;
  const double max_1s_any_channel_kib =
      static_cast<double>(parse_stats.max_channel_wave_bytes_1s) / 1024.0;

  std::cout << "\nSummary\n"
            << "  elapsed_s: " << dt << "\n"
            << "  loops: " << read_stats.loops << "\n"
            << "  packets_total: " << read_stats.packets << "\n"
            << "  packets_with_data: " << read_stats.packets_with_data << "\n"
            << "  bytes_total: " << read_stats.bytes << " (" << std::fixed
            << std::setprecision(2)
            << mib << " MiB)\n"
            << "  parsed_events: " << parse_stats.parsed_events << "\n"
            << "  invalid_markers: " << parse_stats.invalid_markers << "\n"
            << "  header_events_checked: " << parse_stats.header_events << "\n"
            << "  header_invalid_packets: " << parse_stats.header_invalid_packets << "\n"
            << "  header_word1_board_id_inconsistent: "
            << parse_stats.header_word1_board_id_inconsistent << "\n"
            << "  header_word1_reserved_inconsistent: "
            << parse_stats.header_word1_reserved_inconsistent << "\n"
            << "  header_word1_pattern_inconsistent: "
            << parse_stats.header_word1_pattern_inconsistent << "\n"
            << "  header_word1_channel_mask_inconsistent: "
            << parse_stats.header_word1_channel_mask_inconsistent << "\n"
            << "  header_fail_flag_events: " << parse_stats.header_fail_flag_events << "\n"
            << "  header_counter_jump_events: " << parse_stats.header_counter_jump_events << "\n"
            << "  header_inferred_missing_events: "
            << parse_stats.header_inferred_missing_events << "\n"
            << "  header_counter_backwards: " << parse_stats.header_counter_backwards << "\n"
            << "  header_counter_repeats: " << parse_stats.header_counter_repeats << "\n"
            << "  header_time_backwards: " << parse_stats.header_time_backwards << "\n"
            << "  header_time_repeats: " << parse_stats.header_time_repeats << "\n"
            << "  bus_error_terminations: " << read_stats.bus_error_terminations << "\n"
            << "  read_errors: " << read_stats.read_errors << "\n"
            << "  event_full_seen: " << read_stats.event_full_seen << "\n"
            << "  saved_packets: " << read_stats.saved_packets << "\n"
            << "  channel_rate_avg_runtime_per_channel_kiBps: " << std::fixed
            << std::setprecision(2) << avg_runtime_per_channel_kibps << "\n"
            << "  channel_rate_max_runtime_per_channel_kiBps: "
            << max_runtime_per_channel_kibps << "\n"
            << "  channel_rate_avg_1s_over_time_channels_kiB: "
            << avg_1s_over_time_channels_kib << "\n"
            << "  channel_rate_max_1s_any_channel_kiB: " << max_1s_any_channel_kib
            << "\n";

  cleanup();
  return 0;
}
