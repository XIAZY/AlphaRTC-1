/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifdef WIN32
#pragma comment(lib, \
                "../../modules/third_party/statcollect/lib/StatCollect.lib")
#pragma comment(lib, "../../modules/third_party/onnxinfer/lib/onnxinfer.lib")
#endif  //  WIN32

#include "modules/remote_bitrate_estimator/remote_estimator_proxy.h"

#include <algorithm>
#include <limits>

#include "api/alphacc_config.h"
#include "modules/rtp_rtcp/source/rtcp_packet/transport_feedback.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/safe_minmax.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {

// Impossible to request feedback older than what can be represented by 15 bits.
const int RemoteEstimatorProxy::kMaxNumberOfPackets = (1 << 15);

// The maximum allowed value for a timestamp in milliseconds. This is lower
// than the numerical limit since we often convert to microseconds.
static constexpr int64_t kMaxTimeMs =
    std::numeric_limits<int64_t>::max() / 1000;

RemoteEstimatorProxy::RemoteEstimatorProxy(
    Clock* clock,
    TransportFeedbackSenderInterface* feedback_sender,
    const WebRtcKeyValueConfig* key_value_config)
    : clock_(clock),
      feedback_sender_(feedback_sender),
      send_config_(key_value_config),
      last_process_time_ms_(-1),
      media_ssrc_(0),
      feedback_packet_count_(0),
      send_interval_ms_(send_config_.default_interval->ms()),
      send_periodic_feedback_(true),
      bwe_sendback_interval_ms_(GetAlphaCCConfig()->bwe_feedback_duration_ms),
      last_bwe_sendback_ms_(clock->TimeInMilliseconds()),
      stats_collect_(GetAlphaCCConfig()->redis_sid.c_str(),
                     StatCollect::SC_TYPE_STRUCT),
      redis_save_interval_ms_(GetAlphaCCConfig()->redis_update_duration_ms),
      last_redis_save_ms_(clock->TimeInMilliseconds()),
      cycles_(-1),
      max_abs_send_time_(0) {
  onnx_infer_ = onnxinfer::CreateONNXInferInterface(
      GetAlphaCCConfig()->onnx_model_path.c_str());
  if (!onnxinfer::IsReady(onnx_infer_)) {
    RTC_LOG(LS_ERROR) << "Failed to create onnx_infer_.";
  }
  RTC_LOG(LS_INFO)
      << "Maximum interval between transport feedback RTCP messages (ms): "
      << send_config_.max_interval->ms();
  auto connect_result = stats_collect_.DBConnect(
      GetAlphaCCConfig()->redis_ip.c_str(), GetAlphaCCConfig()->redis_port);
  if (connect_result != StatCollect::SC_SUCCESS) {
    RTC_LOG(LS_ERROR) << "StatCollect failed.";
  }
}

RemoteEstimatorProxy::~RemoteEstimatorProxy() {
  stats_collect_.DBClose();
  if (onnx_infer_) {
    onnxinfer::DestroyONNXInferInterface(onnx_infer_);
  }
}

void RemoteEstimatorProxy::IncomingPacket(int64_t arrival_time_ms,
                                          size_t payload_size,
                                          const RTPHeader& header) {
  if (!header.extension.hasTransportSequenceNumber) {
    RTC_LOG(LS_WARNING)
        << "RemoteEstimatorProxy: Incoming packet "
           "is missing the transport sequence number extension!";
    return;
  }
  rtc::CritScope cs(&lock_);
  media_ssrc_ = header.ssrc;
  OnPacketArrival(header.extension.transportSequenceNumber, arrival_time_ms,
                  header.extension.feedback_request);

  //--- ONNXInfer: Input the per-packet info to ONNXInfer module ---
  uint32_t send_time_ms =
      GetTtimeFromAbsSendtime(header.extension.absoluteSendTime);

  // lossCound and RTT field for onnxinfer::OnReceived() are set to -1 since
  // no available lossCound and RTT in webrtc
  onnxinfer::OnReceived(onnx_infer_, header.payloadType, header.sequenceNumber,
                        send_time_ms, header.ssrc, header.paddingLength,
                        header.headerLength, arrival_time_ms, payload_size, -1, -1);

  //--- BandWidthControl: Send back bandwidth estimation into to sender ---
  bool time_to_send_bew_message = TimeToSendBweMessage();
  float estimation = 0;
  if (time_to_send_bew_message) {
    BweMessage bwe;
    estimation = onnxinfer::GetBweEstimate(onnx_infer_);
    bwe.pacing_rate = bwe.padding_rate = bwe.target_rate = estimation;
    bwe.timestamp_ms = clock_->TimeInMilliseconds();
    SendbackBweEstimation(bwe);
  }

  // -- StatCollect: Collect packet-related info into redis ------

  double pacing_rate =
      time_to_send_bew_message ? estimation : SC_PACER_PACING_RATE_EMPTY;
  double padding_rate =
      time_to_send_bew_message ? estimation : SC_PACER_PADDING_RATE_EMPTY;

  // Save per-packet info locally on receiving
  stats_collect_.StatsCollect(pacing_rate, padding_rate, header.payloadType,
                              header.sequenceNumber, send_time_ms, header.ssrc,
                              header.paddingLength, header.headerLength,
                              arrival_time_ms, payload_size, 0);
  // Periodically push to remote redis service
  if (TimeToSaveIntoRedis()) {
    SaveIntoRedis();
  }
}

