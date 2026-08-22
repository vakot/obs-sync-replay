#include "recording/synchronized-recording-consumer.hpp"

extern "C" {
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <chrono>
#include <utility>

namespace obs_sync_replay {

namespace {

uint64_t WallClockNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

SynchronizedRecordingConsumer::SynchronizedRecordingConsumer(std::vector<PacketStreamConfig> stream_configs,
                                                             std::vector<std::filesystem::path> paths,
                                                             std::vector<CaptureStreamId> stream_ids)
    : stream_configs_(std::move(stream_configs)), paths_(std::move(paths)), stream_ids_(std::move(stream_ids)),
      pending_(stream_configs_.size()),
      pending_audio_(stream_configs_.empty() ? 0 : stream_configs_.front().audio_streams.size()),
      writers_(stream_configs_.size()), dts_origins_(stream_configs_.size(), 0),
      audio_dts_origins_(pending_audio_.size(), 0) {}

SynchronizedRecordingConsumer::~SynchronizedRecordingConsumer() {
    Stop();
}

bool SynchronizedRecordingConsumer::Start() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (running_ || worker_.joinable() || stream_configs_.empty() || stream_configs_.size() != paths_.size() ||
        (!stream_ids_.empty() && stream_ids_.size() != stream_configs_.size())) {
        return false;
    }
    if (stream_ids_.empty()) {
        stream_ids_.resize(stream_configs_.size());
        for (size_t index = 0; index < stream_ids_.size(); ++index) {
            stream_ids_[index] = static_cast<CaptureStreamId>(index);
        }
    }
    queue_.clear();
    for (auto& stream : pending_) {
        stream.clear();
    }
    for (auto& track : pending_audio_) {
        track.clear();
    }
    for (auto& writer : writers_) {
        writer.reset();
    }
    range_ = {};
    std::fill(dts_origins_.begin(), dts_origins_.end(), 0);
    std::fill(audio_dts_origins_.begin(), audio_dts_origins_.end(), 0);
    running_ = true;
    stop_requested_ = false;
    failed_ = false;
    started_ = false;
    packet_count_ = 0;
    start_wall_ns_ = WallClockNs();
    result_.reset();
    worker_ = std::thread(&SynchronizedRecordingConsumer::Run, this);
    return true;
}

void SynchronizedRecordingConsumer::OnPacket(OwnedCapturedEncodedPacket packet) {
    if (!packet) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto local_index = LocalStreamIndex(packet->stream_id);
        const bool valid_audio = packet->packet.kind == EncodedPacketKind::Audio &&
                                 packet->packet.audio_track_index < pending_audio_.size();
        if (!running_ || stop_requested_ || failed_ || (!local_index && !valid_audio)) {
            return;
        }
        if (queue_.size() >= kMaxQueuePackets) {
            failed_ = true;
            result_ = SynchronizedRecordingConsumerResult{};
            result_->paths = paths_;
            result_->error = "recording-queue-overflow";
            condition_.notify_one();
            return;
        }
        queue_.push_back(std::move(packet));
    }
    condition_.notify_one();
}

void SynchronizedRecordingConsumer::Stop() noexcept {
    std::thread worker;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !worker_.joinable()) {
            return;
        }
        stop_requested_ = true;
        worker = std::move(worker_);
    }
    condition_.notify_one();
    if (worker.joinable()) {
        worker.join();
    }
}

std::optional<SynchronizedRecordingConsumerResult> SynchronizedRecordingConsumer::result() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

void SynchronizedRecordingConsumer::Run() {
    for (;;) {
        OwnedCapturedEncodedPacket packet;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stop_requested_ || failed_ || !queue_.empty(); });
            if (failed_ || (queue_.empty() && stop_requested_)) {
                break;
            }
            packet = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto local_index = packet ? LocalStreamIndex(packet->stream_id) : std::nullopt;
        if (packet && packet->packet.kind == EncodedPacketKind::Audio &&
            packet->packet.audio_track_index < pending_audio_.size()) {
            auto& track = pending_audio_[packet->packet.audio_track_index];
            if (track.find(packet->packet.source_cts) == track.end()) {
                if (stream_configs_.front().audio_streams[packet->packet.audio_track_index].extra_data.empty() &&
                    !packet->codec_extra_data.empty()) {
                    for (PacketStreamConfig& config : stream_configs_) {
                        config.audio_streams[packet->packet.audio_track_index].extra_data = packet->codec_extra_data;
                    }
                }
                track.emplace(packet->packet.source_cts, std::move(packet));
            }
        } else if (packet && local_index) {
            auto& stream = pending_[*local_index];
            if (stream.find(packet->packet.source_cts) == stream.end()) {
                if (stream_configs_[*local_index].extra_data.empty() && !packet->codec_extra_data.empty()) {
                    stream_configs_[*local_index].extra_data = packet->codec_extra_data;
                }
                stream.emplace(packet->packet.source_cts, std::move(packet));
            }
        }
        size_t pending_packet_count = 0;
        for (const auto& stream : pending_) {
            pending_packet_count += stream.size();
        }
        for (const auto& track : pending_audio_) {
            pending_packet_count += track.size();
        }
        if (pending_packet_count > kMaxQueuePackets) {
            Fail("recording-pending-overflow");
            break;
        }
        if (!started_ && !EstablishCommonStart()) {
            // Startup remains pending until all streams expose a common keyframe.
        }
        if (started_ && !FlushCommonPrefix()) {
            break;
        }
    }

    bool failed = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        failed = failed_;
    }
    if (!failed) {
        if (!started_ && !EstablishCommonStart()) {
            Fail("no-common-recording-start-keyframe");
        } else if (started_ && !FlushCommonPrefix()) {
            Fail("no-common-recording-prefix");
        } else if (started_ && std::any_of(pending_.begin(), pending_.end(),
                                          [](const auto& stream) { return !stream.empty(); })) {
            Fail("recording-unresolved-common-prefix");
        }
    }
    Finalize();
    const std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
}

