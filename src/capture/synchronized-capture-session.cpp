#include "capture/synchronized-capture-session.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

namespace obs_sync_replay {

namespace {

struct StreamState final {
    std::string name;
    PacketStreamConfig config;
    std::map<uint64_t, OwnedCapturedEncodedPacket> packets;
    size_t retained_bytes = 0;
    size_t peak_retained_bytes = 0;
    uint64_t max_source_cts = 0;
    bool has_source_cts = false;
};

size_t PacketBytes(const OwnedCapturedEncodedPacket& packet) {
    return packet ? packet->packet.payload.size() : 0;
}

void EvictToCapacity(StreamState& stream, const size_t capacity_bytes, uint64_t* evicted_packets) {
    while (stream.retained_bytes > capacity_bytes && !stream.packets.empty()) {
        auto oldest = stream.packets.begin();
        stream.retained_bytes -= PacketBytes(oldest->second);
        stream.packets.erase(oldest);
        if (evicted_packets) {
            ++*evicted_packets;
        }
    }

    // A replay snapshot may start only at a keyframe. Discard a partial GOP
    // exposed by capacity eviction so the ring's oldest usable packet remains
    // decodable without retaining an unbounded pre-keyframe tail.
    while (!stream.packets.empty() && !stream.packets.begin()->second->packet.keyframe) {
        auto oldest = stream.packets.begin();
        stream.retained_bytes -= PacketBytes(oldest->second);
        stream.packets.erase(oldest);
        if (evicted_packets) {
            ++*evicted_packets;
        }
    }
}

bool ContainsAll(const std::vector<StreamState>& streams, const std::vector<CaptureStreamId>& stream_ids,
                 const uint64_t cts) {
    return std::all_of(stream_ids.begin(), stream_ids.end(), [&streams, cts](const CaptureStreamId stream_id) {
        return streams[stream_id].packets.find(cts) != streams[stream_id].packets.end();
    });
}

bool AllKeyframes(const std::vector<StreamState>& streams, const std::vector<CaptureStreamId>& stream_ids,
                  const uint64_t cts) {
    return std::all_of(stream_ids.begin(), stream_ids.end(), [&streams, cts](const CaptureStreamId stream_id) {
        const auto packet = streams[stream_id].packets.find(cts);
        return packet != streams[stream_id].packets.end() && packet->second->packet.keyframe;
    });
}

} // namespace

struct SynchronizedCaptureSession::State final {
    explicit State(const SynchronizedCaptureConfig& value) : config(value) {}

    SynchronizedCaptureConfig config;
    std::vector<StreamState> streams;
    std::vector<CapturePacketConsumer*> consumers;
    bool running = false;
    size_t retained_bytes = 0;
    size_t peak_retained_bytes = 0;
    uint64_t evicted_packet_count = 0;
    uint64_t duplicate_packet_count = 0;
    uint64_t rejected_packet_count = 0;
    bool replay_retention_enabled = true;
    mutable std::mutex mutex;
};

SynchronizedCaptureSession::SynchronizedCaptureSession(SynchronizedCaptureConfig config)
    : state_(std::make_unique<State>(config)) {}

SynchronizedCaptureSession::~SynchronizedCaptureSession() {
    Stop();
}

bool SynchronizedCaptureSession::RegisterStream(const CaptureStreamId stream_id, std::string name,
                                                PacketStreamConfig config) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->running || name.empty() || config.width == 0 || config.height == 0) {
        return false;
    }
    if (std::any_of(state_->streams.begin(), state_->streams.end(),
                    [&name](const StreamState& stream) { return stream.name == name; })) {
        return false;
    }
    // Stream IDs are the public identity used by callbacks; duplicate IDs are
    // rejected even before packets exist.
    if (state_->streams.size() > 0) {
        // The ID is stored by position in this bounded implementation. Keep the
        // registration contract explicit by requiring dense IDs 0..N-1.
        if (stream_id != state_->streams.size()) {
            return false;
        }
    } else if (stream_id != 0) {
        return false;
    }
    state_->streams.push_back(StreamState{std::move(name), std::move(config)});
    return true;
}

bool SynchronizedCaptureSession::Subscribe(CapturePacketConsumer* consumer) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!consumer || std::find(state_->consumers.begin(), state_->consumers.end(), consumer) != state_->consumers.end()) {
        return false;
    }
    state_->consumers.push_back(consumer);
    return true;
}

