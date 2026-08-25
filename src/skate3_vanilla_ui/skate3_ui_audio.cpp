#include "skate3_ui_audio.h"

#include "generated/skate3_init.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>

namespace skate3::vanilla_ui {
namespace {

using Json = nlohmann::json;

constexpr int kSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr uint32_t kRetailMenuSoundQueue = 0x82495828;

std::atomic<float> g_retail_frontend_volume{1.0f};

const char *EventName(MenuSound sound) {
  switch (sound) {
  case MenuSound::kNavigateUp:
    return "core_nav_up";
  case MenuSound::kNavigateDown:
    return "core_nav_down";
  case MenuSound::kConfirm:
    return "core_a_button";
  case MenuSound::kBack:
    return "core_b_button";
  case MenuSound::kFade:
    return "core_fade";
  case MenuSound::kTransitionIn:
    return "crossbar_in";
  case MenuSound::kTransitionOut:
    return "crossbar_out";
  case MenuSound::kPopup:
    return "core_popup";
  }
  return "";
}

struct Clip {
  std::vector<float> samples;
};

struct Layer {
  std::shared_ptr<const Clip> clip;
  double pitch_base = 1.0;
  double pitch_random_range = 0.0;
  double delay_seconds = 0.0;
  double delay_random_range_seconds = 0.0;
  float gain = 1.0f;
  float gain_random_min = 1.0f;
};

struct RandomGroup {
  std::vector<Layer> alternatives;
};

struct Event {
  float volume = 1.0f;
  float graph_gain = 1.0f;
  double graph_pitch_base = 1.0;
  double graph_pitch_random_range = 0.0;
  std::size_t duration_frames = 0;
  std::vector<Layer> layers;
  std::vector<RandomGroup> random_groups;
};

struct Voice {
  std::shared_ptr<const Clip> clip;
  double position = 0.0;
  double pitch_ratio = 1.0;
  std::size_t delay_frames = 0;
  std::size_t remaining_frames = 0;
  float volume = 1.0f;
};

bool SafeRelativePath(const std::filesystem::path &path) {
  return !path.empty() && !path.is_absolute() && !path.has_root_name() &&
         std::none_of(path.begin(), path.end(), [](const auto &part) {
           return part == "." || part == "..";
         });
}

} // namespace

extern "C" REX_FUNC(Skate3VanillaUi_MenuSoundQueueHook) {
  const uint32_t manager = ctx.r3.u32;
  const int32_t sound_type = ctx.r5.s32;
  if (manager >= 0x10000u) {
    const uint32_t settings = REX_LOAD_U32(manager + 24);
    if (settings >= 0x10000u) {
      const uint32_t field = sound_type == 5 || sound_type == 2 ? 40u : 44u;
      const int32_t percentage =
          static_cast<int32_t>(REX_LOAD_U32(settings + field));
      if (percentage >= 0 && percentage <= 100) {
        g_retail_frontend_volume.store(static_cast<float>(percentage) * 0.01f,
                                       std::memory_order_release);
      }
    }
  }
  sub_82495828(ctx, base);
}

struct RetailMenuAudio::Impl {
  explicit Impl(std::filesystem::path root) : cache_root(std::move(root)) {}

  ~Impl() {
    if (stream) {
      SDL_DestroyAudioStream(stream);
      stream = nullptr;
    }
  }

  std::filesystem::path cache_root;
  SDL_AudioStream *stream = nullptr;
  std::map<std::string, Event, std::less<>> events;
  std::map<std::filesystem::path, std::shared_ptr<const Clip>> clips;
  std::vector<Voice> voices;
  std::mt19937 random{std::random_device{}()};
  std::mutex mutex;
  bool attempted = false;
  bool ready = false;
  std::string error;

