#include "audio-thread.h"
#include "input-thread.h"
#include "model-utils/model-downloader-types.h"
#include "model-utils/model-downloader.h"
#include "model-utils/model-find-utils.h"
#include "plugin-support.h"
#include "sherpa-tts/sherpa-tts.h"
#include "squawk-source.h"
#include "squawk-source-data.h"
#include "tts-utils.h"

#include <new>
#include <chrono>

const char *squawk_source_name(void *unused)
{
	return "Squawk Text-to-Speech";
}

void audio_samples_callback(void *data, const float *samples, int num_samples, int sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	squawk_source_data *squawk_data = (squawk_source_data *)data;
	squawk_data->audioThread->pushAudioSamples(
		std::vector<float>(samples, samples + num_samples));
}

void *squawk_source_create(obs_data_t *settings, obs_source_t *source)
{
	obs_log(LOG_INFO, "Squawk source create");

	void *data = bmalloc(sizeof(squawk_source_data));
	squawk_source_data *squawk_data = new (data) squawk_source_data();

	squawk_data->tts_context.callback_data = squawk_data;

	squawk_data->context = source;
	squawk_data->audioThread = std::make_unique<AudioThread>(source);
	squawk_data->audioThread->start();

	squawk_data->inputThread = std::make_unique<InputThread>();
	squawk_data->inputThread->setSpeechGenerationCallback(
		[squawk_data](const std::string &text) {
			std::string transformed_text = text;
			if (squawk_data->phonetic_transcription) {
				transformed_text = phonetic_transcription(text);
			}
			generate_audio_from_text(squawk_data->tts_context, transformed_text.c_str(),
						 squawk_data->speaker_id);
		});
	squawk_data->inputThread->start();

	squawk_source_update(data, settings);

	return data;
}

void squawk_source_destroy(void *data)
{
	squawk_source_data *squawk_data = (squawk_source_data *)data;
	squawk_data->audioThread->stop();
	squawk_data->inputThread->stop();
	destroy_sherpa_tts_context(squawk_data->tts_context);
	squawk_data->~squawk_source_data();
	bfree(data);
}

void squawk_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "speaker_id", 0);
	obs_data_set_default_string(settings, "text", "Hello, World!");
	obs_data_set_default_string(settings, "model", "vits-coqui-en-vctk");
	obs_data_set_default_string(settings, "input_source", "none");
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_bool(settings, "phonetic_transcription", true);
}

bool add_sources_to_list(void *list_property, obs_source_t *source)
{
	auto source_id = obs_source_get_id(source);
	if (strcmp(source_id, "text_ft2_source_v2") != 0 &&
	    strcmp(source_id, "text_gdiplus_v2") != 0) {
		return true;
	}

	obs_property_t *sources = (obs_property_t *)list_property;
	const char *name = obs_source_get_name(source);
	obs_property_list_add_string(sources, name, name);
	return true;
}

