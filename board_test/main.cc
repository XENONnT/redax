#include <CAENVMElib.h>

#include <chrono>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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

struct CliOverrides {
  std::optional<int> link;
  std::optional<int> crate;
  std::optional<int> board_id;
  std::optional<uint32_t> base_address;
  std::optional<uint32_t> channel_mask;
  std::optional<uint16_t> fixed_dac;
  std::optional<std::vector<uint16_t>> thresholds;
  std::optional<uint32_t> trigger_mask;
  std::optional<uint32_t> post_trigger;
  std::vector<std::pair<uint32_t, uint32_t>> reg_writes;
  std::optional<std::string> start_mode;
  std::optional<bool> mode10_explicit_start_stop;
  std::optional<bool> do_reset;
  std::optional<bool> do_sn_check;
  std::optional<bool> continue_on_read_error;
  std::optional<bool> save_packets;
  std::optional<std::string> dump_invalid_prefix;
  std::optional<std::string> save_packets_prefix;
  std::optional<int> read_sleep_us;
  std::optional<int> status_period;
  std::optional<int64_t> buffer_bytes;
  std::optional<double> duration_s;
  std::optional<int64_t> max_packets;

  std::optional<std::string> config_name;
  std::optional<std::string> config_override_file;
  std::optional<std::string> host;
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

  // Redax-like channel accounting: waveform payload bytes per channel.
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

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [options]\n"
      << "Input modes:\n"
      << "  A) Explicit CLI: --link/--crate/--base (+ optional settings)\n"
      << "  B) Config merge: --config <name|file.json|path/to/file.json>\n"
      << "     Includes are resolved from the same directory as root config.\n"
      << "\n"
      << "Configuration:\n"
      << "  --config <name|file.json|path>  Root config to load\n"
      << "  --config-override-file <path>  Optional override JSON merged last\n"
      << "  --host <name>               Host key (default: <local_hostname>_reader_0)\n"
      << "  --link <int>                Optical link number\n"
      << "  --crate <int>               Crate number\n"
      << "  --base <hex|dec>            VME base address (e.g. 0x32100000)\n"
      << "  --board-id <int>            Board ID for logging\n"
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
      << "  --duration-s <float>        Run duration in seconds (default 30)\n"
      << "  --max-packets <int>         Stop after N packets with data (0 = unlimited)\n"
      << "  --sleep-us <int>            Poll sleep in us (default 10)\n"
      << "  --buffer-bytes <int>        BLT read buffer bytes (default 0x800000)\n"
      << "  --status-period <int>       Print status every N loops (default 1000)\n"
      << "  --continue-on-read-error    Continue after read error\n"
      << "  --save-packets              Save all non-empty data packets\n"
      << "  --save-packets-prefix <p>   Packet dump prefix (default 'packet')\n"
      << "  --dump-invalid-prefix <path_prefix>  Dump invalid packets to binary files\n"
      << "\n";
}

