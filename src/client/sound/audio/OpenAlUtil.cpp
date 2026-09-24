// File: src/client/sound/audio/OpenAlUtil.cpp
#include "client/sound/audio/OpenAlUtil.hpp"

#include "common/core/Log.hpp"

namespace Client::Audio {

    namespace {
        const char* AlErrorToString(ALenum error) {
            switch (error) {
                case AL_INVALID_NAME:      return "Invalid name parameter.";
                case AL_INVALID_ENUM:      return "Invalid enumerated parameter value.";
                case AL_INVALID_VALUE:     return "Invalid parameter parameter value.";
                case AL_INVALID_OPERATION: return "Invalid operation.";
                case AL_OUT_OF_MEMORY:     return "Unable to allocate memory.";
                default:                   return "An unrecognized error occurred.";
            }
        }
        const char* AlcErrorToString(ALCenum error) {
            switch (error) {
                case ALC_INVALID_DEVICE:  return "Invalid device.";
                case ALC_INVALID_CONTEXT: return "Invalid context.";
                case ALC_INVALID_ENUM:    return "Illegal enum.";
                case ALC_INVALID_VALUE:   return "Invalid value.";
                case ALC_OUT_OF_MEMORY:   return "Unable to allocate memory.";
                default:                  return "An unrecognized error occurred.";
            }
        }
    } // namespace

    bool CheckALError(const char* location) {
        const ALenum error = alGetError();
        if (error != AL_NO_ERROR) {
            Log::Error("[Sound] %s: %s", location, AlErrorToString(error));
            return true;
        }
        return false;
    }

    bool CheckALCError(ALCdevice* device, const char* location) {
        const ALCenum error = alcGetError(device);
        if (error != ALC_NO_ERROR) {
            Log::Error("[Sound] %s (%p): %s", location, static_cast<void*>(device), AlcErrorToString(error));
            return true;
        }
        return false;
    }

    ALenum AudioFormatToOpenAl(const AudioFormat& format) {
        if (format.channels == 1) return AL_FORMAT_MONO16;
        if (format.channels == 2) return AL_FORMAT_STEREO16;
        return 0;
    }

} // namespace Client::Audio