bool RemoteEstimatorProxy::LatestEstimate(std::vector<unsigned int>* ssrcs,
                                          unsigned int* bitrate_bps) const {
  return false;
}

int64_t RemoteEstimatorProxy::TimeUntilNextProcess() {
  rtc::CritScope cs(&lock_);
  if (!send_periodic_feedback_) {
    // Wait a day until next process.
    return 24 * 60 * 60 * 1000;
  } else if (last_process_time_ms_ != -1) {
    int64_t now = clock_->TimeInMilliseconds();
    if (now - last_process_time_ms_ < send_interval_ms_)
      return last_process_time_ms_ + send_interval_ms_ - now;
  }
  return 0;
}

void RemoteEstimatorProxy::Process() {
  rtc::CritScope cs(&lock_);
  if (!send_periodic_feedback_) {
    return;
  }
  last_process_time_ms_ = clock_->TimeInMilliseconds();

  SendPeriodicFeedbacks();
}

void RemoteEstimatorProxy::OnBitrateChanged(int bitrate_bps) {
  // TwccReportSize = Ipv4(20B) + UDP(8B) + SRTP(10B) +
  // AverageTwccReport(30B)
  // TwccReport size at 50ms interval is 24 byte.
  // TwccReport size at 250ms interval is 36 byte.
  // AverageTwccReport = (TwccReport(50ms) + TwccReport(250ms)) / 2
  constexpr int kTwccReportSize = 20 + 8 + 10 + 30;
  const double kMinTwccRate =
      kTwccReportSize * 8.0 * 1000.0 / send_config_.max_interval->ms();
  const double kMaxTwccRate =
      kTwccReportSize * 8.0 * 1000.0 / send_config_.min_interval->ms();

  // Let TWCC reports occupy 5% of total bandwidth.
  rtc::CritScope cs(&lock_);
  send_interval_ms_ = static_cast<int>(
      0.5 + kTwccReportSize * 8.0 * 1000.0 /
                rtc::SafeClamp(send_config_.bandwidth_fraction * bitrate_bps,
                               kMinTwccRate, kMaxTwccRate));
}

void RemoteEstimatorProxy::SetSendPeriodicFeedback(
    bool send_periodic_feedback) {
  rtc::CritScope cs(&lock_);
  send_periodic_feedback_ = send_periodic_feedback;
}