bool ParseU32(const std::string& s, uint32_t* out) {
  auto in_u32_range = [](unsigned long long v) {
    return v <= 0xFFFFFFFFULL;
  };
  try {
    size_t idx = 0;
    unsigned long long v = std::stoull(s, &idx, 0);
    if (idx != s.size() || !in_u32_range(v)) {
      return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    // Accept bare hex like "FFFF0000" used in some exported configs.
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
    if (idx != s.size() || v < INT32_MIN || v > INT32_MAX) {
      return false;
    }
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
    if (idx != s.size()) {
      return false;
    }
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
    if (idx != s.size() || !std::isfinite(v)) {
      return false;
    }
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
    if (!ParseU32(token, &v) || v > 0xFFFF) {
      return false;
    }
    vals.push_back(static_cast<uint16_t>(v));
  }
  if (vals.size() != 8) {
    return false;
  }
  *out = vals;
  return true;
}

bool ParseRegWrite(const std::string& s, std::pair<uint32_t, uint32_t>* out) {
  size_t eq = s.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= s.size()) {
    return false;
  }
  uint32_t reg = 0;
  uint32_t val = 0;
  if (!ParseU32(s.substr(0, eq), &reg) || !ParseU32(s.substr(eq + 1), &val)) {
    return false;
  }
  *out = {reg, val};
  return true;
}

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  bool b = false;
  double n = 0.0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;

  static Json Null() { return Json{}; }
  static Json Bool(bool v) {
    Json j;
    j.type = Type::kBool;
    j.b = v;
    return j;
  }
  static Json Number(double v) {
    Json j;
    j.type = Type::kNumber;
    j.n = v;
    return j;
  }
  static Json String(std::string v) {
    Json j;
    j.type = Type::kString;
    j.s = std::move(v);
    return j;
  }
  static Json Array(std::vector<Json> v) {
    Json j;
    j.type = Type::kArray;
    j.a = std::move(v);
    return j;
  }
  static Json Object(std::map<std::string, Json> v) {
    Json j;
    j.type = Type::kObject;
    j.o = std::move(v);
    return j;
  }

  bool IsNull() const { return type == Type::kNull; }
  bool IsBool() const { return type == Type::kBool; }
  bool IsNumber() const { return type == Type::kNumber; }
  bool IsString() const { return type == Type::kString; }
  bool IsArray() const { return type == Type::kArray; }
  bool IsObject() const { return type == Type::kObject; }

  const Json* Find(const std::string& key) const {
    if (!IsObject()) return nullptr;
    auto it = o.find(key);
    if (it == o.end()) return nullptr;
    return &it->second;
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string text) : src_(std::move(text)) {}

  bool Parse(Json* out, std::string* err) {
    SkipWs();
    if (!ParseValue(out, err)) return false;
    SkipWs();
    if (pos_ != src_.size()) {
      *err = "trailing characters after JSON value";
      return false;
    }
    return true;
  }

 private:
  bool ParseValue(Json* out, std::string* err) {
    SkipWs();
    if (pos_ >= src_.size()) {
      *err = "unexpected end of input";
      return false;
    }
    char c = src_[pos_];
    if (c == '{') return ParseObject(out, err);
    if (c == '[') return ParseArray(out, err);
    if (c == '"') return ParseString(out, err);
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return ParseNumber(out, err);
    }
    if (ConsumeLiteral("true")) {
      *out = Json::Bool(true);
      return true;
    }
    if (ConsumeLiteral("false")) {
      *out = Json::Bool(false);
      return true;
    }
    if (ConsumeLiteral("null")) {
      *out = Json::Null();
      return true;
    }
    *err = "invalid JSON token";
    return false;
  }

  bool ParseObject(Json* out, std::string* err) {
    if (!ConsumeChar('{')) {
      *err = "expected '{'";
      return false;
    }
    SkipWs();
    std::map<std::string, Json> obj;
    if (ConsumeChar('}')) {
      *out = Json::Object(std::move(obj));
      return true;
    }
    while (true) {
      Json key_json;
      if (!ParseString(&key_json, err)) return false;
      if (!ConsumeChar(':')) {
        *err = "expected ':' after object key";
        return false;
      }
      Json value;
      if (!ParseValue(&value, err)) return false;
      obj[key_json.s] = std::move(value);
      SkipWs();
      if (ConsumeChar('}')) break;
      if (!ConsumeChar(',')) {
        *err = "expected ',' or '}' in object";
        return false;
      }
    }
    *out = Json::Object(std::move(obj));
    return true;
  }

  bool ParseArray(Json* out, std::string* err) {
    if (!ConsumeChar('[')) {
      *err = "expected '['";
      return false;
    }
    SkipWs();
    std::vector<Json> arr;
    if (ConsumeChar(']')) {
      *out = Json::Array(std::move(arr));
      return true;
    }
    while (true) {
      Json value;
      if (!ParseValue(&value, err)) return false;
      arr.push_back(std::move(value));
      SkipWs();
      if (ConsumeChar(']')) break;
      if (!ConsumeChar(',')) {
        *err = "expected ',' or ']' in array";
        return false;
      }
    }
    *out = Json::Array(std::move(arr));
    return true;
  }

  bool ParseString(Json* out, std::string* err) {
    if (!ConsumeChar('"')) {
      *err = "expected string opening quote";
      return false;
    }
    std::string result;
    while (pos_ < src_.size()) {
      char c = src_[pos_++];
      if (c == '"') {
        *out = Json::String(std::move(result));
        return true;
      }
      if (c == '\\') {
        if (pos_ >= src_.size()) {
          *err = "unterminated escape sequence";
          return false;
        }
        char e = src_[pos_++];
        switch (e) {
          case '"':
          case '\\':
          case '/':
            result.push_back(e);
            break;
          case 'b':
            result.push_back('\b');
            break;
          case 'f':
            result.push_back('\f');
            break;
          case 'n':
            result.push_back('\n');
            break;
          case 'r':
            result.push_back('\r');
            break;
          case 't':
            result.push_back('\t');
            break;
          case 'u': {
            // Keep parser dependency-free: decode simple ASCII subset, fallback '?'
            if (pos_ + 4 > src_.size()) {
              *err = "invalid unicode escape";
              return false;
            }
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
              char h = src_[pos_++];
              code <<= 4U;
              if (h >= '0' && h <= '9')
                code |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f')
                code |= static_cast<unsigned>(10 + h - 'a');
              else if (h >= 'A' && h <= 'F')
                code |= static_cast<unsigned>(10 + h - 'A');
              else {
                *err = "invalid unicode hex digit";
                return false;
              }
            }
            result.push_back(code <= 0x7F ? static_cast<char>(code) : '?');
            break;
          }
          default:
            *err = "invalid string escape character";
            return false;
        }
      } else {
        result.push_back(c);
      }
    }
    *err = "unterminated string";
    return false;
  }

  bool ParseNumber(Json* out, std::string* err) {
    size_t start = pos_;
    if (src_[pos_] == '-') pos_++;
    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
    if (pos_ < src_.size() && src_[pos_] == '.') {
      pos_++;
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
    }
    if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
      pos_++;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) pos_++;
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) pos_++;
    }
    std::string token = src_.substr(start, pos_ - start);
    char* endp = nullptr;
    double v = std::strtod(token.c_str(), &endp);
    if (endp == token.c_str() || *endp != '\0') {
      *err = "invalid number token";
      return false;
    }
    *out = Json::Number(v);
    return true;
  }

  bool ConsumeLiteral(const char* lit) {
    size_t len = std::strlen(lit);
    if (src_.compare(pos_, len, lit) == 0) {
      pos_ += len;
      return true;
    }
    return false;
  }

  bool ConsumeChar(char c) {
    SkipWs();
    if (pos_ < src_.size() && src_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }

  void SkipWs() {
    while (pos_ < src_.size() &&
           std::isspace(static_cast<unsigned char>(src_[pos_]))) {
      pos_++;
    }
  }

  std::string src_;
  size_t pos_ = 0;
};

