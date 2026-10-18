#pragma once

/// @file resources.hpp
/// REST resources: models, voices and pronunciation dictionaries.
/// Reach them through kugelaudio::Client (`client.voices().list()`).
/// Every method is blocking and thread-safe, and throws a KugelAudioError
/// subclass on failure.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kugelaudio/types.hpp"

namespace kugelaudio {

namespace detail {
struct ClientCore;
}

/// `GET /v1/models`.
class ModelsResource {
 public:
  explicit ModelsResource(std::shared_ptr<detail::ClientCore> core);
  /// List the available text-to-speech models.
  std::vector<Model> list() const;

 private:
  std::shared_ptr<detail::ClientCore> core_;
};

/// Voice listing and management (`/v1/voices`).
class VoicesResource {
 public:
  explicit VoicesResource(std::shared_ptr<detail::ClientCore> core);

  /// List voices visible to the API key, one page at a time.
  VoiceListResponse list(const VoiceListOptions& options = {}) const;
  /// Get one voice.
  /// @throws NotFoundError when the voice does not exist or is not visible.
  VoiceDetail get(std::int64_t voice_id) const;
  /// Create a voice from metadata and optional reference recordings.
  /// @throws ValidationError when a reference file cannot be read, or the
  ///         server rejects the request (e.g. missing speaker consent).
  VoiceDetail create(const VoiceCreateRequest& request) const;
  /// Update the fields set in `update`.
  VoiceDetail update(std::int64_t voice_id, const VoiceUpdate& update) const;
  /// Delete a voice (`DELETE /v1/voices/{id}`).
  void remove(std::int64_t voice_id) const;
  /// List the reference recordings of a voice.
  std::vector<VoiceReference> list_references(std::int64_t voice_id) const;
  /// Upload a reference recording.
  /// @param speaker_consent Must be true: you are the speaker or have the
  ///        speaker's permission.
  /// @param reference_text Transcript of the recording (optional).
  VoiceReference add_reference(std::int64_t voice_id, const std::filesystem::path& file,
                               bool speaker_consent, const std::string& reference_text = {}) const;
  /// Delete a reference recording.
  void remove_reference(std::int64_t voice_id, std::int64_t reference_id) const;
  /// Request publication: marks the voice public, pending verification.
  VoiceDetail publish(std::int64_t voice_id) const;
  /// Generate a preview sample from the voice's sample text.
  GeneratedSample generate_sample(std::int64_t voice_id) const;

 private:
  std::shared_ptr<detail::ClientCore> core_;
};

/// Entries of one dictionary (`/v1/dictionaries/{id}/entries`). Every method
/// takes an optional `project_id`, required only for master-key callers.
class DictionaryEntriesResource {
 public:
  explicit DictionaryEntriesResource(std::shared_ptr<detail::ClientCore> core);

  /// List entries, optionally filtered.
  DictionaryEntryList list(std::int64_t dictionary_id,
                           const DictionaryEntryListOptions& options = {},
                           std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Add one entry.
  DictionaryEntry add(std::int64_t dictionary_id, const DictionaryEntryInput& entry,
                      std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Update the fields set in `update`.
  DictionaryEntry update(std::int64_t dictionary_id, std::int64_t entry_id,
                         const DictionaryEntryUpdate& update,
                         std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Delete one entry.
  void remove(std::int64_t dictionary_id, std::int64_t entry_id,
              std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Atomically replace every entry with `entries` (idempotent sync).
  BulkReplaceResult replace_all(std::int64_t dictionary_id,
                                const std::vector<DictionaryEntryInput>& entries,
                                std::optional<std::int64_t> project_id = std::nullopt) const;

 private:
  std::shared_ptr<detail::ClientCore> core_;
};

/// Per-project pronunciation dictionaries (`/v1/dictionaries`). The server
/// invalidates its cache after every change, so the next synthesis uses it.
class DictionariesResource {
 public:
  explicit DictionariesResource(std::shared_ptr<detail::ClientCore> core);

  /// List every dictionary of the project.
  std::vector<Dictionary> list(std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Create a dictionary.
  Dictionary create(const DictionaryCreate& request,
                    std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Get one dictionary.
  Dictionary get(std::int64_t dictionary_id,
                 std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Update the fields set in `update`.
  Dictionary update(std::int64_t dictionary_id, const DictionaryUpdate& update,
                    std::optional<std::int64_t> project_id = std::nullopt) const;
  /// Delete a dictionary and its entries.
  void remove(std::int64_t dictionary_id,
              std::optional<std::int64_t> project_id = std::nullopt) const;
  /// The entries of a dictionary.
  const DictionaryEntriesResource& entries() const noexcept { return entries_; }

 private:
  std::shared_ptr<detail::ClientCore> core_;
  DictionaryEntriesResource entries_;
};

}  // namespace kugelaudio