void RemoteEstimatorProxy::OnPacketArrival(
    uint16_t sequence_number,
    int64_t arrival_time,
    absl::optional<FeedbackRequest> feedback_request) {
  if (arrival_time < 0 || arrival_time > kMaxTimeMs) {
    RTC_LOG(LS_WARNING) << "Arrival time out of bounds: " << arrival_time;
    return;
  }

  int64_t seq = unwrapper_.Unwrap(sequence_number);

  if (send_periodic_feedback_) {
    if (periodic_window_start_seq_ &&
        packet_arrival_times_.lower_bound(*periodic_window_start_seq_) ==
            packet_arrival_times_.end()) {
      // Start new feedback packet, cull old packets.
      for (auto it = packet_arrival_times_.begin();
           it != packet_arrival_times_.end() && it->first < seq &&
           arrival_time - it->second >= send_config_.back_window->ms();) {
        it = packet_arrival_times_.erase(it);
      }
    }
    if (!periodic_window_start_seq_ || seq < *periodic_window_start_seq_) {
      periodic_window_start_seq_ = seq;
    }
  }

  // We are only interested in the first time a packet is received.
  if (packet_arrival_times_.find(seq) != packet_arrival_times_.end())
    return;

  packet_arrival_times_[seq] = arrival_time;

  // Limit the range of sequence numbers to send feedback for.
  auto first_arrival_time_to_keep = packet_arrival_times_.lower_bound(
      packet_arrival_times_.rbegin()->first - kMaxNumberOfPackets);
  if (first_arrival_time_to_keep != packet_arrival_times_.begin()) {
    packet_arrival_times_.erase(packet_arrival_times_.begin(),
                                first_arrival_time_to_keep);
    if (send_periodic_feedback_) {
      // |packet_arrival_times_| cannot be empty since we just added one element
      // and the last element is not deleted.
      RTC_DCHECK(!packet_arrival_times_.empty());
      periodic_window_start_seq_ = packet_arrival_times_.begin()->first;
    }
  }

  if (feedback_request) {
    // Send feedback packet immediately.
    SendFeedbackOnRequest(seq, *feedback_request);
  }
}

bool RemoteEstimatorProxy::TimeToSendBweMessage() {
  int64_t time_now = clock_->TimeInMilliseconds();
  if (time_now - bwe_sendback_interval_ms_ > last_bwe_sendback_ms_) {
    last_bwe_sendback_ms_ = time_now;
    return true;
  }
  return false;
}

void RemoteEstimatorProxy::SendPeriodicFeedbacks() {
  // |periodic_window_start_seq_| is the first sequence number to include in the
  // current feedback packet. Some older may still be in the map, in case a
  // reordering happens and we need to retransmit them.
  if (!periodic_window_start_seq_)
    return;

  for (auto begin_iterator =
           packet_arrival_times_.lower_bound(*periodic_window_start_seq_);
       begin_iterator != packet_arrival_times_.cend();
       begin_iterator =
           packet_arrival_times_.lower_bound(*periodic_window_start_seq_)) {
    rtcp::TransportFeedback feedback_packet;
    periodic_window_start_seq_ = BuildFeedbackPacket(
        feedback_packet_count_++, media_ssrc_, *periodic_window_start_seq_,
        begin_iterator, packet_arrival_times_.cend(), &feedback_packet);

    RTC_DCHECK(feedback_sender_ != nullptr);
    feedback_sender_->SendTransportFeedback(&feedback_packet);
    // Note: Don't erase items from packet_arrival_times_ after sending, in case
    // they need to be re-sent after a reordering. Removal will be handled
    // by OnPacketArrival once packets are too old.
  }
}

void RemoteEstimatorProxy::SendFeedbackOnRequest(
    int64_t sequence_number,
    const FeedbackRequest& feedback_request) {
  if (feedback_request.sequence_count == 0) {
    return;
  }
  rtcp::TransportFeedback feedback_packet(feedback_request.include_timestamps);

  int64_t first_sequence_number =
      sequence_number - feedback_request.sequence_count + 1;
  auto begin_iterator =
      packet_arrival_times_.lower_bound(first_sequence_number);
  auto end_iterator = packet_arrival_times_.upper_bound(sequence_number);

  BuildFeedbackPacket(feedback_packet_count_++, media_ssrc_,
                      first_sequence_number, begin_iterator, end_iterator,
                      &feedback_packet);

  // Clear up to the first packet that is included in this feedback packet.
  packet_arrival_times_.erase(packet_arrival_times_.begin(), begin_iterator);

  RTC_DCHECK(feedback_sender_ != nullptr);
  feedback_sender_->SendTransportFeedback(&feedback_packet);
}

void RemoteEstimatorProxy::SendbackBweEstimation(const BweMessage& bwe) {
  rtcp::App app_packet;

  app_packet.SetSubType(kAppPacketSubType);
  app_packet.SetName(kAppPacketName);

  app_packet.SetData(reinterpret_cast<const uint8_t*>(&bwe), sizeof(bwe));
  feedback_sender_->SendApplicationPacket(&app_packet);
}