std::string JoinPath(const std::string& dir, const std::string& file) {
  if (dir.empty()) return file;
  if (dir.back() == '/') return dir + file;
  return dir + "/" + file;
}

std::string AsConfigFileName(const std::string& name) {
  if (name.size() >= 5 && name.substr(name.size() - 5) == ".json") return name;
  return name + ".json";
}

bool IsAbsolutePath(const std::string& path) {
  return !path.empty() && path.front() == '/';
}

std::string DirName(const std::string& path) {
  const size_t p = path.find_last_of('/');
  if (p == std::string::npos) return ".";
  if (p == 0) return "/";
  return path.substr(0, p);
}

bool ReadTextFile(const std::string& path, std::string* out, std::string* err) {
  std::ifstream in(path);
  if (!in.is_open()) {
    *err = "cannot open file: " + path;
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

bool LoadJsonFile(const std::string& path, Json* out, std::string* err) {
  std::string text;
  if (!ReadTextFile(path, &text, err)) return false;
  JsonParser parser(std::move(text));
  if (!parser.Parse(out, err)) {
    *err = "JSON parse error in " + path + ": " + *err;
    return false;
  }
  if (!out->IsObject()) {
    *err = "top-level JSON must be an object in " + path;
    return false;
  }
  return true;
}

Json MergeObjectsShallow(const std::vector<Json>& docs) {
  Json merged = Json::Object({});
  for (const auto& d : docs) {
    if (!d.IsObject()) continue;
    for (const auto& kv : d.o) merged.o[kv.first] = kv.second;
  }
  return merged;
}

bool JsonToString(const Json& j, std::string* out) {
  if (!j.IsString()) return false;
  *out = j.s;
  return true;
}

bool JsonToInt(const Json& j, int* out) {
  if (j.IsNumber()) {
    if (!std::isfinite(j.n)) return false;
    double truncated = std::trunc(j.n);
    if (truncated != j.n) return false;
    if (truncated < static_cast<double>(INT32_MIN) ||
        truncated > static_cast<double>(INT32_MAX)) {
      return false;
    }
    *out = static_cast<int>(truncated);
    return true;
  }
  if (j.IsString()) return ParseI32(j.s, out);
  return false;
}

bool JsonToU32(const Json& j, uint32_t* out) {
  if (j.IsNumber()) {
    if (!std::isfinite(j.n)) return false;
    double truncated = std::trunc(j.n);
    if (truncated != j.n) return false;
    if (truncated < 0.0 || truncated > static_cast<double>(0xFFFFFFFFULL)) return false;
    *out = static_cast<uint32_t>(truncated);
    return true;
  }
  if (j.IsString()) return ParseU32(j.s, out);
  return false;
}

bool JsonToBool(const Json& j, bool* out) {
  if (j.IsBool()) {
    *out = j.b;
    return true;
  }
  if (j.IsNumber()) {
    *out = (j.n != 0.0);
    return true;
  }
  if (j.IsString()) {
    if (j.s == "true" || j.s == "1") {
      *out = true;
      return true;
    }
    if (j.s == "false" || j.s == "0") {
      *out = false;
      return true;
    }
  }
  return false;
}

std::string GetLocalHostname() {
  char host[256] = {0};
  if (gethostname(host, sizeof(host) - 1) != 0) return "";
  host[sizeof(host) - 1] = '\0';
  return std::string(host);
}

bool IsDigitizerType(const std::string& type) {
  return type == "V1724" || type == "V1730" || type == "V1724_MV" ||
         type == "f1724";
}

bool ResolveMergedConfig(const std::string& config_spec,
                         const std::optional<std::string>& override_file, Json* out,
                         std::string* err) {
  Json root;
  const std::string root_name = AsConfigFileName(config_spec);
  const std::string root_path = root_name;
  const std::string root_dir = DirName(root_path);
  if (!LoadJsonFile(root_path, &root, err)) return false;

  std::vector<Json> docs;
  const Json* includes = root.Find("includes");
  if (includes && includes->IsArray()) {
    for (const auto& inc : includes->a) {
      std::string inc_name;
      if (!JsonToString(inc, &inc_name)) {
        *err = "includes entries must be strings";
        return false;
      }
      Json inc_doc;
      const std::string inc_path = JoinPath(root_dir, AsConfigFileName(inc_name));
      if (!LoadJsonFile(inc_path, &inc_doc, err)) return false;
      docs.push_back(std::move(inc_doc));
    }
  }
  docs.push_back(std::move(root));
  Json merged = MergeObjectsShallow(docs);

  if (override_file.has_value()) {
    Json ov;
    const std::string ov_name = AsConfigFileName(*override_file);
    const std::string ov_path =
        IsAbsolutePath(ov_name) ? ov_name : JoinPath(root_dir, ov_name);
    if (!LoadJsonFile(ov_path, &ov, err)) return false;
    merged = MergeObjectsShallow({merged, ov});
  }

  *out = std::move(merged);
  return true;
}

bool ApplyMergedConfigToOptions(const Json& cfg, const std::string& host,
                                const std::optional<int>& preferred_board_id,
                                Options* opt, std::string* err) {
  std::string detector;
  if (const Json* dets = cfg.Find("detectors"); dets && dets->IsObject()) {
    if (const Json* d = dets->Find(host); d && d->IsString()) detector = d->s;
  }

  const Json* boards = cfg.Find("boards");
  if (!boards || !boards->IsArray()) {
    *err = "config has no 'boards' array";
    return false;
  }

  const Json* selected_board = nullptr;
  for (const auto& b : boards->a) {
    if (!b.IsObject()) continue;

    std::string btype;
    if (const Json* t = b.Find("type")) {
      if (!JsonToString(*t, &btype)) continue;
      if (!IsDigitizerType(btype)) continue;
    } else {
      continue;
    }

    if (const Json* skip = b.Find("skip")) {
      bool skip_v = false;
      if (JsonToBool(*skip, &skip_v) && skip_v) continue;
    }

    if (const Json* host_j = b.Find("host")) {
      std::string board_host;
      if (JsonToString(*host_j, &board_host) && !board_host.empty() &&
          !host.empty() && board_host != host) {
        continue;
      }
    }

    int bid = -1;
    if (const Json* board_id = b.Find("board")) {
      if (!JsonToInt(*board_id, &bid)) continue;
    } else {
      continue;
    }

    if (preferred_board_id.has_value() && bid != *preferred_board_id) continue;

    selected_board = &b;
    break;
  }

  if (!selected_board) {
    *err = preferred_board_id.has_value()
               ? "no eligible board matched requested board-id in config"
               : "no eligible V17XX board found in config";
    return false;
  }

  int link = -1;
  int crate = -1;
  int board_id = -1;
  uint32_t base = 0;
  const Json* link_j = selected_board->Find("link");
  const Json* crate_j = selected_board->Find("crate");
  const Json* board_j = selected_board->Find("board");
  const Json* base_j = selected_board->Find("vme_address");
  if (!link_j || !crate_j || !board_j || !base_j ||
      !JsonToInt(*link_j, &link) || !JsonToInt(*crate_j, &crate) ||
      !JsonToInt(*board_j, &board_id) || !JsonToU32(*base_j, &base)) {
    *err = "selected board is missing link/crate/board/vme_address";
    return false;
  }
  opt->link = link;
  opt->crate = crate;
  opt->board_id = board_id;
  opt->base_address = base;

  if (const Json* j = cfg.Find("baseline_fixed_value")) {
    int v = 0;
    if (JsonToInt(*j, &v) && v >= 0 && v <= 0xFFFF) {
      opt->fixed_dac = static_cast<uint16_t>(v);
    }
  }
  if (const Json* j = cfg.Find("channel_mask")) {
    uint32_t v = 0;
    if (JsonToU32(*j, &v)) {
      opt->channel_mask = v;
      opt->apply_channel_mask = true;
    }
  }
  if (const Json* j = cfg.Find("trigger_mask")) {
    uint32_t v = 0;
    if (JsonToU32(*j, &v)) opt->trigger_mask = v;
  }
  if (const Json* j = cfg.Find("post_trigger")) {
    uint32_t v = 0;
    if (JsonToU32(*j, &v)) opt->post_trigger = v;
  }
  if (const Json* j = cfg.Find("do_sn_check")) {
    bool b = false;
    if (JsonToBool(*j, &b)) opt->do_sn_check = b;
  }
  if (const Json* j = cfg.Find("run_start")) {
    int v = 0;
    if (JsonToInt(*j, &v)) opt->start_mode = (v == 0 ? "software" : "sin");
  }
  if (const Json* j = cfg.Find("us_between_reads")) {
    int v = 0;
    if (JsonToInt(*j, &v) && v >= 0) opt->read_sleep_us = v;
  }

  if (const Json* t_all = cfg.Find("thresholds"); t_all && t_all->IsObject()) {
    std::string key = std::to_string(board_id);
    if (const Json* t = t_all->Find(key); t && t->IsArray() && !t->a.empty()) {
      std::vector<uint16_t> out(8, 0xA);
      for (size_t i = 0; i < 8 && i < t->a.size(); ++i) {
        int v = 0;
        if (JsonToInt(t->a[i], &v) && v >= 0 && v <= 0xFFFF) {
          out[i] = static_cast<uint16_t>(v);
        }
      }
      opt->thresholds = std::move(out);
    }
  }

  if (const Json* regs = cfg.Find("registers"); regs && regs->IsArray()) {
    for (const auto& r : regs->a) {
      if (!r.IsObject()) continue;
      bool use = false;
      if (const Json* bj = r.Find("board")) {
        if (bj->IsNumber() || bj->IsString()) {
          int board_tag = -1;
          if (JsonToInt(*bj, &board_tag)) {
            use = (board_tag == board_id);
          } else if (bj->IsString()) {
            use = (bj->s == "all" || (!detector.empty() && bj->s == detector));
          }
        }
      }
      if (!use) continue;
      const Json* regj = r.Find("reg");
      const Json* valj = r.Find("val");
      if (!regj || !valj) continue;
      uint32_t reg = 0;
      uint32_t val = 0;
      if (!JsonToU32(*regj, &reg) || !JsonToU32(*valj, &val)) continue;
      opt->reg_writes.emplace_back(reg, val);
    }
  }

  return true;
}

void ApplyCliOverrides(const CliOverrides& cli, Options* opt) {
  if (cli.link.has_value()) opt->link = *cli.link;
  if (cli.crate.has_value()) opt->crate = *cli.crate;
  if (cli.board_id.has_value()) opt->board_id = *cli.board_id;
  if (cli.base_address.has_value()) opt->base_address = *cli.base_address;
  if (cli.channel_mask.has_value()) {
    opt->channel_mask = *cli.channel_mask;
    opt->apply_channel_mask = true;
  }
  if (cli.fixed_dac.has_value()) opt->fixed_dac = *cli.fixed_dac;
  if (cli.thresholds.has_value()) opt->thresholds = *cli.thresholds;
  if (cli.trigger_mask.has_value()) opt->trigger_mask = *cli.trigger_mask;
  if (cli.post_trigger.has_value()) opt->post_trigger = *cli.post_trigger;
  if (!cli.reg_writes.empty()) {
    for (const auto& rv : cli.reg_writes) opt->reg_writes.push_back(rv);
  }
  if (cli.start_mode.has_value()) opt->start_mode = *cli.start_mode;
  if (cli.mode10_explicit_start_stop.has_value()) {
    opt->mode10_explicit_start_stop = *cli.mode10_explicit_start_stop;
  }
  if (cli.do_reset.has_value()) opt->do_reset = *cli.do_reset;
  if (cli.do_sn_check.has_value()) opt->do_sn_check = *cli.do_sn_check;
  if (cli.continue_on_read_error.has_value()) {
    opt->continue_on_read_error = *cli.continue_on_read_error;
  }
  if (cli.save_packets.has_value()) opt->save_packets = *cli.save_packets;
  if (cli.dump_invalid_prefix.has_value()) {
    opt->dump_invalid_prefix = *cli.dump_invalid_prefix;
  }
  if (cli.save_packets_prefix.has_value()) {
    opt->save_packets_prefix = *cli.save_packets_prefix;
  }
  if (cli.read_sleep_us.has_value()) opt->read_sleep_us = *cli.read_sleep_us;
  if (cli.status_period.has_value()) opt->status_period = *cli.status_period;
  if (cli.buffer_bytes.has_value()) opt->buffer_bytes = *cli.buffer_bytes;
  if (cli.duration_s.has_value()) opt->duration_s = *cli.duration_s;
  if (cli.max_packets.has_value()) opt->max_packets = *cli.max_packets;
}

bool ParseArgs(int argc, char** argv, CliOverrides* cli) {
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
    } else if (a == "--config") {
      cli->config_name = need("--config");
    } else if (a == "--config-override-file") {
      cli->config_override_file = need("--config-override-file");
    } else if (a == "--host") {
      cli->host = need("--host");
    } else if (a == "--link") {
      int v = 0;
      if (!ParseI32(need("--link"), &v)) return false;
      cli->link = v;
    } else if (a == "--crate") {
      int v = 0;
      if (!ParseI32(need("--crate"), &v)) return false;
      cli->crate = v;
    } else if (a == "--board-id") {
      int v = 0;
      if (!ParseI32(need("--board-id"), &v)) return false;
      cli->board_id = v;
    } else if (a == "--base") {
      uint32_t v = 0;
      if (!ParseU32(need("--base"), &v)) return false;
      cli->base_address = v;
    } else if (a == "--channel-mask") {
      uint32_t v = 0;
      if (!ParseU32(need("--channel-mask"), &v)) return false;
      cli->channel_mask = v;
    } else if (a == "--fixed-dac") {
      uint32_t v = 0;
      if (!ParseU32(need("--fixed-dac"), &v) || v > 0xFFFF) return false;
      cli->fixed_dac = static_cast<uint16_t>(v);
    } else if (a == "--threshold") {
      uint32_t v = 0;
      if (!ParseU32(need("--threshold"), &v) || v > 0xFFFF) return false;
      cli->thresholds = std::vector<uint16_t>(8, static_cast<uint16_t>(v));
    } else if (a == "--thresholds") {
      std::vector<uint16_t> vals;
      if (!ParseThresholdList(need("--thresholds"), &vals)) return false;
      cli->thresholds = std::move(vals);
    } else if (a == "--trigger-mask") {
      uint32_t v = 0;
      if (!ParseU32(need("--trigger-mask"), &v)) return false;
      cli->trigger_mask = v;
    } else if (a == "--post-trigger") {
      uint32_t v = 0;
      if (!ParseU32(need("--post-trigger"), &v)) return false;
      cli->post_trigger = v;
    } else if (a == "--reg") {
      std::pair<uint32_t, uint32_t> rv;
      if (!ParseRegWrite(need("--reg"), &rv)) return false;
      cli->reg_writes.push_back(rv);
    } else if (a == "--start-mode") {
      std::string v = need("--start-mode");
      if (v != "software" && v != "sin") return false;
      cli->start_mode = v;
    } else if (a == "--mode10-explicit-start-stop") {
      cli->mode10_explicit_start_stop = true;
    } else if (a == "--duration-s") {
      double v = 0.0;
      if (!ParseDouble(need("--duration-s"), &v) || v <= 0.0) {
        return false;
      }
      cli->duration_s = v;
    } else if (a == "--max-packets") {
      int64_t v = 0;
      if (!ParseI64(need("--max-packets"), &v) || v < 0) {
        return false;
      }
      cli->max_packets = v;
    } else if (a == "--sleep-us") {
      int v = 0;
      if (!ParseI32(need("--sleep-us"), &v) || v < 0) {
        return false;
      }
      cli->read_sleep_us = v;
    } else if (a == "--status-period") {
      int v = 0;
      if (!ParseI32(need("--status-period"), &v) || v <= 0) {
        return false;
      }
      cli->status_period = v;
    } else if (a == "--buffer-bytes") {
      int64_t v = 0;
      if (!ParseI64(need("--buffer-bytes"), &v) || v <= 0) {
        return false;
      }
      cli->buffer_bytes = v;
    } else if (a == "--dump-invalid-prefix") {
      cli->dump_invalid_prefix = need("--dump-invalid-prefix");
    } else if (a == "--continue-on-read-error") {
      cli->continue_on_read_error = true;
    } else if (a == "--save-packets") {
      cli->save_packets = true;
    } else if (a == "--save-packets-prefix") {
      cli->save_packets_prefix = need("--save-packets-prefix");
      cli->save_packets = true;
    } else if (a == "--no-reset") {
      cli->do_reset = false;
    } else if (a == "--do-sn-check") {
      cli->do_sn_check = true;
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

bool WaitStatusBit(int handle, uint32_t base, uint32_t mask, bool want_set, int ntries,
                   int sleep_us) {
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

// Parse V1724 event/channel headers and accumulate waveform payload bytes per channel.
// This mirrors redax's GetDataPerChan basis: samples_in_pulse * sizeof(uint16_t),
// which is equivalent to (channel_words - 2) * sizeof(uint32_t) for V1724.
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
    if (ev_words < 4U || ev_words > remaining) {
      return false;
    }

    const size_t ev_end = i + static_cast<size_t>(ev_words);
    const uint32_t channel_mask = words[i + 1] & 0xFFU;
    size_t ev_pos = i + 4;  // first channel header in V1724 format

    for (int ch = 0; ch < 8; ++ch) {
      if ((channel_mask & (1U << ch)) == 0U) continue;
      if (ev_pos >= ev_end) return false;

      const uint32_t ch_words = words[ev_pos] & 0x7FFFFFU;
      if (ch_words < 2U) return false;
      const size_t ch_words_sz = static_cast<size_t>(ch_words);
      if (ev_pos + ch_words_sz > ev_end) return false;

      (*out_bytes)[ch] += static_cast<int64_t>(ch_words - 2U) * static_cast<int64_t>(sizeof(uint32_t));
      ev_pos += ch_words_sz;
    }
    i = ev_end;
  }
  return true;
}

bool DumpPacket(const std::string& prefix, int64_t packet_idx, const uint8_t* data,
                int64_t nbytes) {
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
  CliOverrides cli;
  if (!ParseArgs(argc, argv, &cli)) {
    PrintUsage(argv[0]);
    return 2;
  }

  Options opt;
  const std::string local_host = GetLocalHostname();
  const std::string default_host =
      local_host.empty() ? std::string("hostname_reader_0")
                         : local_host + "_reader_0";
  const std::string host = cli.host.has_value() ? *cli.host : default_host;
  std::cout << "Host key: " << host;
  if (!cli.host.has_value()) std::cout << " (default)";
  std::cout << "\n";

  if (cli.config_name.has_value()) {
    Json cfg;
    std::string err;
    if (!ResolveMergedConfig(*cli.config_name, cli.config_override_file, &cfg, &err)) {
      std::cerr << "Config load failed: " << err << "\n";
      return 2;
    }
    if (!ApplyMergedConfigToOptions(cfg, host, cli.board_id, &opt, &err)) {
      std::cerr << "Config apply failed: " << err << "\n";
      return 2;
    }
  }

  ApplyCliOverrides(cli, &opt);
  if (opt.link < 0 || opt.crate < 0 || opt.base_address == 0) {
    std::cerr << "Missing required board connection parameters. Provide --link/--crate/--base"
              << " or load them via --config.\n";
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

  std::cout << "Connected. link=" << opt.link << " crate=" << opt.crate << " base=0x"
            << std::hex << opt.base_address << std::dec << "\n";

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
      int sn = static_cast<int>(lsb & 0xFFU) | (static_cast<int>(msb & 0xFFU) << 8U);
      std::cout << "SN read: " << sn << "\n";
    }
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
    // [1:0]=10 first-trigger controlled, bit[2]=1 arm, bit[2]=0 disarm/stop.
    start_word = 0x106;
    stop_word = 0x102;
    issue_sw_start = true;
    std::cout << "Mode10 explicit start/stop enabled. AQ_CTRL start=0x106 stop=0x102\n";
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
    std::cout << "Issued explicit SW trigger start pulse (0x8108=0x1)\n";
  }
  if (!WaitStatusBit(handle, opt.base_address, STATUS_RUN, true, 1000, 1000)) {
    std::cerr << "warning: run bit did not assert\n";
  }

  Stats stats;
  std::array<int64_t, 8> channel_wave_bytes_window{};
  constexpr auto kRateWindow = std::chrono::seconds(1);
  std::vector<uint8_t> read_buffer(static_cast<size_t>(opt.buffer_bytes), 0);
  auto t0 = std::chrono::steady_clock::now();
  auto t_end = t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(opt.duration_s));
  auto rate_window_start = t0;

  auto flush_rate_windows = [&](std::chrono::steady_clock::time_point now) {
    while (rate_window_start + kRateWindow <= now) {
      for (int ch = 0; ch < 8; ++ch) {
        stats.channel_wave_bytes_windows_total += channel_wave_bytes_window[ch];
        stats.max_channel_wave_bytes_1s =
            std::max(stats.max_channel_wave_bytes_1s, channel_wave_bytes_window[ch]);
        channel_wave_bytes_window[ch] = 0;
      }
      stats.rate_windows_1s++;
      rate_window_start += kRateWindow;
    }
  };

  while (std::chrono::steady_clock::now() < t_end) {
    flush_rate_windows(std::chrono::steady_clock::now());
    stats.loops++;
    uint32_t status = 0;
    if (!ReadReg(handle, opt.base_address, REG_AQ_STATUS, &status)) {
      stats.read_errors++;
      if (!opt.continue_on_read_error) break;
      std::this_thread::sleep_for(std::chrono::microseconds(opt.read_sleep_us));
      continue;
    }

    if (status & STATUS_EVENT_FULL) stats.event_full_seen++;

    if ((status & STATUS_EVENT_READY) == 0) {
      if (stats.loops % opt.status_period == 0) {
        std::cout << "loop=" << stats.loops << " status=0x" << std::hex << status
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
        stats.read_errors++;
        std::cerr << "read cycle failed ret=" << cycle_ret << " nb=" << nb << "\n";
        break;
      }
      if (cycle_ret == cvSuccess && nb == 0) {
        stats.read_errors++;
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
        stats.bus_error_terminations++;
        break;
      }
    }

    if (cycle_ret != cvSuccess && cycle_ret != cvBusError) {
      if (!opt.continue_on_read_error) break;
      continue;
    }
    if (overflow) {
      stats.read_errors++;
      if (!opt.continue_on_read_error) break;
      continue;
    }

    stats.packets++;
    if (total_bytes <= 0) continue;
    stats.packets_with_data++;
    stats.bytes += total_bytes;
    if (opt.save_packets) {
      if (DumpPacket(opt.save_packets_prefix, stats.packets, read_buffer.data(),
                     total_bytes)) {
        stats.saved_packets++;
      }
    }

    size_t n_words = static_cast<size_t>(total_bytes / 4);
    const auto* words = reinterpret_cast<const uint32_t*>(read_buffer.data());
    PacketCheck check = CheckPacket(words, n_words);
    stats.parsed_events += check.events;

    std::array<int64_t, 8> packet_ch_wave_bytes{};
    bool parsed_channels = ParseChannelWaveBytesV1724(words, n_words, &packet_ch_wave_bytes);
    if (!parsed_channels) {
      if (!check.invalid) {
        std::cerr << "channel parse failed packet=" << stats.packets << "\n";
      }
    } else {
      for (int ch = 0; ch < 8; ++ch) {
        stats.channel_wave_bytes_total[ch] += packet_ch_wave_bytes[ch];
        channel_wave_bytes_window[ch] += packet_ch_wave_bytes[ch];
      }
    }

    if (check.invalid) {
      stats.invalid_markers++;
      std::cerr << "invalid marker packet=" << stats.packets
                << " idx=" << check.bad_index
                << " advertised_words=" << check.advertised_words
                << " remaining_words=" << check.remaining_words
                << " word=0x" << std::hex << check.bad_word << std::dec << "\n";
      if (opt.dump_invalid_prefix.has_value()) {
        DumpPacket(*opt.dump_invalid_prefix, stats.packets, read_buffer.data(),
                   total_bytes);
      }
    }

    if (stats.packets % opt.status_period == 0) {
      uint32_t pll = 0;
      uint32_t ros = 0;
      (void)ReadReg(handle, opt.base_address, REG_BOARD_FAIL, &pll);
      (void)ReadReg(handle, opt.base_address, REG_READOUT_STATUS, &ros);
      std::cout << "packet=" << stats.packets << " bytes=" << total_bytes
                << " events=" << check.events
                << " invalid=" << (check.invalid ? 1 : 0)
                << " cycle_ret=" << CvName(cycle_ret) << "(" << cycle_ret << ")"
                << " pll=0x" << std::hex << pll << " ros=0x" << ros << std::dec
                << "\n";
    }

    if (opt.max_packets > 0 && stats.packets_with_data >= opt.max_packets) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::microseconds(opt.read_sleep_us));
  }

  (void)WriteReg(handle, opt.base_address, REG_AQ_CTRL, stop_word);
  (void)WaitStatusBit(handle, opt.base_address, STATUS_RUN, false, 1000, 1000);

  auto t1 = std::chrono::steady_clock::now();
  flush_rate_windows(t1);
  double dt = std::chrono::duration<double>(t1 - t0).count();
  double mib = static_cast<double>(stats.bytes) / (1024.0 * 1024.0);
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
        std::max(max_channel_total_bytes, stats.channel_wave_bytes_total[ch]);
    total_wave_bytes += stats.channel_wave_bytes_total[ch];
  }

  const double safe_dt = dt > 0.0 ? dt : 1.0;
  const double avg_runtime_per_channel_kibps =
      static_cast<double>(total_wave_bytes) / safe_dt / static_cast<double>(enabled_channels) /
      1024.0;
  const double max_runtime_per_channel_kibps =
      static_cast<double>(max_channel_total_bytes) / safe_dt / 1024.0;
  const double avg_1s_over_time_channels_kib =
      (stats.rate_windows_1s > 0)
          ? (static_cast<double>(stats.channel_wave_bytes_windows_total) /
             static_cast<double>(stats.rate_windows_1s * enabled_channels) / 1024.0)
          : 0.0;
  const double max_1s_any_channel_kib =
      static_cast<double>(stats.max_channel_wave_bytes_1s) / 1024.0;

  std::cout << "\nSummary\n"
            << "  elapsed_s: " << dt << "\n"
            << "  loops: " << stats.loops << "\n"
            << "  packets_total: " << stats.packets << "\n"
            << "  packets_with_data: " << stats.packets_with_data << "\n"
            << "  bytes_total: " << stats.bytes << " (" << std::fixed << std::setprecision(2)
            << mib << " MiB)\n"
            << "  parsed_events: " << stats.parsed_events << "\n"
            << "  invalid_markers: " << stats.invalid_markers << "\n"
            << "  bus_error_terminations: " << stats.bus_error_terminations << "\n"
            << "  read_errors: " << stats.read_errors << "\n"
            << "  event_full_seen: " << stats.event_full_seen << "\n"
            << "  saved_packets: " << stats.saved_packets << "\n"
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
