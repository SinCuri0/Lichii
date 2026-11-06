// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_video_frame_queue.h"
#include "core/hle/service/nx2_video_input_queue.h"

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Picture {
    u64 pts;
    std::unique_ptr<int> payload;
};

struct Frame {
    std::optional<Picture> picture;
    u64 pts;
    bool end;
};

std::vector<Picture> Pictures(std::initializer_list<u64> timestamps) {
    std::vector<Picture> pictures;
    for (const u64 pts : timestamps) {
        pictures.push_back({pts, std::make_unique<int>(7)});
    }
    return pictures;
}

bool LateAt100ms(u64 pts) {
    return Service::IsNx2VideoFrameLate(pts, 16'666, true, Clock::time_point{}, 0,
                                       Clock::time_point{}, Clock::time_point{} + 100ms);
}

} // namespace

TEST_CASE("A sole late decoded picture remains eligible for its output image",
          "[nx2_video_retention][late]") {
    Service::Nx2VideoInputQueue inputs;
    inputs.Push({{1}, 16'667, false});
    const auto input = inputs.Pop();
    auto pictures = Pictures({input.pts});
    std::deque<Frame> decoded;
    REQUIRE(LateAt100ms(input.pts));
    Service::QueueNx2VideoPictures(decoded, pictures, input.pts, input.end);

    // This is the production decoder wait predicate's output arm. No further input can wake it.
    REQUIRE(inputs.Empty());
    REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
    CHECK_FALSE(Service::HasReadyNx2VideoFrame(decoded, false));
    u64 dropped{};
    auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
    REQUIRE(frame.picture);
    CHECK(*frame.picture->payload == 7);
    CHECK(frame.pts == input.pts);
    CHECK_FALSE(frame.end);
    CHECK(dropped == 0);
    CHECK_FALSE(Service::HasReadyNx2VideoFrame(decoded, true));
}

TEST_CASE("A compressed packet producing no picture cannot replace a late candidate",
          "[nx2_video_retention][late]") {
    std::deque<Frame> decoded;
    auto pictures = Pictures({16'667});
    Service::QueueNx2VideoPictures(decoded, pictures, 16'667, false);
    Service::Nx2VideoInputQueue inputs;
    inputs.Push({{2}, 33'334, false});
    CHECK_FALSE(Service::HasReadyNx2VideoFrame(decoded, false));
    const auto next = inputs.Pop();
    pictures.clear(); // A valid decoder submission may produce zero pictures.
    Service::QueueNx2VideoPictures(decoded, pictures, next.pts, next.end);
    REQUIRE(inputs.Empty());
    REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
    u64 dropped{};
    auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
    CHECK(frame.pts == 16'667);
    REQUIRE(frame.picture);
    CHECK(dropped == 0);
}

TEST_CASE("Real decoded replacements supersede late pictures but retain the newest",
          "[nx2_video_retention][late]") {
    std::deque<Frame> decoded;
    auto pictures = Pictures({16'667, 33'334, 50'001});
    Service::QueueNx2VideoPictures(decoded, pictures, 50'001, false);
    REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
    u64 dropped{};
    auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
    CHECK(frame.pts == 50'001);
    REQUIRE(frame.picture);
    CHECK(dropped == 2);
    CHECK(decoded.empty());
}

TEST_CASE("Video output retains EOS pictures and synthesizes empty EOS",
          "[nx2_video_retention]") {
    std::deque<Frame> decoded;
    u64 dropped{};
    SECTION("Last decoded picture is EOS even when late") {
        auto pictures = Pictures({16'667, 33'334});
        Service::QueueNx2VideoPictures(decoded, pictures, 33'334, true);
        REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
        auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
        CHECK(frame.end);
        CHECK(frame.pts == 33'334);
        REQUIRE(frame.picture);
        CHECK(dropped == 1);
    }
    SECTION("Empty EOS with no candidate still completes an output") {
        auto pictures = Pictures({});
        Service::QueueNx2VideoPictures(decoded, pictures, 42, true);
        REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
        auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
        CHECK(frame.end);
        CHECK(frame.pts == 42);
        CHECK_FALSE(frame.picture);
        CHECK(dropped == 0);
    }
    SECTION("Empty EOS marks the retained candidate") {
        auto pictures = Pictures({16'667});
        Service::QueueNx2VideoPictures(decoded, pictures, 16'667, false);
        pictures.clear();
        Service::QueueNx2VideoPictures(decoded, pictures, 33'334, true);
        REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
        auto frame = Service::TakeNx2VideoFrame(decoded, LateAt100ms, dropped);
        CHECK(frame.end);
        CHECK(frame.pts == 16'667);
        REQUIRE(frame.picture);
        CHECK(dropped == 0);
    }
}

TEST_CASE("Paused and on-time movie clocks preserve decoded order",
          "[nx2_video_retention]") {
    std::deque<Frame> decoded;
    auto pictures = Pictures({16'667, 33'334});
    Service::QueueNx2VideoPictures(decoded, pictures, 33'334, false);
    Clock::time_point now;
    SECTION("Pause longer than one second") {
        now += 1s + 1us;
    }
    SECTION("On-time frame within the two-frame grace period") {
        now += 49'999us;
    }
    const auto is_late = [now](u64 pts) {
        return Service::IsNx2VideoFrameLate(pts, 16'666, true, Clock::time_point{}, 0,
                                           Clock::time_point{}, now);
    };
    REQUIRE_FALSE(is_late(16'667));
    u64 dropped{};
    for (const u64 pts : {16'667, 33'334}) {
        REQUIRE(Service::HasReadyNx2VideoFrame(decoded, true));
        CHECK(Service::TakeNx2VideoFrame(decoded, is_late, dropped).pts == pts);
    }
    CHECK(dropped == 0);
    CHECK_FALSE(Service::HasReadyNx2VideoFrame(decoded, true));
    CHECK_FALSE(Service::IsNx2VideoFrameLate(0, 16'666, false, Clock::time_point{}, 0,
                                            Clock::time_point{}, Clock::time_point{} + 100ms));
}