bool SynchronizedCaptureSession::Unsubscribe(CapturePacketConsumer* consumer) {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    const auto it = std::find(state_->consumers.begin(), state_->consumers.end(), consumer);
    if (it == state_->consumers.end()) {
        return false;
    }
    state_->consumers.erase(it);
    return true;
}

bool SynchronizedCaptureSession::Start() {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->running || state_->streams.size() < 1) {
        return false;
    }
    state_->running = true;
    return true;
}

bool SynchronizedCaptureSession::Ingest(const CaptureStreamId stream_id, EncodedPacket packet,
                                        std::vector<uint8_t> codec_extra_data) {
    std::vector<CapturePacketConsumer*> consumers;
    OwnedCapturedEncodedPacket owned_packet;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->running || stream_id >= state_->streams.size() || packet.payload.empty() ||
            packet.timebase_num <= 0 || packet.timebase_den <= 0) {
            ++state_->rejected_packet_count;
            return false;
        }

        StreamState& stream = state_->streams[stream_id];
        if (stream.packets.find(packet.source_cts) != stream.packets.end()) {
            ++state_->duplicate_packet_count;
            return false;
        }
        if (stream.config.timebase_num <= 0 || stream.config.timebase_den <= 0) {
            stream.config.timebase_num = packet.timebase_num;
            stream.config.timebase_den = packet.timebase_den;
        }
        if (!codec_extra_data.empty()) {
            stream.config.extra_data = codec_extra_data;
        }

        auto packet_value = std::make_shared<CapturedEncodedPacket>();
        packet_value->stream_id = stream_id;
        packet_value->packet = std::move(packet);
        packet_value->codec_extra_data = std::move(codec_extra_data);
        owned_packet = std::move(packet_value);
        if (state_->replay_retention_enabled) {
            stream.packets.emplace(owned_packet->packet.source_cts, owned_packet);
            stream.retained_bytes += PacketBytes(owned_packet);
            stream.peak_retained_bytes = std::max(stream.peak_retained_bytes, stream.retained_bytes);
            stream.max_source_cts = std::max(stream.max_source_cts, owned_packet->packet.source_cts);
            stream.has_source_cts = true;
            state_->retained_bytes += PacketBytes(owned_packet);
            state_->peak_retained_bytes = std::max(state_->peak_retained_bytes, state_->retained_bytes);
            EvictToCapacity(stream, state_->config.ring_capacity_bytes, &state_->evicted_packet_count);
            state_->retained_bytes = 0;
            for (const StreamState& current : state_->streams) {
                state_->retained_bytes += current.retained_bytes;
            }
        }
        consumers = state_->consumers;
    }

    // Snapshot the consumer list and release the session lock before fan-out.
    // Replay saving uses retained shared packets and never runs on this path.
    for (CapturePacketConsumer* consumer : consumers) {
        if (consumer) {
            consumer->OnPacket(owned_packet);
        }
    }
    return true;
}

void SynchronizedCaptureSession::SetReplayRetentionEnabled(const bool enabled) noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->replay_retention_enabled = enabled;
    if (enabled) {
        return;
    }
    for (StreamState& stream : state_->streams) {
        stream.packets.clear();
        stream.retained_bytes = 0;
        stream.max_source_cts = 0;
        stream.has_source_cts = false;
    }
    state_->retained_bytes = 0;
}

void SynchronizedCaptureSession::Stop() noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->running = false;
}

bool SynchronizedCaptureSession::running() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->running;
}

size_t SynchronizedCaptureSession::stream_count() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->streams.size();
}

std::optional<uint64_t> SynchronizedCaptureSession::common_watermark_cts() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->streams.empty() ||
        std::any_of(state_->streams.begin(), state_->streams.end(),
                    [](const StreamState& stream) { return !stream.has_source_cts; })) {
        return std::nullopt;
    }
    uint64_t watermark = std::numeric_limits<uint64_t>::max();
    for (const StreamState& stream : state_->streams) {
        watermark = std::min(watermark, stream.max_source_cts);
    }
    return watermark;
}

std::optional<ReplaySnapshot> SynchronizedCaptureSession::SnapshotCommonRange(const uint64_t duration_ns) const {
    std::vector<CaptureStreamId> stream_ids;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        stream_ids.reserve(state_->streams.size());
        for (CaptureStreamId stream_id = 0; stream_id < state_->streams.size(); ++stream_id) {
            stream_ids.push_back(stream_id);
        }
    }
    return SnapshotCommonRangeDetailed(stream_ids, duration_ns).snapshot;
}

