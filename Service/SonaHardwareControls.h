// Hardware volume access, used only on the driver's serial configuration queue.
#pragma once
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <cmath>
#include <vector>

struct SonaHardwareControls {
    AudioObjectID device = kAudioObjectUnknown;
    std::vector<AudioObjectPropertyElement> volumeElements, muteElements;

    static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                               AudioObjectPropertyElement element) {
        return {selector, kAudioObjectPropertyScopeOutput, element};
    }

    template<typename T>
    bool read(AudioObjectPropertySelector selector, AudioObjectPropertyElement element, T& value) const {
        auto addr = address(selector, element);
        UInt32 size = sizeof(value);
        return AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &value) == noErr;
    }

    template<typename T>
    bool write(AudioObjectPropertySelector selector, AudioObjectPropertyElement element, T value) const {
        auto addr = address(selector, element);
        return AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(value), &value) == noErr;
    }

    bool writable(AudioObjectPropertySelector selector, AudioObjectPropertyElement element) const {
        auto addr = address(selector, element);
        Boolean settable = false;
        return AudioObjectHasProperty(device, &addr) &&
            AudioObjectIsPropertySettable(device, &addr, &settable) == noErr && settable;
    }

    void bind(AudioObjectID id) {
        device = id;
        volumeElements.clear();
        muteElements.clear();
        if (device == kAudioObjectUnknown) return;
        // Most devices expose a main control. Some USB devices expose channel controls only.
        UInt32 channels = 0;
        auto addr = address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyElementMain);
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) == noErr && size >= sizeof(AudioBufferList)) {
            std::vector<UInt8> storage(size);
            auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
            if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) == noErr)
                for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
        }
        auto discover = [&](AudioObjectPropertySelector selector, auto& elements) {
            if (writable(selector, kAudioObjectPropertyElementMain)) {
                elements.push_back(kAudioObjectPropertyElementMain);
            } else {
                for (UInt32 ch = 1; ch <= channels; ++ch)
                    if (writable(selector, ch)) elements.push_back(ch);
            }
        };
        discover(kAudioDevicePropertyVolumeScalar, volumeElements);
        discover(kAudioDevicePropertyMute, muteElements);
    }

    bool volume(Float32& value) const {
        if (volumeElements.empty()) return false;
        value = 0;
        for (auto element : volumeElements) {
            Float32 v;
            if (!read(kAudioDevicePropertyVolumeScalar, element, v) || !std::isfinite(v)) return false;
            value = std::max(value, v);
        }
        return true;
    }

    bool muted(bool& value) const {
        if (muteElements.empty()) return false;
        value = true;
        for (auto element : muteElements) {
            UInt32 m;
            if (!read(kAudioDevicePropertyMute, element, m)) return false;
            value = value && m != 0;
        }
        return true;
    }

    bool setVolume(Float32 value) const {
        Float32 previous;
        if (!volume(previous)) return false;
        bool ok = true;
        for (auto element : volumeElements) {
            Float32 channel;
            if (!read(kAudioDevicePropertyVolumeScalar, element, channel)) { ok = false; continue; }
            // Preserve channel balance. From all-zero, restore equal levels.
            Float32 next = previous > 0 ? channel / previous * value : value;
            if (!write(kAudioDevicePropertyVolumeScalar, element, next)) ok = false;
        }
        return ok;
    }

    bool setMuted(bool value) const {
        if (muteElements.empty()) return false;
        bool ok = true;
        for (auto element : muteElements)
            if (!write(kAudioDevicePropertyMute, element, UInt32(value))) ok = false;
        return ok;
    }
};
