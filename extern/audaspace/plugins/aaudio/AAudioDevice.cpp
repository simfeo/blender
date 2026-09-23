/*******************************************************************************
 * Copyright 2009-2016 Jörg Müller
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#include "AAudioDevice.h"

#include "devices/DeviceManager.h"
#include "devices/IDeviceFactory.h"
#include "Exception.h"
#include "IReader.h"

#include <cstring>

AUD_NAMESPACE_BEGIN

aaudio_data_callback_result_t AAudioDevice::mix_callback(AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames)
{
	AAudioDevice* device = static_cast<AAudioDevice*>(user_data);

	/* AAudio wants a full buffer every time it asks, whether or not anything is playing: returning
	 * without filling it plays whatever the buffer held before, which is the previous sound over
	 * again. So silence is written explicitly when stopped. */
	if(!device->m_playback.load(std::memory_order_relaxed))
	{
		std::memset(audio_data, 0, size_t(num_frames) * AUD_DEVICE_SAMPLE_SIZE(device->m_specs));
		return AAUDIO_CALLBACK_RESULT_CONTINUE;
	}

	device->mix(static_cast<data_t*>(audio_data), num_frames);

	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioDevice::error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error)
{
	/* Nothing to do here on purpose. The stream is gone -- the usual cause is the output route
	 * changing, headphones pulled out or a Bluetooth speaker connecting -- and AAudio forbids
	 * closing or reopening it from this callback, which runs on the audio thread. Reopening would
	 * have to be posted to another thread; until that exists, sound stops until Blender opens the
	 * device again, which it does whenever the audio preferences are touched. */
}

void AAudioDevice::playing(bool playing)
{
	if(m_playback.exchange(playing) == playing)
		return;

	if(playing)
		AAudioStream_requestStart(m_stream);
	else
		AAudioStream_requestPause(m_stream);
}

AAudioDevice::AAudioDevice(DeviceSpecs specs, int buffersize) :
	m_playback(false),
	m_stream(nullptr)
{
	if(specs.channels == CHANNELS_INVALID)
		specs.channels = CHANNELS_STEREO;
	if(specs.format == FORMAT_INVALID)
		specs.format = FORMAT_FLOAT32;
	if(specs.rate == RATE_INVALID)
		specs.rate = RATE_48000;

	AAudioStreamBuilder* builder = nullptr;

	if(AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
		AUD_THROW(DeviceException, "The AAudio stream builder could not be created.");

	/* AAudio only speaks 16 bit integer and 32 bit float. Anything else audaspace might ask for is
	 * asked of it as float, which is what its mixer works in anyway. */
	aaudio_format_t format = AAUDIO_FORMAT_PCM_FLOAT;
	if(specs.format == FORMAT_S16)
		format = AAUDIO_FORMAT_PCM_I16;
	else
		specs.format = FORMAT_FLOAT32;

	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setFormat(builder, format);
	AAudioStreamBuilder_setChannelCount(builder, specs.channels);
	AAudioStreamBuilder_setSampleRate(builder, int32_t(specs.rate));
	AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
	AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);

	/* Not AAUDIO_PERFORMANCE_MODE_LOW_LATENCY. That mode hands out a buffer of a few milliseconds
	 * and expects the callback to always beat it; audaspace mixes an arbitrary number of sound
	 * sources with effects on them, and missing that deadline is an audible gap. Blender is playing
	 * a timeline here rather than answering a touch, so the larger buffer is the right trade. */
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);

	AAudioStreamBuilder_setDataCallback(builder, AAudioDevice::mix_callback, this);
	AAudioStreamBuilder_setErrorCallback(builder, AAudioDevice::error_callback, this);

	const aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &m_stream);

	AAudioStreamBuilder_delete(builder);

	if(result != AAUDIO_OK || m_stream == nullptr)
		AUD_THROW(DeviceException, "The audio device couldn't be opened with AAudio.");

	/* What was asked for and what was given are not the same thing: the device has a rate of its
	 * own and AAudio resamples only if it feels like it. Everything downstream mixes to m_specs, so
	 * it has to describe the stream that actually exists or every sound plays at the wrong pitch. */
	specs.rate = SampleRate(AAudioStream_getSampleRate(m_stream));
	specs.channels = Channels(AAudioStream_getChannelCount(m_stream));
	specs.format = AAudioStream_getFormat(m_stream) == AAUDIO_FORMAT_PCM_I16 ? FORMAT_S16 : FORMAT_FLOAT32;

	m_specs = specs;

	create();
}

AAudioDevice::~AAudioDevice()
{
	destroy();

	if(m_stream != nullptr)
	{
		AAudioStream_requestStop(m_stream);
		AAudioStream_close(m_stream);
		m_stream = nullptr;
	}
}

class AAudioDeviceFactory : public IDeviceFactory
{
private:
	DeviceSpecs m_specs;
	int m_buffersize;

public:
	AAudioDeviceFactory() :
		m_buffersize(AUD_DEFAULT_BUFFER_SIZE)
	{
		m_specs.format = FORMAT_FLOAT32;
		m_specs.channels = CHANNELS_STEREO;
		m_specs.rate = RATE_48000;
	}

	virtual std::shared_ptr<IDevice> openDevice()
	{
		return std::shared_ptr<IDevice>(new AAudioDevice(m_specs, m_buffersize));
	}

	virtual int getPriority()
	{
		/* Above every other device, because on Android there is no other: nothing else audaspace
		 * can build here opens a working output. Blender picks names[0] by default, and this makes
		 * that the one that works. */
		return 1 << 15;
	}

	virtual void setSpecs(DeviceSpecs specs)
	{
		m_specs = specs;
	}

	virtual void setBufferSize(int buffersize)
	{
		m_buffersize = buffersize;
	}

	virtual void setName(const std::string &name)
	{
	}
};

void AAudioDevice::registerPlugin()
{
	DeviceManager::registerDevice("AAudio", std::shared_ptr<IDeviceFactory>(new AAudioDeviceFactory));
}

#ifdef AAUDIO_PLUGIN
extern "C" AUD_PLUGIN_API void registerPlugin()
{
	AAudioDevice::registerPlugin();
}

extern "C" AUD_PLUGIN_API const char* getName()
{
	return "AAudio";
}
#endif

AUD_NAMESPACE_END
