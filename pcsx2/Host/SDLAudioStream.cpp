// SPDX-FileCopyrightText: 2002-2024 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Host/AudioStream.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include <SDL3/SDL.h>

namespace
{
	class SDLAudioStream final : public AudioStream
	{
	public:
		SDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~SDLAudioStream();

		void SetPaused(bool paused) override;

		bool OpenDevice(bool stretch_enabled, Error* error);
		void CloseDevice();

	protected:
		__fi bool IsOpen() const { return (m_device_stream != nullptr); }

		__fi u32 CalculateSampleCount(u32 bytes_len) const { return bytes_len / sizeof(SampleType) / m_output_channels; }

		static void AudioCallback(void* userdata, SDL_AudioStream* stream, int len, int total_amount);

		SDL_AudioStream* m_device_stream = nullptr;

		std::unique_ptr<SampleType[]> m_sample_buffer = nullptr;

		u32 m_sample_buffer_size = 0;
	};
} // namespace

static bool InitializeSDLAudio(Error* error)
{
	static bool initialized = false;
	if (initialized)
		return true;

	// May as well keep it alive until the process exits.
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
	{
		Error::SetStringFmt(error, "SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
		return false;
	}

	std::atexit([]() { SDL_QuitSubSystem(SDL_INIT_AUDIO); });

	initialized = true;
	return true;
}

SDLAudioStream::SDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

SDLAudioStream::~SDLAudioStream()
{
	if (IsOpen())
		SDLAudioStream::CloseDevice();
}

std::unique_ptr<AudioStream> AudioStream::CreateSDLAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	if (!InitializeSDLAudio(error))
		return {};

	std::unique_ptr<SDLAudioStream> stream = std::make_unique<SDLAudioStream>(sample_rate, parameters);
	if (!stream->OpenDevice(stretch_enabled, error))
		stream.reset();

	return stream;
}

bool SDLAudioStream::OpenDevice(bool stretch_enabled, Error* error)
{
	pxAssert(!IsOpen());

	static constexpr const std::array<SampleReader, static_cast<size_t>(AudioExpansionMode::Count)> sample_readers = {{
		// Disabled
		&StereoSampleReaderImpl,
		// StereoLFE
		&SampleReaderImpl<AudioExpansionMode::StereoLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_LFE>,
		// Quadraphonic
		&SampleReaderImpl<AudioExpansionMode::Quadraphonic, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		// QuadraphonicLFE
		&SampleReaderImpl<AudioExpansionMode::QuadraphonicLFE, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		// Surround51
		&SampleReaderImpl<AudioExpansionMode::Surround51, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
		// Surround71
		&SampleReaderImpl<AudioExpansionMode::Surround71, READ_CHANNEL_FRONT_LEFT, READ_CHANNEL_FRONT_RIGHT,
			READ_CHANNEL_FRONT_CENTER, READ_CHANNEL_LFE, READ_CHANNEL_SIDE_LEFT, READ_CHANNEL_SIDE_RIGHT,
			READ_CHANNEL_REAR_LEFT, READ_CHANNEL_REAR_RIGHT>,
	}};

	u32 sample_frames = GetBufferSizeForMS(m_sample_rate, (m_parameters.minimal_output_latency) ? m_parameters.buffer_ms : m_parameters.output_latency_ms);
	if (!SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(sample_frames).c_str()))
		Console.Warning("SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) failed: {}", SDL_GetError());

	SDL_AudioSpec spec = {};
	spec.format = SDL_AUDIO_S16;
	spec.channels = m_output_channels;
	spec.freq = m_sample_rate;

	m_device_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, AudioCallback, static_cast<void*>(this));
	if (!m_device_stream) {
		Error::SetStringFmt(error, "SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
		return false;
	}

	SDL_AudioSpec obtained_spec = {};
	int obtained_sample_frames = 0;
	SDL_AudioDeviceID device_id = SDL_GetAudioStreamDevice(m_device_stream);
	if (SDL_GetAudioDeviceFormat(device_id, &obtained_spec, &obtained_sample_frames))
		DEV_LOG("Requested {} frame buffer, got {} frame buffer", sample_frames, obtained_sample_frames);
	else
		Console.Warning("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());

	m_sample_buffer_size = CalculateSampleCount(obtained_sample_frames);
	m_sample_buffer = std::make_unique<SampleType[]>(m_sample_buffer_size);
	BaseInitialize(sample_readers[static_cast<size_t>(m_parameters.expansion_mode)], stretch_enabled);

	return true;
}

void SDLAudioStream::SetPaused(bool paused)
{
	if (m_paused == paused)
		return;

	SDL_AudioDeviceID device_id = SDL_GetAudioStreamDevice(m_device_stream);
	if (paused)
		SDL_PauseAudioDevice(device_id);
	else
		SDL_ResumeAudioDevice(device_id);

	m_paused = paused;
}

void SDLAudioStream::CloseDevice()
{
	SDL_DestroyAudioStream(m_device_stream);
	m_device_stream = nullptr;
	m_sample_buffer_size = 0;
}

void SDLAudioStream::AudioCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int /* total_amount */)
{
	SDLAudioStream* const this_ptr = static_cast<SDLAudioStream*>(userdata);
	u32 num_frames = this_ptr->CalculateSampleCount(additional_amount);

	if (num_frames > this_ptr->m_sample_buffer_size) {
		Console.Warning("AudioCallback received request greater than buffer {}, buffer is {}", num_frames, this_ptr->m_sample_buffer_size);
		num_frames = this_ptr->m_sample_buffer_size;
	}

	this_ptr->ReadFrames(this_ptr->m_sample_buffer.get(), num_frames);
}
