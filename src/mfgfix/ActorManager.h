#pragma once
#include "Settings.h"

#include <array>
#include <mutex>

namespace MfgFix
{
    class BSFaceGenAnimationData;

    class ActorManager
    {
      public:
        static inline void SetSpeed(RE::Actor* a_actor, float a_speed)
        {
            if (!a_actor)
                return;

            if (auto animData = a_actor->GetFaceGenAnimationData()) {
                auto id = reinterpret_cast<uintptr_t>(animData);
                auto formId = a_actor->GetFormID();

                if (a_speed == 0.0) {
                    _mapping.erase(id);
                    _speed.erase(formId);
                } else {
                    _mapping[id] = formId;
                    _speed[formId] = a_speed;
                }

                // every API write goes through here first: the phoneme targets are about to be ours
                std::scoped_lock trackLock(_trackLock);
                if (a_speed == 0.0) {
                    _track.erase(id);
                } else {
                    _track[id].apiWrites = 2;  // this update and the next: the write itself lands after this call
                }
            }
        }

        static inline float GetSpeed(BSFaceGenAnimationData* a_data)
        {
            auto id = reinterpret_cast<uintptr_t>(a_data);
            if (_mapping.count(id)) {
                auto formId = _mapping[id];
                if (_speed.count(formId)) {
                    return _speed[formId];
                }
            }

            return Settings::Get().transition.fDefaultSpeed;
        }

        static constexpr std::uint32_t kPhonemes = 16;

        // Lipsync pass-through. A smooth write leaves ONE speed on the actor, and SmoothUpdate slides every
        // channel toward its target at that speed. A plugin that drives the mouth by writing the phoneme targets
        // every frame (AudioUtil's lipsync) is rate-limited by it: slow enough for a face is too slow for speech.
        // So: a phoneme target that keeps changing WITHOUT a call into our API is being driven from outside, and
        // is shown as written instead of slid. "Keeps changing" = two outside changes within kBurst; the channel
        // then passes through until kHold after the last one. A single outside write (some other plugin setting a
        // value once) is slid exactly as before, and so is everything written through MfgConsoleFunc(Ext).
        // a_weight[i] is how far the shown value moves to the target this update: 0 = not driven (slide as
        // usual), 1 = show the target. A channel that has just become driven eases in over kEase instead of
        // jumping: a line starts from silence, and the mouth would otherwise snap from its resting shape to
        // shut on the line's first frame.
        static inline void TrackPhonemeDrivers(BSFaceGenAnimationData* a_data, const float* a_target, std::uint32_t a_count, float a_timeDelta,
            std::array<float, kPhonemes>& a_weight)
        {
            a_weight.fill(0.0f);
            if (!a_target) {
                return;
            }
            std::scoped_lock trackLock(_trackLock);
            const auto it = _track.find(reinterpret_cast<uintptr_t>(a_data));
            if (it == _track.end()) {
                return;
            }
            auto& track = it->second;
            const bool ours = track.apiWrites > 0;
            if (ours) {
                --track.apiWrites;
            }
            const auto count = std::min<std::uint32_t>(a_count, kPhonemes);
            for (std::uint32_t i = 0; i < count; ++i) {
                track.hold[i] = std::max(0.0f, track.hold[i] - a_timeDelta);
                const bool changed = std::fabs(a_target[i] - track.last[i]) > 0.0001f;
                if (changed && ours) {
                    // a script just set this channel: it slides, even inside the tail of a lipsync line (a face
                    // change landing within kHold of the last lipsync write would otherwise snap). A driver that
                    // is still running takes the channel back two changes later.
                    track.hold[i] = 0.0f;
                    track.sinceChange[i] = kBurst;
                } else if (changed) {
                    if (track.sinceChange[i] < kBurst) {
                        if (track.hold[i] <= 0.0f) {
                            track.ease[i] = kEase;  // just became driven
                        }
                        track.hold[i] = kHold;
                    }
                    track.sinceChange[i] = 0.0f;
                } else {
                    track.sinceChange[i] += a_timeDelta;
                }
                track.last[i] = a_target[i];
                if (track.hold[i] > 0.0f) {
                    a_weight[i] = track.ease[i] > a_timeDelta ? a_timeDelta / track.ease[i] : 1.0f;
                    track.ease[i] = std::max(0.0f, track.ease[i] - a_timeDelta);
                } else {
                    track.ease[i] = 0.0f;
                }
            }
        }

      private:
        static constexpr float kBurst = 0.25f;  // two outside changes this close together = a per-frame driver
        static constexpr float kHold = 0.30f;   // pass-through lasts this long after its last change
        static constexpr float kEase = 0.18f;   // a channel that becomes driven reaches its target over this long

        struct PhonemeTrack
        {
            std::array<float, kPhonemes> last{};         // the targets as of the last update
            std::array<float, kPhonemes> sinceChange{};  // seconds since an outside write last moved the target
            std::array<float, kPhonemes> hold{};         // seconds of pass-through left
            std::array<float, kPhonemes> ease{};         // seconds of ease-in left
            int apiWrites{ 2 };                          // updates left in which a change is our own API's
        };

        static inline std::mutex _trackLock;
        static inline std::unordered_map<std::uintptr_t, PhonemeTrack> _track;
        static inline std::unordered_map<RE::FormID, float> _speed;
        static inline std::unordered_map<std::uintptr_t, RE::FormID> _mapping;
        static inline float _defaultSpeed{ 0.f };
    };
}