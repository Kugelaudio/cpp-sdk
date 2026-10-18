#include "kugelaudio/resources.hpp"

#include <fstream>
#include <iterator>
#include <sstream>

#include "client_core.hpp"
#include "json_mapping.hpp"
#include "kugelaudio/errors.hpp"

namespace kugelaudio {

namespace {

using detail::json;
using detail::QueryParams;

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ValidationError("Cannot read audio file " + path.string() + ".");
  std::ostringstream data;
  data << in.rdbuf();
  if (in.bad()) throw ValidationError("Cannot read audio file " + path.string() + ".");
  return data.str();
}

QueryParams project_query(std::optional<std::int64_t> project_id) {
  if (!project_id) return {};
  return {{"project_id", std::to_string(*project_id)}};
}

std::string id(std::int64_t value) { return std::to_string(value); }

const json& expect_object(const json& body, const std::string& context) {
  if (!body.is_object()) {
    throw KugelAudioError("Unexpected response from KugelAudio (" + context +
                          "): expected a JSON object.");
  }
  return body;
}

}  // namespace

// ------------------------------------------------------------------ models

ModelsResource::ModelsResource(std::shared_ptr<detail::ClientCore> core) : core_(std::move(core)) {}

std::vector<Model> ModelsResource::list() const {
  const json body = expect_object(core_->request("GET", "/v1/models"), "GET /v1/models");
  std::vector<Model> models;
  if (auto it = body.find("models"); it != body.end() && it->is_array()) {
    for (const auto& item : *it) models.push_back(detail::model_from_json(item));
  }
  return models;
}

// ------------------------------------------------------------------ voices

VoicesResource::VoicesResource(std::shared_ptr<detail::ClientCore> core) : core_(std::move(core)) {}

VoiceListResponse VoicesResource::list(const VoiceListOptions& options) const {
  QueryParams query{{"limit", std::to_string(options.limit)},
                    {"offset", std::to_string(options.offset)}};
  if (options.language && !options.language->empty())
    query.emplace_back("language", *options.language);
  if (options.include_public) query.emplace_back("include_public", "true");
  return detail::voice_list_from_json(core_->request("GET", "/v1/voices", query));
}

VoiceDetail VoicesResource::get(std::int64_t voice_id) const {
  return detail::voice_detail_from_json(core_->request("GET", "/v1/voices/" + id(voice_id)));
}

VoiceDetail VoicesResource::create(const VoiceCreateRequest& request) const {
  std::vector<detail::MultipartPart> parts;
  parts.push_back({"metadata", std::nullopt, "application/json",
                   detail::dump_json(detail::voice_create_metadata(request))});
  for (const auto& path : request.reference_files) {
    parts.push_back({"files", path.filename().string(), "audio/wav", read_file(path)});
  }
  return detail::voice_detail_from_json(
      core_->request_multipart("POST", "/v1/voices", std::move(parts)));
}

VoiceDetail VoicesResource::update(std::int64_t voice_id, const VoiceUpdate& update) const {
  return detail::voice_detail_from_json(
      core_->request("PATCH", "/v1/voices/" + id(voice_id), {}, detail::voice_update_body(update)));
}

void VoicesResource::remove(std::int64_t voice_id) const {
  core_->request("DELETE", "/v1/voices/" + id(voice_id));
}

std::vector<VoiceReference> VoicesResource::list_references(std::int64_t voice_id) const {
  const std::string path = "/v1/voices/" + id(voice_id) + "/references";
  const json body = core_->request("GET", path);
  if (!body.is_array()) {
    throw KugelAudioError("Expected a JSON array from GET " + path + ".");
  }
  std::vector<VoiceReference> references;
  for (const auto& item : body) references.push_back(detail::voice_reference_from_json(item));
  return references;
}

VoiceReference VoicesResource::add_reference(std::int64_t voice_id,
                                             const std::filesystem::path& file,
                                             bool speaker_consent,
                                             const std::string& reference_text) const {
  std::vector<detail::MultipartPart> parts;
  parts.push_back({"file", file.filename().string(), "audio/wav", read_file(file)});
  parts.push_back(
      {"speaker_consent", std::nullopt, "text/plain", speaker_consent ? "true" : "false"});
  if (!reference_text.empty()) {
    parts.push_back({"reference_text", std::nullopt, "text/plain", reference_text});
  }
  return detail::voice_reference_from_json(core_->request_multipart(
      "POST", "/v1/voices/" + id(voice_id) + "/references", std::move(parts)));
}

void VoicesResource::remove_reference(std::int64_t voice_id, std::int64_t reference_id) const {
  core_->request("DELETE", "/v1/voices/" + id(voice_id) + "/references/" + id(reference_id));
}