  static void SDLCALL AudioCallback(void *userdata,
                                    SDL_AudioStream *audio_stream,
                                    int additional_amount,
                                    [[maybe_unused]] int total_amount) {
    auto *self = static_cast<Impl *>(userdata);
    if (!self || !audio_stream || additional_amount <= 0) {
      return;
    }
    const std::size_t float_count =
        static_cast<std::size_t>(additional_amount) / sizeof(float);
    const std::size_t frame_count = float_count / kOutputChannels;
    std::vector<float> output(frame_count * kOutputChannels, 0.0f);
    {
      std::lock_guard lock(self->mutex);
      for (auto &voice : self->voices) {
        for (std::size_t frame = 0;
             frame < frame_count && voice.remaining_frames > 0; ++frame) {
          --voice.remaining_frames;
          if (voice.delay_frames > 0) {
            --voice.delay_frames;
            continue;
          }
          const auto source_frame = static_cast<std::size_t>(voice.position);
          if (source_frame >= voice.clip->samples.size()) {
            voice.remaining_frames = 0;
            break;
          }
          const auto next_frame =
              std::min(source_frame + 1, voice.clip->samples.size() - 1);
          const auto fraction =
              static_cast<float>(voice.position - source_frame);
          const float source =
              std::lerp(voice.clip->samples[source_frame],
                        voice.clip->samples[next_frame], fraction);
          const float sample = source * voice.volume;
          output[frame * 2] += sample;
          output[frame * 2 + 1] += sample;
          voice.position += voice.pitch_ratio;
        }
      }
      std::erase_if(self->voices, [](const Voice &voice) {
        return voice.remaining_frames == 0 ||
               voice.position >= voice.clip->samples.size();
      });
    }
    SDL_PutAudioStreamData(audio_stream, output.data(),
                           static_cast<int>(output.size() * sizeof(float)));
  }

  std::shared_ptr<const Clip>
  LoadClip(const std::filesystem::path &manifest_root,
           const std::filesystem::path &relative) {
    if (!SafeRelativePath(relative)) {
      error = "menu-audio manifest contains an unsafe WAV path";
      return {};
    }
    const auto path = (manifest_root / relative).lexically_normal();
    if (const auto found = clips.find(path); found != clips.end()) {
      return found->second;
    }

    SDL_AudioSpec source_spec{};
    Uint8 *source_data = nullptr;
    Uint32 source_length = 0;
    const auto path_string = path.string();
    if (!SDL_LoadWAV(path_string.c_str(), &source_spec, &source_data,
                     &source_length)) {
      error = "Unable to load exact retail menu sound " + path_string + ": " +
              SDL_GetError();
      return {};
    }
    const SDL_AudioSpec target_spec = {
        .format = SDL_AUDIO_F32LE,
        .channels = 1,
        .freq = kSampleRate,
    };
    Uint8 *converted_data = nullptr;
    int converted_length = 0;
    const bool converted = SDL_ConvertAudioSamples(
        &source_spec, source_data, static_cast<int>(source_length),
        &target_spec, &converted_data, &converted_length);
    SDL_free(source_data);
    if (!converted) {
      error = "Unable to convert exact retail menu sound " + path_string +
              ": " + SDL_GetError();
      return {};
    }
    auto clip = std::make_shared<Clip>();
    const auto sample_count =
        static_cast<std::size_t>(converted_length) / sizeof(float);
    const auto *samples = reinterpret_cast<const float *>(converted_data);
    clip->samples.assign(samples, samples + sample_count);
    SDL_free(converted_data);
    clips.emplace(path, clip);
    return clip;
  }