int64_t RemoteEstimatorProxy::BuildFeedbackPacket(
    uint8_t feedback_packet_count,
    uint32_t media_ssrc,
    int64_t base_sequence_number,
    std::map<int64_t, int64_t>::const_iterator begin_iterator,
    std::map<int64_t, int64_t>::const_iterator end_iterator,
    rtcp::TransportFeedback* feedback_packet) {
  RTC_DCHECK(begin_iterator != end_iterator);

  // TODO(sprang): Measure receive times in microseconds and remove the
  // conversions below.
  feedback_packet->SetMediaSsrc(media_ssrc);
  // Base sequence number is the expected first sequence number. This is known,
  // but we might not have actually received it, so the base time shall be the
  // time of the first received packet in the feedback.
  feedback_packet->SetBase(static_cast<uint16_t>(base_sequence_number & 0xFFFF),
                           begin_iterator->second * 1000);
  feedback_packet->SetFeedbackSequenceNumber(feedback_packet_count);
  int64_t next_sequence_number = base_sequence_number;
  for (auto it = begin_iterator; it != end_iterator; ++it) {
    if (!feedback_packet->AddReceivedPacket(
            static_cast<uint16_t>(it->first & 0xFFFF), it->second * 1000)) {
      // If we can't even add the first seq to the feedback packet, we won't be
      // able to build it at all.
      RTC_CHECK(begin_iterator != it);

      // Could not add timestamp, feedback packet might be full. Return and
      // try again with a fresh packet.
      break;
    }
    next_sequence_number = it->first + 1;
  }
  return next_sequence_number;
}

uint32_t RemoteEstimatorProxy::GetTtimeFromAbsSendtime(
    uint32_t absoluteSendTime) {
  if (cycles_ == -1) {
    // Initalize
    max_abs_send_time_ = absoluteSendTime;
    cycles_ = 0;
  }
  // Abs sender time is 24 bit 6.18 fixed point. Shift by 8 to normalize to
  // 32 bits (unsigned). Calculate the difference between this packet's
  // send time and the maximum observed. Cast to signed 32-bit to get the
  // desired wrap-around behavior.
  if (static_cast<int32_t>((absoluteSendTime << 8) -
                           (max_abs_send_time_ << 8)) >= 0) {
    // The difference is non-negative, meaning that this packet is newer
    // than the previously observed maximum absolute send time.
    if (absoluteSendTime < max_abs_send_time_) {
      // Wrap detected.
      cycles_++;
    }
    max_abs_send_time_ = absoluteSendTime;
  }
  // Abs sender time is 24 bit 6.18 fixed point. Divide by 2^18 to convert
  // to floating point representation.
  double send_time_seconds =
      static_cast<double>(absoluteSendTime) / 262144 + 64.0 * cycles_;
  uint32_t send_time_ms =
      static_cast<uint32_t>(std::round(send_time_seconds * 1000));
  return send_time_ms;
}
void RemoteEstimatorProxy::SaveIntoRedis(int retry_times) {
  StatCollect::SCResult res = stats_collect_.DBSave();
  if (res == StatCollect::SC_SUCCESS) {
    return;
  }

  retry_times--;
  if (retry_times < 0) {  // Do not try to save any more
    RTC_LOG(LS_ERROR) << "Can not save rtp packet info into redis.";
    return;
  } else {
    if (res == StatCollect::SC_CONNECT_ERROR) {
      const char* redis_ip = GetAlphaCCConfig()->redis_ip.c_str();
      int redis_port = GetAlphaCCConfig()->redis_port;
      stats_collect_.DBConnect(redis_ip, redis_port);
    }
    if (res == StatCollect::SC_SESSION_ERROR ||
        res == StatCollect::SC_COLLECT_TYPE_ERROR) {
      const char* sessionID = GetAlphaCCConfig()->redis_sid.c_str();
      StatCollect::SCType collectType = StatCollect::SC_TYPE_STRUCT;
      stats_collect_.SetStatsConfig(sessionID, collectType);
    }
    SaveIntoRedis(retry_times);
  }
}

bool RemoteEstimatorProxy::TimeToSaveIntoRedis() {
  int64_t time_now = clock_->TimeInMilliseconds();
  if (time_now - redis_save_interval_ms_ > last_redis_save_ms_) {
    last_redis_save_ms_ = time_now;
    return true;
  }
  return false;
}

}  // namespace webrtc