VoiceDetail VoicesResource::publish(std::int64_t voice_id) const {
  return detail::voice_detail_from_json(
      core_->request("POST", "/v1/voices/" + id(voice_id) + "/publish"));
}

GeneratedSample VoicesResource::generate_sample(std::int64_t voice_id) const {
  return detail::generated_sample_from_json(
      core_->request("POST", "/v1/voices/" + id(voice_id) + "/generate-sample"));
}

// ------------------------------------------------------------ dictionaries

DictionaryEntriesResource::DictionaryEntriesResource(std::shared_ptr<detail::ClientCore> core)
    : core_(std::move(core)) {}

DictionaryEntryList DictionaryEntriesResource::list(std::int64_t dictionary_id,
                                                    const DictionaryEntryListOptions& options,
                                                    std::optional<std::int64_t> project_id) const {
  QueryParams query{{"limit", std::to_string(options.limit)},
                    {"offset", std::to_string(options.offset)}};
  if (options.search) query.emplace_back("search", *options.search);
  if (project_id) query.emplace_back("project_id", std::to_string(*project_id));
  return detail::dictionary_entry_list_from_json(
      core_->request("GET", "/v1/dictionaries/" + id(dictionary_id) + "/entries", query));
}

DictionaryEntry DictionaryEntriesResource::add(std::int64_t dictionary_id,
                                               const DictionaryEntryInput& entry,
                                               std::optional<std::int64_t> project_id) const {
  return detail::dictionary_entry_from_json(
      core_->request("POST", "/v1/dictionaries/" + id(dictionary_id) + "/entries",
                     project_query(project_id), detail::entry_input_body(entry)));
}

DictionaryEntry DictionaryEntriesResource::update(std::int64_t dictionary_id, std::int64_t entry_id,
                                                  const DictionaryEntryUpdate& update,
                                                  std::optional<std::int64_t> project_id) const {
  return detail::dictionary_entry_from_json(
      core_->request("PATCH", "/v1/dictionaries/" + id(dictionary_id) + "/entries/" + id(entry_id),
                     project_query(project_id), detail::entry_update_body(update)));
}

void DictionaryEntriesResource::remove(std::int64_t dictionary_id, std::int64_t entry_id,
                                       std::optional<std::int64_t> project_id) const {
  core_->request("DELETE", "/v1/dictionaries/" + id(dictionary_id) + "/entries/" + id(entry_id),
                 project_query(project_id));
}

BulkReplaceResult DictionaryEntriesResource::replace_all(
    std::int64_t dictionary_id, const std::vector<DictionaryEntryInput>& entries,
    std::optional<std::int64_t> project_id) const {
  json items = json::array();
  for (const auto& entry : entries) items.push_back(detail::entry_input_body(entry));
  return detail::bulk_replace_from_json(
      core_->request("PUT", "/v1/dictionaries/" + id(dictionary_id) + "/entries",
                     project_query(project_id), json{{"entries", items}}));
}

DictionariesResource::DictionariesResource(std::shared_ptr<detail::ClientCore> core)
    : core_(core), entries_(core) {}

std::vector<Dictionary> DictionariesResource::list(std::optional<std::int64_t> project_id) const {
  const json body = expect_object(
      core_->request("GET", "/v1/dictionaries", project_query(project_id)), "GET /v1/dictionaries");
  std::vector<Dictionary> dictionaries;
  if (auto it = body.find("dictionaries"); it != body.end() && it->is_array()) {
    for (const auto& item : *it) dictionaries.push_back(detail::dictionary_from_json(item));
  }
  return dictionaries;
}

Dictionary DictionariesResource::create(const DictionaryCreate& request,
                                        std::optional<std::int64_t> project_id) const {
  return detail::dictionary_from_json(core_->request("POST", "/v1/dictionaries",
                                                     project_query(project_id),
                                                     detail::dictionary_create_body(request)));
}

Dictionary DictionariesResource::get(std::int64_t dictionary_id,
                                     std::optional<std::int64_t> project_id) const {
  return detail::dictionary_from_json(
      core_->request("GET", "/v1/dictionaries/" + id(dictionary_id), project_query(project_id)));
}

Dictionary DictionariesResource::update(std::int64_t dictionary_id, const DictionaryUpdate& update,
                                        std::optional<std::int64_t> project_id) const {
  return detail::dictionary_from_json(
      core_->request("PATCH", "/v1/dictionaries/" + id(dictionary_id), project_query(project_id),
                     detail::dictionary_update_body(update)));
}

void DictionariesResource::remove(std::int64_t dictionary_id,
                                  std::optional<std::int64_t> project_id) const {
  core_->request("DELETE", "/v1/dictionaries/" + id(dictionary_id), project_query(project_id));
}

}  // namespace kugelaudio