  bool EnsureLoaded() {
    if (attempted) {
      return ready;
    }
    attempted = true;
    const auto manifest_path = RetailMenuAudioManifestPath(cache_root);
    std::ifstream input(manifest_path);
    if (!input) {
      error = "Exact retail menu audio has not been extracted: " +
              manifest_path.string();
      REXLOG_WARN("{}", error);
      return false;
    }
    try {
      Json manifest;
      input >> manifest;
      if (manifest.at("format").get<std::string>() !=
          "skate3-native-menu-audio-v2") {
        throw std::runtime_error("unsupported menu-audio manifest format");
      }
      const auto manifest_root = manifest_path.parent_path();
      for (const auto &[name, source] : manifest.at("events").items()) {
        Event event;
        event.volume = source.at("volume").get<float>();
        event.graph_gain = source.at("graph_gain").get<float>();
        event.graph_pitch_base = source.at("graph_pitch_base").get<double>();
        event.graph_pitch_random_range =
            source.at("graph_pitch_random_range").get<double>();
        if (!std::isfinite(event.graph_gain) || event.graph_gain < 0.0f ||
            !std::isfinite(event.graph_pitch_base) ||
            event.graph_pitch_base <= 0.0 ||
            !std::isfinite(event.graph_pitch_random_range)) {
          throw std::runtime_error(name + " has invalid SPLC graph parameters");
        }
        const double duration_ms = source.at("duration_ms").get<double>();
        if (!std::isfinite(duration_ms) || duration_ms <= 0.0) {
          throw std::runtime_error(name + " has an invalid SPLC duration");
        }
        event.duration_frames = static_cast<std::size_t>(
            std::ceil(duration_ms * static_cast<double>(kSampleRate) / 1000.0));
        for (const auto &layer : source.at("layers")) {
          auto clip =
              LoadClip(manifest_root, layer.at("wav").get<std::string>());
          if (!clip) {
            REXLOG_WARN("Native UI menu audio unavailable: {}", error);
            return false;
          }
          const double pitch_base = layer.at("pitch_base").get<double>();
          const double pitch_random_range =
              layer.at("pitch_random_range").get<double>();
          const double delay_seconds = layer.at("delay_seconds").get<double>();
          const double delay_random_range_seconds =
              layer.at("delay_random_range_seconds").get<double>();
          if (!std::isfinite(pitch_base) || pitch_base <= 0.0 ||
              !std::isfinite(pitch_random_range) ||
              !std::isfinite(delay_seconds) ||
              !std::isfinite(delay_random_range_seconds)) {
            throw std::runtime_error(name +
                                     " has invalid SPLC timing or pitch");
          }
          const float gain = layer.at("gain").get<float>();
          const float gain_random_min =
              layer.at("gain_random_min").get<float>();
          if (!std::isfinite(gain) || gain < 0.0f ||
              !std::isfinite(gain_random_min) || gain_random_min <= 0.0f) {
            throw std::runtime_error(name + " has an invalid SPLC gain");
          }
          event.layers.push_back(
              {.clip = std::move(clip),
               .pitch_base = pitch_base,
               .pitch_random_range = pitch_random_range,
               .delay_seconds = delay_seconds,
               .delay_random_range_seconds = delay_random_range_seconds,
               .gain = gain,
               .gain_random_min = gain_random_min});
        }
        for (const auto &source_group : source.at("random_groups")) {
          if (source_group.at("selection").get<std::string>() !=
              "uniform_one") {
            throw std::runtime_error(name +
                                     " has an unsupported random selection");
          }
          RandomGroup group;
          for (const auto &alternative : source_group.at("alternatives")) {
            auto clip = LoadClip(manifest_root,
                                 alternative.at("wav").get<std::string>());
            if (!clip) {
              REXLOG_WARN("Native UI menu audio unavailable: {}", error);
              return false;
            }
            const double pitch_base =
                alternative.at("pitch_base").get<double>();
            const double pitch_random_range =
                alternative.at("pitch_random_range").get<double>();
            const double delay_seconds =
                alternative.at("delay_seconds").get<double>();
            const double delay_random_range_seconds =
                alternative.at("delay_random_range_seconds").get<double>();
            const float gain = alternative.at("gain").get<float>();
            const float gain_random_min =
                alternative.at("gain_random_min").get<float>();
            if (!std::isfinite(pitch_base) || pitch_base <= 0.0 ||
                !std::isfinite(pitch_random_range) ||
                !std::isfinite(delay_seconds) ||
                !std::isfinite(delay_random_range_seconds) ||
                !std::isfinite(gain) || gain < 0.0f ||
                !std::isfinite(gain_random_min) || gain_random_min <= 0.0f) {
              throw std::runtime_error(name +
                                       " has invalid random SPLC parameters");
            }
            group.alternatives.push_back(
                {.clip = std::move(clip),
                 .pitch_base = pitch_base,
                 .pitch_random_range = pitch_random_range,
                 .delay_seconds = delay_seconds,
                 .delay_random_range_seconds = delay_random_range_seconds,
                 .gain = gain,
                 .gain_random_min = gain_random_min});
          }
          if (group.alternatives.empty()) {
            throw std::runtime_error(name + " has an empty SPLC random grain");
          }
          event.random_groups.push_back(std::move(group));
        }
        events.emplace(name, std::move(event));
      }
    } catch (const std::exception &exception) {
      error = "Unable to parse exact retail menu audio: " +
              std::string(exception.what());
      REXLOG_WARN("{}", error);
      return false;
    }

    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0 &&
        !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      error = "Unable to initialize native menu audio: " +
              std::string(SDL_GetError());
      REXLOG_WARN("{}", error);
      return false;
    }
    const SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32LE,
        .channels = kOutputChannels,
        .freq = kSampleRate,
    };
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                       AudioCallback, this);
    if (!stream) {
      error = "Unable to open native menu audio stream: " +
              std::string(SDL_GetError());
      REXLOG_WARN("{}", error);
      return false;
    }
    const auto device = SDL_GetAudioStreamDevice(stream);
    if (!device || !SDL_ResumeAudioDevice(device)) {
      error = "Unable to start native menu audio stream: " +
              std::string(SDL_GetError());
      REXLOG_WARN("{}", error);
      SDL_DestroyAudioStream(stream);
      stream = nullptr;
      return false;
    }
    ready = true;
    return true;
  }

  void Play(MenuSound sound) {
    if (!EnsureLoaded()) {
      return;
    }
    const auto found = events.find(EventName(sound));
    if (found == events.end()) {
      return;
    }
    std::lock_guard lock(mutex);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double graph_pitch =
        found->second.graph_pitch_base +
        unit(random) * found->second.graph_pitch_random_range;
    const float frontend_volume =
        g_retail_frontend_volume.load(std::memory_order_acquire);
    const auto add_voice = [&](const Layer &layer) {
      const double gain_position = unit(random) * 2.0 - 1.0;
      const double random_gain =
          gain_position >= 0.0
              ? 1.0 + (1.0 / layer.gain_random_min - 1.0) * gain_position
              : 1.0 + (1.0 - layer.gain_random_min) * gain_position;
      const double pitch_ratio =
          graph_pitch *
          (layer.pitch_base + unit(random) * layer.pitch_random_range);
      const double delay_seconds =
          std::max(0.0, layer.delay_seconds +
                            unit(random) * layer.delay_random_range_seconds);
      voices.push_back({.clip = layer.clip,
                        .position = 0.0,
                        .pitch_ratio = pitch_ratio,
                        .delay_frames = static_cast<std::size_t>(
                            std::llround(delay_seconds * kSampleRate)),
                        .remaining_frames = found->second.duration_frames,
                        .volume = frontend_volume * found->second.volume *
                                  found->second.graph_gain * layer.gain *
                                  static_cast<float>(random_gain)});
    };
    for (const auto &layer : found->second.layers) {
      add_voice(layer);
    }
    for (const auto &group : found->second.random_groups) {
      std::uniform_int_distribution<std::size_t> choice(
          0, group.alternatives.size() - 1);
      add_voice(group.alternatives[choice(random)]);
    }
  }
};

RetailMenuAudio::RetailMenuAudio(std::filesystem::path cache_root)
    : impl_(std::make_unique<Impl>(std::move(cache_root))) {}

RetailMenuAudio::~RetailMenuAudio() = default;

void RetailMenuAudio::Play(MenuSound sound) { impl_->Play(sound); }

void InstallAudioHooks(rex::runtime::FunctionDispatcher *dispatcher) {
  if (!dispatcher) {
    REXLOG_WARN("Vanilla UI audio: function dispatcher unavailable");
    return;
  }
  dispatcher->SetFunction(kRetailMenuSoundQueue,
                          &Skate3VanillaUi_MenuSoundQueueHook);
  REXLOG_INFO("Vanilla UI audio: retail front-end volume observer installed at "
              "0x{:08X}",
              kRetailMenuSoundQueue);
}

std::filesystem::path
RetailMenuAudioManifestPath(const std::filesystem::path &cache_root) {
  return cache_root / "assets" / "audio" / "sk8_menu" / "manifest.json";
}

} // namespace skate3::vanilla_ui
