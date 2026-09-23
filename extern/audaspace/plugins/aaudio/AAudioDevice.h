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

#pragma once

/**
 * @file AAudioDevice.h
 * @ingroup plugin
 * The AAudioDevice class.
 *
 * Touch: added by the Blender for Android fork. Audaspace ships a device for every desktop audio
 * API and none for Android, so the port had sound support compiled in and nowhere to send it.
 * AAudio is the NDK's own output API, present since API 26 against a minSdk of 31, and needs no
 * library beyond the platform's own libaaudio.so.
 */

#include "devices/SoftwareDevice.h"

#include <aaudio/AAudio.h>

#include <atomic>
#include <mutex>
#include <thread>

AUD_NAMESPACE_BEGIN

/**
 * This device plays back through AAudio, Android's native audio output API.
 */
class AUD_PLUGIN_API AAudioDevice : public SoftwareDevice
{
private:
	/**
	 * Whether there is currently playback. Read from the audio callback thread.
	 */
	std::atomic<bool> m_playback;

	/**
	 * The AAudio output stream. Replaced by #reopen_stream after a disconnect.
	 */
	AAudioStream* m_stream;

	/**
	 * Guards #m_stream against the reopen thread.
	 */
	std::mutex m_stream_mutex;

	/**
	 * Set while a reopen is pending, so a burst of error callbacks starts only one.
	 */
	std::atomic<bool> m_reopening;

	/**
	 * Set by the destructor so a pending reopen does not open a stream nobody will close.
	 */
	std::atomic<bool> m_closing;

	std::thread m_reopen_thread;

	/**
	 * Opens an output stream for #m_specs and points the callbacks at this device.
	 * \param specs Updated to what the stream actually gave.
	 * \return The stream, or nullptr if AAudio refused.
	 */
	AUD_LOCAL AAudioStream* open_stream(DeviceSpecs& specs);

	/**
	 * Replaces a disconnected stream with one on the current route. Runs on #m_reopen_thread.
	 */
	AUD_LOCAL void reopen_stream();

	/**
	 * AAudio callback to mix the next frames into the stream's buffer.
	 * \param stream The stream asking for data.
	 * \param user_data The device.
	 * \param audio_data The buffer to fill.
	 * \param num_frames How many frames are wanted.
	 */
	AUD_LOCAL static aaudio_data_callback_result_t mix_callback(AAudioStream* stream, void* user_data, void* audio_data, int32_t num_frames);

	/**
	 * AAudio callback for a stream that has become unusable, which happens when the route
	 * changes -- headphones unplugged, a Bluetooth speaker connected. The stream cannot be closed
	 * from the callback thread, so this starts #reopen_stream on a thread of its own.
	 */
	AUD_LOCAL static void error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error);

	// delete copy constructor and operator=
	AAudioDevice(const AAudioDevice&) = delete;
	AAudioDevice& operator=(const AAudioDevice&) = delete;

protected:
	virtual void playing(bool playing);

public:
	/**
	 * Opens the AAudio device for playback.
	 * \param specs The wanted audio specification.
	 * \param buffersize The size of the internal buffer.
	 * \note The specification really used for opening the device may differ; the stream is asked
	 *       what it actually gave and #m_specs is set from that.
	 * \exception DeviceException Thrown if the audio device cannot be opened.
	 */
	AAudioDevice(DeviceSpecs specs, int buffersize = AUD_DEFAULT_BUFFER_SIZE);

	/**
	 * Closes the AAudio device.
	 */
	virtual ~AAudioDevice();

	/**
	 * Registers this plugin.
	 */
	static void registerPlugin();
};

AUD_NAMESPACE_END
