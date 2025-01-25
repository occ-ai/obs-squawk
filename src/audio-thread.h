#ifndef AUDIO_THREAD_H
#define AUDIO_THREAD_H

#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <deque>
#include <atomic>

#include <obs.h>

#include "plugin-support.h"

class AudioThread {
public:
	AudioThread(obs_source_t *ctx) : buffer(), mutex(), context(ctx) {}

	void start()
	{
		// check if the thread is already running
		if (thread.joinable()) {
			return;
		}
		running = true;
		// Start the thread
		thread = std::thread(&AudioThread::run, this);
	}

	void stop()
	{
		running = false;
		// Stop the thread
		if (thread.joinable()) {
			thread.join();
		}
	}

	void pushAudioSamples(const std::vector<float> &samples)
	{
		obs_log(LOG_INFO, "Pushing %lu audio samples to buffer", samples.size());

		// Lock the mutex
		std::lock_guard<std::mutex> lock(mutex);

		if (interrupt_mode) {
			// Clear the buffer if in interrupt mode
			buffer.clear();
		}

		// Push audio samples to the buffer
		for (auto sample : samples) {
			buffer.push_back(sample);
		}
	}

	void setSampleRate(int sample_rate_) { sample_rate = sample_rate_; }
	void setTargetBatchSizeMs(int target_batch_size_ms)
	{
		TARGET_BATCH_SIZE_MS = target_batch_size_ms;
	}
	void setInterruptMode(bool interrupt_mode_) { interrupt_mode = interrupt_mode_; }

private:
	int TARGET_BATCH_SIZE_MS = 50;

	std::deque<float> buffer;
	std::mutex mutex;
	std::thread thread;
	obs_source_t *context;
	int sample_rate = 22050;
	std::atomic<bool> running = false;
	bool interrupt_mode = false;

	void run();
	void emitFromBuffer();
	void emitSilence();
	void emitAudioSamples(const std::vector<float> &samples, int sample_rate);
};

#endif // AUDIO_THREAD_H