bool SynchronizedRecordingConsumer::EstablishCommonStart() {
    if (started_ || pending_.empty()) {
        return started_;
    }
    for (const auto& [cts, packet] : pending_.front()) {
        if (!packet || !packet->packet.keyframe) {
            continue;
        }
        bool common = true;
        for (size_t index = 0; index < pending_.size(); ++index) {
            const auto it = pending_[index].find(cts);
            common = common && it != pending_[index].end() && it->second->packet.keyframe;
        }
        if (!common) {
            continue;
        }
        range_.start_cts = cts;
        for (auto& stream : pending_) {
            stream.erase(stream.begin(), stream.lower_bound(cts));
        }
        for (size_t index = 0; index < stream_configs_.size(); ++index) {
            writers_[index] = std::make_unique<MkvPacketWriter>();
            if (!writers_[index]->Open(paths_[index].string(), stream_configs_[index])) {
                Fail("recording-open-failed:" + writers_[index]->error());
                return false;
            }
            const auto first = pending_[index].find(cts);
            if (first == pending_[index].end()) {
                Fail("recording-start-packet-missing");
                return false;
            }
            dts_origins_[index] = first->second->packet.dts;
        }
        for (size_t track = 0; track < pending_audio_.size(); ++track) {
            const auto first_audio = pending_audio_[track].lower_bound(cts);
            if (first_audio != pending_audio_[track].end()) {
                audio_dts_origins_[track] = first_audio->second->packet.dts;
            }
        }
        started_ = true;
        return true;
    }
    return false;
}

bool SynchronizedRecordingConsumer::FlushCommonPrefix() {
    if (!started_) {
        return false;
    }
    for (;;) {
        if (pending_.empty() || pending_.front().empty()) {
            return true;
        }
        const uint64_t cts = pending_.front().begin()->first;
        for (const auto& stream : pending_) {
            if (stream.find(cts) == stream.end()) {
                return true;
            }
        }
        for (size_t index = 0; index < pending_.size(); ++index) {
            const auto packet = pending_[index].find(cts)->second;
            EncodedPacket output = packet->packet;
            output.pts = av_rescale_q(static_cast<int64_t>(cts - range_.start_cts), AVRational{1, 1'000'000'000},
                                      AVRational{output.timebase_num, output.timebase_den});
            output.dts -= dts_origins_[index];
            if (!writers_[index]->Write(output)) {
                Fail("recording-write-failed:" + writers_[index]->error());
                return false;
            }
            pending_[index].erase(cts);
        }
        for (size_t track = 0; track < pending_audio_.size(); ++track) {
            auto& audio = pending_audio_[track];
            while (!audio.empty() && audio.begin()->first <= cts) {
                const auto packet = audio.begin()->second;
                for (size_t index = 0; index < writers_.size(); ++index) {
                    EncodedPacket output = packet->packet;
                    output.pts = av_rescale_q(static_cast<int64_t>(output.source_cts - range_.start_cts),
                                              AVRational{1, 1'000'000'000},
                                              AVRational{output.timebase_num, output.timebase_den});
                    output.dts -= audio_dts_origins_[track];
                    if (!writers_[index]->Write(output)) {
                        Fail("recording-audio-write-failed:" + writers_[index]->error());
                        return false;
                    }
                }
                audio.erase(audio.begin());
            }
        }
        range_.end_cts = cts;
        ++packet_count_;
    }
}

void SynchronizedRecordingConsumer::Fail(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!failed_) {
        failed_ = true;
        result_ = SynchronizedRecordingConsumerResult{};
        result_->error = std::move(error);
    }
}

void SynchronizedRecordingConsumer::Finalize() {
    SynchronizedRecordingConsumerResult result;
    result.paths = paths_;
    result.range = range_;
    result.packet_count = packet_count_;
    result.streams.resize(writers_.size());
    for (size_t index = 0; index < writers_.size(); ++index) {
        if (!writers_[index]) {
            continue;
        }
        if (failed_) {
            writers_[index]->Abort();
            continue;
        }
        result.streams[index] = writers_[index]->Finalize();
        if (!result.streams[index].success || result.streams[index].first_source_cts != range_.start_cts ||
            result.streams[index].last_source_cts != range_.end_cts) {
            result.error = "recording-range-mismatch:" + result.streams[index].error;
            failed_ = true;
        }
    }
    result.success = !failed_ && started_ && range_.end_cts >= range_.start_cts;
    result.wall_time_ms = (WallClockNs() - start_wall_ns_) / 1'000'000;
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!result_ || result_->error.empty()) {
        result_ = std::move(result);
    }
}

std::optional<size_t> SynchronizedRecordingConsumer::LocalStreamIndex(
    const CaptureStreamId stream_id) const noexcept {
    const auto it = std::find(stream_ids_.begin(), stream_ids_.end(), stream_id);
    if (it == stream_ids_.end()) {
        return std::nullopt;
    }
    return static_cast<size_t>(std::distance(stream_ids_.begin(), it));
}

} // namespace obs_sync_replay