std::optional<ReplaySnapshot> SynchronizedCaptureSession::SnapshotCommonRange(
    const std::vector<CaptureStreamId>& stream_ids, const uint64_t duration_ns) const {
    return SnapshotCommonRangeDetailed(stream_ids, duration_ns).snapshot;
}

ReplaySnapshotAttempt SynchronizedCaptureSession::SnapshotCommonRangeDetailed(
    const std::vector<CaptureStreamId>& stream_ids, const uint64_t duration_ns) const {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->replay_retention_enabled) {
        return {std::nullopt, "replay-retention-disabled"};
    }
    if (stream_ids.empty()) {
        return {std::nullopt, "no-replay-streams"};
    }
    if (duration_ns == 0) {
        return {std::nullopt, "invalid-duration"};
    }
    for (size_t index = 0; index < stream_ids.size(); ++index) {
        const CaptureStreamId stream_id = stream_ids[index];
        if (stream_id >= state_->streams.size() ||
            std::find(stream_ids.begin(), stream_ids.begin() + static_cast<std::ptrdiff_t>(index), stream_id) !=
                stream_ids.begin() + static_cast<std::ptrdiff_t>(index) ||
            state_->streams[stream_id].packets.empty()) {
            return {std::nullopt, "insufficient-history"};
        }
    }

    uint64_t latest_first = 0;
    uint64_t earliest_last = std::numeric_limits<uint64_t>::max();
    for (const CaptureStreamId stream_id : stream_ids) {
        const StreamState& stream = state_->streams[stream_id];
        latest_first = std::max(latest_first, stream.packets.begin()->first);
        earliest_last = std::min(earliest_last, stream.packets.rbegin()->first);
    }
    if (latest_first > earliest_last || earliest_last - latest_first < duration_ns) {
        return {std::nullopt, "insufficient-history"};
    }

    std::optional<uint64_t> start_cts;
    for (const auto& [cts, packet] : state_->streams[stream_ids.front()].packets) {
        if (cts < latest_first || cts > earliest_last || !AllKeyframes(state_->streams, stream_ids, cts)) {
            continue;
        }
        const uint64_t target = earliest_last > duration_ns ? earliest_last - duration_ns : latest_first;
        if (cts <= target) {
            start_cts = cts;
        }
    }
    if (!start_cts) {
        for (const auto& [cts, packet] : state_->streams[stream_ids.front()].packets) {
            if (cts >= latest_first && cts <= earliest_last && AllKeyframes(state_->streams, stream_ids, cts)) {
                start_cts = cts;
                break;
            }
        }
    }
    if (!start_cts) {
        return {std::nullopt, "no-common-keyframe"};
    }

    uint64_t end_cts = *start_cts;
    for (const auto& [cts, packet] : state_->streams[stream_ids.front()].packets) {
        if (cts < *start_cts || cts > earliest_last) {
            continue;
        }
        if (!ContainsAll(state_->streams, stream_ids, cts)) {
            break;
        }
        end_cts = cts;
    }
    if (end_cts < *start_cts) {
        return {std::nullopt, "no-common-range"};
    }

    ReplaySnapshot snapshot;
    snapshot.range = {*start_cts, end_cts};
    snapshot.packets.resize(stream_ids.size());
    snapshot.stream_names.reserve(stream_ids.size());
    snapshot.stream_configs.reserve(stream_ids.size());
    for (size_t index = 0; index < stream_ids.size(); ++index) {
        const StreamState& stream = state_->streams[stream_ids[index]];
        snapshot.stream_names.push_back(stream.name);
        snapshot.stream_configs.push_back(stream.config);
        for (auto it = stream.packets.lower_bound(*start_cts);
             it != stream.packets.end() && it->first <= end_cts; ++it) {
            snapshot.packets[index].push_back(it->second);
        }
    }
    return {std::move(snapshot), {}};
}

ReplaySnapshotAttempt SynchronizedCaptureSession::SnapshotCommonRangeDetailed(const uint64_t duration_ns) const {
    std::vector<CaptureStreamId> stream_ids;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        stream_ids.reserve(state_->streams.size());
        for (CaptureStreamId stream_id = 0; stream_id < state_->streams.size(); ++stream_id) {
            stream_ids.push_back(stream_id);
        }
    }
    return SnapshotCommonRangeDetailed(stream_ids, duration_ns);
}

SynchronizedCaptureMetrics SynchronizedCaptureSession::metrics() const noexcept {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    return {state_->streams.size(), state_->retained_bytes, state_->peak_retained_bytes,
            state_->evicted_packet_count, state_->duplicate_packet_count, state_->rejected_packet_count};
}

} // namespace obs_sync_replay
