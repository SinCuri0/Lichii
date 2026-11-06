// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Service {

inline constexpr auto Nx2MovieClockPause = std::chrono::seconds{1};

inline bool IsNx2VideoFrameLate(u64 pts, u64 frame_us, bool clock_running,
                               std::chrono::steady_clock::time_point start,
                               u64 start_pts, std::chrono::steady_clock::time_point last_take,
                               std::chrono::steady_clock::time_point now) {
    if (!clock_running || now - last_take > Nx2MovieClockPause) {
        return false;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
    const u64 due = start_pts + static_cast<u64>((std::max<s64>)(elapsed, 0));
    return pts + 2 * frame_us < due;
}

template <typename Frame, typename Picture>
void QueueNx2VideoPictures(std::deque<Frame>& decoded, std::vector<Picture>& pictures,
                           u64 input_pts, bool input_end) {
    // Retain decoded pictures until output selection has a real replacement. Dropping the
    // sole late picture can leave an already issued output fence waiting for another input.
    for (auto& picture : pictures) {
        const u64 pts = picture.pts;
        decoded.push_back({std::move(picture), pts, false});
    }
    if (input_end) {
        // Even an empty end-of-stream packet must produce a final output.
        if (decoded.empty()) {
            decoded.push_back({std::nullopt, input_pts, true});
        } else {
            decoded.back().end = true;
        }
    }
}

template <typename Frame>
bool HasReadyNx2VideoFrame(const std::deque<Frame>& decoded, bool image_available) {
    return !decoded.empty() && image_available;
}

template <typename Frame, typename IsLate>
Frame TakeNx2VideoFrame(std::deque<Frame>& decoded, IsLate&& is_late, u64& dropped) {
    while (decoded.size() > 1 && !decoded.front().end && is_late(decoded.front().pts)) {
        decoded.pop_front();
        ++dropped;
    }
    Frame frame = std::move(decoded.front());
    decoded.pop_front();
    return frame;
}

} // namespace Service
