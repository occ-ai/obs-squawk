#include <thread>
#include <chrono>

#include "audio-thread.h"

long long now_ns()
{
	auto now = std::chrono::system_clock::now();
	auto duration = now.time_since_epoch();
	return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void AudioThread::run()
{
	while (this->running) {
		auto start = std::chrono::high_resolution_clock::now();

		// Check if there are audio samples in the buffer
		if (this->buffer.empty()) {
			// Emit silence
			emitSilence();
		} else {
			// Emit audio samples
			emitFromBuffer();
		}

		// Calculate the time taken to process the audio samples
		const auto end = std::chrono::high_resolution_clock::now();
		const auto duration =
			std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		const auto sleep_duration_ms =
			std::chrono::milliseconds(TARGET_BATCH_SIZE_MS) - duration;

		// Sleep for [TARGET_BATCH_SIZE_MS] minus the time taken to process the audio samples
		std::this_thread::sleep_for(sleep_duration_ms);
	}
}

void AudioThread::emitFromBuffer()
{
	// Lock the mutex
	std::lock_guard<std::mutex> lock(mutex);

	const int target_number_of_samples = TARGET_BATCH_SIZE_MS * sample_rate / 1000;

	// Get audio samples from the buffer
	std::vector<float> samples;
	for (int i = 0; i < target_number_of_samples; i++) {
		if (this->buffer.empty()) {
			break;
		}
		samples.push_back(this->buffer.front());
		this->buffer.pop_front();
	}

	// if needed - pad the samples with silence to reach the target batch size
	while ((int)samples.size() < target_number_of_samples) {
		samples.push_back(0.0f);
	}

	// Emit audio samples
	emitAudioSamples(samples, sample_rate);
}

void AudioThread::emitSilence()
{
	// Emit silence
	std::vector<float> silence(TARGET_BATCH_SIZE_MS * sample_rate / 1000, 0.0f);
	emitAudioSamples(silence, sample_rate);
}

void AudioThread::emitAudioSamples(const std::vector<float> &samples, int sample_rate_)
{
	// emit audio samples
	obs_source_audio j;
	j.data[0] = (uint8_t *)samples.data();
	j.data[1] = nullptr;
	j.frames = (uint32_t)samples.size();
	j.speakers = SPEAKERS_MONO;
	j.samples_per_sec = sample_rate_;
	j.format = AUDIO_FORMAT_FLOAT;
	j.timestamp = now_ns() - TARGET_BATCH_SIZE_MS * 1000000;
	// emit audio samples
	obs_source_output_audio(this->context, &j);
}
