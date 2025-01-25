#include "audio-thread.h"
#include "input-thread.h"
#include "model-utils/model-downloader-types.h"
#include "model-utils/model-downloader.h"
#include "model-utils/model-find-utils.h"
#include "plugin-support.h"
#include "sherpa-tts/sherpa-tts.h"
#include "squawk-source-data.h"
#include "squawk-source.h"
#include "tts-utils.h"

#include <new>
#include <chrono>

const char *squawk_source_name(void *unused)
{
	UNUSED_PARAMETER(unused);
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
	squawk_data->tts_context.model_name = "";

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
			generate_audio_from_text(squawk_data->tts_context, transformed_text,
						 squawk_data->speaker_id, squawk_data->speed);
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
	obs_data_set_default_double(settings, "speed", 1.0);
	obs_data_set_default_string(settings, "text", "Hello, World!");
	obs_data_set_default_string(settings, "model", "vits-coqui-en-vctk");
	obs_data_set_default_string(settings, "input_source", "none");
	obs_data_set_default_string(settings, "file", "");
	obs_data_set_default_bool(settings, "line_by_line", false);
	obs_data_set_default_bool(settings, "phonetic_transcription", true);
	obs_data_set_default_bool(settings, "input_debounce", true);
	obs_data_set_default_bool(settings, "interrupt_mode", false);
	obs_data_set_default_int(settings, "latency", 50);
}

bool add_sources_to_list(void *list_property, obs_source_t *source)
{
	auto source_id = obs_source_get_id(source);
	if (strcmp(source_id, "text_ft2_source_v2") != 0 &&
	    strcmp(source_id, "text_gdiplus_v2") != 0 &&
	    strcmp(source_id, "text_gdiplus_v3") != 0) {
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
	obs_property_t *model = obs_properties_add_list(
		ppts, "model", MT_("Model"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
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
					// Update will initialize the model on the TTS context
					squawk_data_->tts_context.model_name = "";
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
	obs_properties_add_int(ppts, "speaker_id", MT_("Speaker_ID"), 0, 1000, 1);

	// add a speed slider between 0.1 and 2.5
	obs_properties_add_float_slider(ppts, "speed", MT_("Speed"), 0.1, 2.5, 0.1);

	// add "inputs" group
	obs_properties_t *inputs_group = obs_properties_create();
	obs_properties_add_group(ppts, "inputs", MT_("Inputs"), OBS_GROUP_NORMAL, inputs_group);
	// add input source selection dropdown property
	obs_property_t *input_source = obs_properties_add_list(inputs_group, "input_source",
							       "Input Source", OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
	// Add "none" option
	obs_property_list_add_string(input_source, MT_("none_no_input"), "none");
	// Add text sources
	obs_enum_sources(add_sources_to_list, input_source);
	// add file property
	obs_properties_add_path(inputs_group, "file", MT_("File"), OBS_PATH_FILE, nullptr, nullptr);
	// add line-by-line boolean property
	obs_property_t *lbl_prop =
		obs_properties_add_bool(inputs_group, "line_by_line", MT_("Line_By_Line"));
	// add help text for line-by-line
	obs_property_set_long_description(lbl_prop, MT_("line_by_line_help"));
	// add boolean property for enabling input debounce
	obs_property_t *debouce_prop =
		obs_properties_add_bool(inputs_group, "input_debounce", MT_("Input_Debounce"));
	// add help text for input debounce
	obs_property_set_long_description(debouce_prop, MT_("input_debounce_help"));

	// add text property
	obs_properties_add_text(ppts, "text", MT_("Text"), OBS_TEXT_DEFAULT);

	// add button property for generating audio on demand
	obs_properties_add_button(
		ppts, "generate_audio", MT_("Generate_Audio"),
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

			std::thread audio_gen_thread([squawk_data_, text, speaker_id]() {
				generate_audio_from_text(squawk_data_->tts_context, text,
							 speaker_id, squawk_data_->speed);
			});
			audio_gen_thread.detach();

			return true;
		});

	// add advanced settings group
	obs_properties_t *advanced_group = obs_properties_create();
	obs_properties_add_group(ppts, "advanced", MT_("Advanced"), OBS_GROUP_NORMAL,
				 advanced_group);

	// add boolean propery for enabling phonetic transcription
	obs_properties_add_bool(advanced_group, "phonetic_transcription",
				MT_("Phonetic_Transcription"));
	// add info desxription for phonetic transcription
	obs_property_set_long_description(obs_properties_get(advanced_group,
							     "phonetic_transcription"),
					  MT_("phonetic_transcription_help"));

	// add boolean property for enabling interrupt mode
	obs_properties_add_bool(advanced_group, "interrupt_mode", MT_("Interrupt_Mode"));
	// add info description for interrupt mode
	obs_property_set_long_description(obs_properties_get(advanced_group, "interrupt_mode"),
					  MT_("interrupt_mode_help"));

	// add int slider for setting the latency
	obs_properties_add_int_slider(advanced_group, "latency", MT_("Latency"), 10, 250, 10);
	obs_property_set_long_description(obs_properties_get(advanced_group, "latency"),
					  MT_("latency_help"));

	// add button for deleting all cached models
	obs_properties_add_button(
		advanced_group, "delete_models", MT_("Delete_Cached_Models"),
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

	// add plugin info
	char small_info[256];
	snprintf(small_info, sizeof(small_info), PLUGIN_INFO_TEMPLATE, PLUGIN_VERSION);
	obs_properties_add_text(ppts, "plugin_info", small_info, OBS_TEXT_INFO);

	return ppts;
}

void squawk_source_update(void *data, obs_data_t *settings)
{
	obs_log(LOG_DEBUG, "Squawk source update");

	squawk_source_data *squawk_data = (squawk_source_data *)data;

	squawk_data->speaker_id = (uint32_t)obs_data_get_int(settings, "speaker_id");
	squawk_data->speed = (float)obs_data_get_double(settings, "speed");
	squawk_data->phonetic_transcription = obs_data_get_bool(settings, "phonetic_transcription");

	std::string source = obs_data_get_string(settings, "input_source");
	if (source == "none") {
		source = "";
	}
	squawk_data->inputThread->setOBSTextSource(source);
	squawk_data->inputThread->setFile(obs_data_get_string(settings, "file"));
	squawk_data->inputThread->setReadingMode(obs_data_get_bool(settings, "line_by_line")
							 ? ReadingMode::LineByLine
							 : ReadingMode::Whole);
	squawk_data->inputThread->setDebounceMode(obs_data_get_bool(settings, "input_debounce")
							  ? DebouceMode::Debounced
							  : DebouceMode::Immediate);

	std::string new_model_name = obs_data_get_string(settings, "model");
	if (new_model_name != squawk_data->tts_context.model_name) {
		destroy_sherpa_tts_context(squawk_data->tts_context);
		// find model info by name
		squawk_data->tts_context.model_name = new_model_name;
		// init the tts context
		init_sherpa_tts_context(squawk_data->tts_context, audio_samples_callback,
					squawk_data);
	}

	squawk_data->audioThread->setTargetBatchSizeMs((int)obs_data_get_int(settings, "latency"));
	squawk_data->audioThread->setInterruptMode(obs_data_get_bool(settings, "interrupt_mode"));
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
