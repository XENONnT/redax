#include "V1730.hh"
#include "MongoLog.hh"
#include "Options.hh"

V1730::V1730(std::shared_ptr<MongoLog>& log, std::shared_ptr<Options>& options, int bid, unsigned address)
  :V1724(log, options, bid, address){
  fNChannels = 16;
  fSampleWidth = 2;
  fClockCycle = 2;
  fArtificialDeadtimeChannel = 799;
  fDefaultDelay = 2*fSampleWidth*0xA; // see register document
  fDefaultPreTrig = 6*fSampleWidth; // undocumented value?
  fBufferSize = 0x1400000; // 640 kS/ch
}

V1730::~V1730(){}

std::tuple<int, int, bool, uint32_t, uint32_t> V1730::UnpackEventHeader(std::u32string_view sv) {
  // returns {words this event, channel mask, board fail, header timestamp, event counter}
  if (sv.size() < 4)
    return {int(sv.size()), 0, true, 0xFFFFFFFF, 0xFFFFFFFF};
  // Header fields per CAEN V1730/V1725
  // - event size: bits[27:0] of w0
  // - channel mask[7:0]: bits[7:0] of w1
  // - channel mask[15:8]: bits[31:24] of w2
  // - event counter: bits[23:0] of w2
  // - board fail flag: bit[26] of w1
  // - trigger time tag: bits[30:0] of w3
  return {sv[0]&0xFFFFFFF,
          int((sv[1] & 0x000000FF) | ((sv[2] & 0xFF000000) >> 16)),
          bool(sv[1] & 0x04000000),
          sv[3] & 0x7FFFFFFF,
          sv[2] & 0x00FFFFFF};
}

std::tuple<int64_t, int, uint16_t, std::u32string_view>
V1730::UnpackChannelHeader(std::u32string_view sv, long, uint32_t, uint32_t, int, int, short ch) {
  // returns {timestamp (ns), words this channel, baseline, waveform}
  if (sv.size() < 3)
    return {0, int(sv.size()), 0, sv.substr(sv.size(), 0)};
  int words = sv[0]&0x7FFFFF;
  const uint64_t time_tag = uint64_t(uint32_t(sv[1])) | (uint64_t(sv[2] & 0x0000FFFF) << 32);
  return {int64_t(time_tag)*fClockCycle - fDelayPerCh[ch] - fPreTrigPerCh[ch]*2, // factor of 2 is special here, see CAEN docs
          words,
          (sv[2]>>16)&0x3FFF,
          sv.substr(3, words-3)};
}