obs_properties_t *squawk_source_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();

	// add model selection dropdown property
	obs_property_t *model = obs_properties_add_list(ppts, "model", "Model", OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	for (auto model_info : model_infos) {
		obs_property_list_add_string(model, model_info.friendly_name.c_str(),
					     model_info.local_folder_name.c_str());
	}
	// add a callback to update the model
	obs_property_set_modified_callback2(
		model,
		[](void *data_, obs_properties *props, obs_property_t *property,
		   obs_data_t *settings) {
			UNUSED_PARAMETER(props);
			UNUSED_PARAMETER(property);

			squawk_source_data *squawk_data_ = (squawk_source_data *)data_;
			const char *model_name = obs_data_get_string(settings, "model");
			obs_log(LOG_INFO, "Selected model: %s", model_name);
			// find model info by name
			auto model_info = find_model_info_by_name(model_name);
			std::string model_folder = find_model_folder(model_info);
			if (!model_folder.empty()) {
				obs_log(LOG_INFO, "Model folder found: %s", model_folder.c_str());
				return true;
			}

			obs_log(LOG_INFO, "Model folder not found - downloading...");
			download_model_with_ui_dialog(
				model_info, [model_info, squawk_data_](int download_status,
								       const std::string &path) {
					UNUSED_PARAMETER(download_status);
					obs_log(LOG_INFO, "Model downloaded: %s", path.c_str());
					unpack_model(model_info, path);
					// update the source
					obs_data_t *source_settings =
						obs_source_get_settings(squawk_data_->context);
					obs_source_update(squawk_data_->context, source_settings);
					obs_data_release(source_settings);
				});
			return true;
		},
		data);

	// add speaker id property
	obs_properties_add_int(ppts, "speaker_id", "Speaker ID", 0, 100, 1);

	// add input source selection dropdown property
	obs_property_t *input_source = obs_properties_add_list(
		ppts, "input_source", "Input Source", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	// Add "none" option
	obs_property_list_add_string(input_source, obs_module_text("none_no_output"), "none");
	// Add text sources
	obs_enum_sources(add_sources_to_list, input_source);
	// add file property
	obs_properties_add_path(ppts, "file", "File", OBS_PATH_FILE, nullptr, nullptr);

	// add text property
	obs_properties_add_text(ppts, "text", "Text", OBS_TEXT_DEFAULT);

	// add button property for generating audio on demand
	obs_properties_add_button(
		ppts, "generate_audio", "Generate Audio",
		[](obs_properties_t *props, obs_property_t *property, void *data_) {
			UNUSED_PARAMETER(props);
			UNUSED_PARAMETER(property);

			obs_log(LOG_INFO, "Generate Audio button clicked");
			squawk_source_data *squawk_data_ = (squawk_source_data *)data_;
			// get settings from source
			obs_data_t *settings = obs_source_get_settings(squawk_data_->context);
			std::string text = obs_data_get_string(settings, "text");
			int speaker_id = (int)obs_data_get_int(settings, "speaker_id");
			// release settings
			obs_data_release(settings);

			if (squawk_data_->phonetic_transcription) {
				std::string original_text = text;
				text = phonetic_transcription(text);
				obs_log(LOG_INFO, "Phonetic transcription: %s -> %s",
					original_text.c_str(), text.c_str());
			}

			generate_audio_from_text(squawk_data_->tts_context, text.c_str(),
						 speaker_id);

			return true;
		});

	// add button for deleting all cached models
	obs_properties_add_button(
		ppts, "delete_models", "Delete Cached Models",
		[](obs_properties_t *props, obs_property_t *property, void *data_) {
			UNUSED_PARAMETER(props);
			UNUSED_PARAMETER(property);

			obs_log(LOG_INFO, "Delete Cached Models button clicked");
			delete_cached_models();
			squawk_source_data *squawk_data_ = (squawk_source_data *)data_;
			// get settings from source
			obs_data_t *settings = obs_source_get_settings(squawk_data_->context);
			// set the model to the default model
			obs_data_set_string(settings, "model", "vits-coqui-en-vctk");
			// release settings
			obs_data_release(settings);
			return true;
		});

	// add boolean propery for enabling phonetic transcription
	obs_properties_add_bool(ppts, "phonetic_transcription", "Phonetic Transcription");

	return ppts;
}

void squawk_source_update(void *data, obs_data_t *settings)
{
	obs_log(LOG_INFO, "Squawk source update");

	squawk_source_data *squawk_data = (squawk_source_data *)data;

	squawk_data->speaker_id = (int)obs_data_get_int(settings, "speaker_id");
	squawk_data->phonetic_transcription = obs_data_get_bool(settings, "phonetic_transcription");

	std::string source = obs_data_get_string(settings, "input_source");
	if (source == "none") {
		source = "";
	}
	squawk_data->inputThread->setOBSTextSource(source);

	squawk_data->inputThread->setFile(obs_data_get_string(settings, "file"));

	std::string new_model_name = obs_data_get_string(settings, "model");
	if (new_model_name != squawk_data->tts_context.model_name) {
		destroy_sherpa_tts_context(squawk_data->tts_context);
		// find model info by name
		squawk_data->tts_context.model_name = new_model_name;
		// init the tts context
		init_sherpa_tts_context(squawk_data->tts_context, audio_samples_callback,
					squawk_data);
	}
}

void squawk_source_activate(void *data)
{
	UNUSED_PARAMETER(data);
}

void squawk_source_deactivate(void *data)
{
	UNUSED_PARAMETER(data);
}

void squawk_source_show(void *data)
{
	UNUSED_PARAMETER(data);
}

void squawk_source_hide(void *data)
{
	UNUSED_PARAMETER(data);
}
