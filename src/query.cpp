#include "hoshidicts/query.hpp"

#include <ankerl/unordered_dense.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <zstd.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string_view>
#include <utf8.h>

#include "hash/hash.hpp"
#include "json/yomitan_parser.hpp"

namespace {
std::pair<void*, size_t> map_file(const std::string& path) {
#ifdef _WIN32
  HANDLE file =
      CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }

  LARGE_INTEGER file_size;
  if (!GetFileSizeEx(file, &file_size)) {
    CloseHandle(file);
    return {};
  }

  HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  CloseHandle(file);
  if (!mapping) {
    return {};
  }

  void* data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);
  if (!data) {
    return {};
  }

  return {data, static_cast<size_t>(file_size.QuadPart)};
#else
  int fd = open(path.c_str(), O_RDONLY);
  if (fd == -1) {
    return {};
  }

  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    return {};
  }

  void* data = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (data == MAP_FAILED) {
    return {};
  }

  return {data, static_cast<size_t>(st.st_size)};
#endif
}

void unmap_file(void* data, size_t size) {
  if (!data) {
    return;
  }
#ifdef _WIN32
  UnmapViewOfFile(data);
#else
  munmap(data, size);
#endif
}

uint8_t read_u8(const uint8_t*& addr) { return *addr++; }

uint16_t read_u16(const uint8_t*& addr) {
  uint16_t result;
  std::memcpy(&result, addr, sizeof(uint16_t));
  addr += sizeof(uint16_t);
  return result;
}

uint32_t read_u32(const uint8_t*& addr) {
  uint32_t result;
  std::memcpy(&result, addr, sizeof(uint32_t));
  addr += sizeof(uint32_t);
  return result;
}

int32_t read_i32(const uint8_t*& addr) {
  int32_t result;
  std::memcpy(&result, addr, sizeof(int32_t));
  addr += sizeof(int32_t);
  return result;
}

uint64_t read_u64(const uint8_t*& addr) {
  uint64_t result;
  std::memcpy(&result, addr, sizeof(uint64_t));
  addr += sizeof(uint64_t);
  return result;
}

std::string_view read_str(const uint8_t*& addr, uint32_t len) {
  std::string_view result(reinterpret_cast<const char*>(addr), len);
  addr += len;
  return result;
}

int query_match_priority(const TermResult& term, const std::string& expression) {
  if (term.expression == expression) {
    return 0;
  }
  if (term.reading == expression) {
    return 1;
  }
  return 2;
}

bool has_storage_marker(const std::string& path) {
  return std::filesystem::is_regular_file(path + "/.hoshidicts_1") ||
         std::filesystem::is_regular_file(path + "/.hoshidicts_2");
}

int detect_storage_version(const std::string& path) {
  if (std::filesystem::is_regular_file(path + "/.hoshidicts_2")) {
    return 2;
  }
  if (std::filesystem::is_regular_file(path + "/.hoshidicts_1")) {
    return 1;
  }
  return 0;
}

bool is_kana_codepoint(uint32_t codepoint) {
  return (codepoint >= 0x3040 && codepoint <= 0x309f) || (codepoint >= 0x30a0 && codepoint <= 0x30ff) ||
         (codepoint >= 0xff66 && codepoint <= 0xff9f);
}

bool is_kanji_codepoint(uint32_t codepoint) {
  return (codepoint >= 0x3400 && codepoint <= 0x4dbf) || (codepoint >= 0x4e00 && codepoint <= 0x9fff) ||
         (codepoint >= 0xf900 && codepoint <= 0xfaff);
}

bool is_kana_only(std::string_view text) {
  if (text.empty()) {
    return false;
  }

  auto it = text.begin();
  while (it != text.end()) {
    const uint32_t codepoint = utf8::next(it, text.end());
    if (!is_kana_codepoint(codepoint)) {
      return false;
    }
  }

  return true;
}

bool contains_kanji(std::string_view text) {
  auto it = text.begin();
  while (it != text.end()) {
    if (is_kanji_codepoint(utf8::next(it, text.end()))) {
      return true;
    }
  }

  return false;
}

int get_freq_value_for_dict(const TermResult& term, const std::string& dict_name) {
  for (const auto& frequency_entry : term.frequencies) {
    if (frequency_entry.dict_name != dict_name) {
      continue;
    }

    int min_frequency = INT_MAX;
    for (const auto& frequency : frequency_entry.frequencies) {
      if (frequency.value >= 0) {
        min_frequency = std::min(min_frequency, frequency.value);
      }
    }
    return min_frequency;
  }

  return INT_MAX;
}

bool freq_sort_order(const TermResult& a, const TermResult& b, const std::vector<std::string>& freq_dict_order) {
  for (const auto& dict_name : freq_dict_order) {
    const int freq_a = get_freq_value_for_dict(a, dict_name);
    const int freq_b = get_freq_value_for_dict(b, dict_name);
    if (freq_a != freq_b) {
      return freq_a < freq_b;
    }
  }

  return false;
}

constexpr size_t kHashTableHeaderSize = sizeof(uint32_t);
constexpr size_t kHashTableSlotSize = sizeof(uint64_t) * 2;
constexpr size_t kOffsetIndexHeaderSize = sizeof(uint32_t);
constexpr size_t kOffsetIndexEntrySize = sizeof(uint64_t);
constexpr uint8_t kMetaEntryType = 1;

bool has_space(const uint8_t* current, const uint8_t* end, size_t required_size) {
  return current <= end && static_cast<size_t>(end - current) >= required_size;
}

bool meta_blob_matches_mode(const uint8_t* blob_addr, const uint8_t* blobs_end, std::string_view mode) {
  const uint8_t* cursor = blob_addr;
  if (!has_space(cursor, blobs_end, sizeof(uint8_t))) {
    return false;
  }

  const uint8_t type = read_u8(cursor);
  if (type != kMetaEntryType) {
    return false;
  }

  if (!has_space(cursor, blobs_end, sizeof(uint16_t))) {
    return false;
  }
  const uint16_t expression_len = read_u16(cursor);
  if (!has_space(cursor, blobs_end, expression_len)) {
    return false;
  }
  cursor += expression_len;

  if (!has_space(cursor, blobs_end, sizeof(uint8_t))) {
    return false;
  }
  const uint8_t mode_len = read_u8(cursor);
  if (!has_space(cursor, blobs_end, mode_len)) {
    return false;
  }

  if (mode_len != mode.size()) {
    return false;
  }

  return std::memcmp(cursor, mode.data(), mode_len) == 0;
}

bool has_meta_mode_entries_in_storage(const uint8_t* hash_table, size_t hash_table_size, const uint8_t* blobs,
                                      size_t blobs_size, std::string_view mode, uint32_t min_count) {
  if (!hash_table || !blobs || mode.empty()) {
    return false;
  }

  const uint8_t* hash_cursor = hash_table;
  const uint8_t* hash_end = hash_table + hash_table_size;
  if (!has_space(hash_cursor, hash_end, kHashTableHeaderSize)) {
    return false;
  }

  const uint32_t capacity = read_u32(hash_cursor);
  const size_t required_hash_table_size =
      kHashTableHeaderSize + static_cast<size_t>(capacity) * kHashTableSlotSize;
  if (capacity == 0 || required_hash_table_size > hash_table_size) {
    return false;
  }

  const uint8_t* blobs_begin = blobs;
  const uint8_t* blobs_end = blobs + blobs_size;

  uint32_t matched_entries = 0;
  for (uint32_t i = 0; i < capacity; ++i) {
    if (!has_space(hash_cursor, hash_end, kHashTableSlotSize)) {
      return false;
    }

    const uint8_t* slot_cursor = hash_cursor;
    const uint64_t hash = read_u64(slot_cursor);
    const uint64_t offset_index_offset = read_u64(slot_cursor);
    hash_cursor += kHashTableSlotSize;

    if (hash == 0 || offset_index_offset == 0 || offset_index_offset >= blobs_size) {
      continue;
    }

    const uint8_t* index_cursor = blobs_begin + offset_index_offset;
    if (!has_space(index_cursor, blobs_end, kOffsetIndexHeaderSize)) {
      continue;
    }
    const uint32_t entry_count = read_u32(index_cursor);
    if (!has_space(index_cursor, blobs_end, static_cast<size_t>(entry_count) * kOffsetIndexEntrySize)) {
      continue;
    }

    for (uint32_t entry = 0; entry < entry_count; ++entry) {
      const uint64_t blob_offset = read_u64(index_cursor);
      if (blob_offset >= blobs_size) {
        continue;
      }

      const uint8_t* blob_addr = blobs_begin + blob_offset;
      if (!meta_blob_matches_mode(blob_addr, blobs_end, mode)) {
        continue;
      }

      matched_entries++;
      if (matched_entries >= min_count) {
        return true;
      }
    }
  }

  return false;
}
}

struct DictionaryQuery::DictionaryData {
  int storage_version = 1;
  hash::linear table;
  uint8_t* blobs = nullptr;
  size_t blobs_size = 0;
  uint8_t* hash_table = nullptr;
  size_t hash_table_size = 0;
  uint8_t* media = nullptr;
  size_t media_size = 0;
  uint8_t* media_index = nullptr;
  size_t media_index_size = 0;

  ~DictionaryData() {
    unmap_file(blobs, blobs_size);
    unmap_file(hash_table, hash_table_size);
    unmap_file(media, media_size);
    unmap_file(media_index, media_index_size);
  }
};

DictionaryQuery::DictionaryQuery() = default;
DictionaryQuery::~DictionaryQuery() = default;

DictionaryQuery::DictionaryQuery(DictionaryQuery&&) noexcept = default;
DictionaryQuery& DictionaryQuery::operator=(DictionaryQuery&&) noexcept = default;

bool DictionaryQuery::has_meta_mode_entries(const std::string& path, const std::string& mode, uint32_t min_count) {
  if (min_count == 0) {
    return true;
  }

  if (mode.empty()) {
    return false;
  }

  if (!has_storage_marker(path)) {
    return false;
  }

  auto [hash_table, hash_table_size] = map_file(path + "/hash.table");
  auto [blobs, blobs_size] = map_file(path + "/blobs.bin");
  if (!hash_table || !blobs) {
    unmap_file(hash_table, hash_table_size);
    unmap_file(blobs, blobs_size);
    return false;
  }

  const bool has_entries = has_meta_mode_entries_in_storage(
      reinterpret_cast<const uint8_t*>(hash_table), hash_table_size, reinterpret_cast<const uint8_t*>(blobs),
      blobs_size, mode, min_count);

  unmap_file(hash_table, hash_table_size);
  unmap_file(blobs, blobs_size);
  return has_entries;
}

void DictionaryQuery::add_dict(const std::string& path, DictionaryType type) {
  const int storage_version = detect_storage_version(path);
  if (storage_version == 0) {
    return;
  }

  Dictionary dict;
  Index index;
  std::string buf{};
  if (glz::read_file_json(index, path + "/index.json", buf)) {
    return;
  }

  dict.name = index.title.empty() ? std::filesystem::path(path).stem().string() : index.title;
  if (std::filesystem::exists(path + "/styles.css")) {
    std::ifstream f(path + "/styles.css");
    dict.styles = std::string(std::istreambuf_iterator<char>(f), {});
  }

  dict.data = std::make_unique<DictionaryData>();
  dict.data->storage_version = storage_version;

  auto [hash_table, hash_table_size] = map_file(path + "/hash.table");
  if (!hash_table) {
    return;
  }
  dict.data->hash_table_size = hash_table_size;
  dict.data->hash_table = reinterpret_cast<uint8_t*>(hash_table);
  dict.data->table.load(hash_table);

  auto [blobs, blobs_size] = map_file(path + "/blobs.bin");
  if (!blobs) {
    return;
  }
  dict.data->blobs_size = blobs_size;
  dict.data->blobs = reinterpret_cast<uint8_t*>(blobs);

  auto [media, media_size] = map_file(path + "/media.bin");
  if (media) {
    dict.data->media_size = media_size;
    dict.data->media = reinterpret_cast<uint8_t*>(media);

    auto [media_index, media_index_size] = map_file(path + "/media.idx");
    if (media_index) {
      dict.data->media_index_size = media_index_size;
      dict.data->media_index = reinterpret_cast<uint8_t*>(media_index);
    }
  }

  switch (type) {
    case TERM:
      term_dicts_.push_back(std::move(dict));
      break;
    case FREQ:
      freq_dicts_.push_back(std::move(dict));
      break;
    case PITCH:
      pitch_dicts_.push_back(std::move(dict));
      break;
  }
}

void DictionaryQuery::add_term_dict(const std::string& path) { add_dict(path, DictionaryQuery::DictionaryType::TERM); }

void DictionaryQuery::add_freq_dict(const std::string& path) { add_dict(path, DictionaryQuery::DictionaryType::FREQ); }

void DictionaryQuery::add_pitch_dict(const std::string& path) {
  add_dict(path, DictionaryQuery::DictionaryType::PITCH);
}

std::vector<TermResult> DictionaryQuery::query(const std::string& expression) const {
  std::map<std::pair<std::string_view, std::string_view>, TermResult> term_map;
  for (const auto& [name, styles, data] : term_dicts_) {
    uint64_t offset_addr = data->table(expression);
    if (offset_addr == 0) {
      continue;
    }
    const uint8_t* index_addr = data->blobs + offset_addr;

    uint32_t count = read_u32(index_addr);
    for (uint32_t i = 0; i < count; i++) {
      uint64_t offset = read_u64(index_addr);
      const uint8_t* blob_addr = data->blobs + offset;

      // first byte encodes term (0) or meta (1) entry
      uint8_t type = read_u8(blob_addr);
      if (type != 0) {
        continue;
      }

      uint16_t expr_len = read_u16(blob_addr);
      std::string_view expr = read_str(blob_addr, expr_len);

      uint16_t reading_len = read_u16(blob_addr);
      std::string_view reading = read_str(blob_addr, reading_len);
      const int score = data->storage_version >= 2 ? read_i32(blob_addr) : 0;

      if (expr != expression && reading != expression) {
        continue;
      }

      uint64_t glossary_offset = read_u64(blob_addr);
      uint32_t glossary_size = read_u32(blob_addr);
      std::string glossary = decompress_glossary(data->blobs + glossary_offset, glossary_size);

      uint8_t def_tags_size = read_u8(blob_addr);
      std::string_view definition_tags = read_str(blob_addr, def_tags_size);

      uint8_t rules_size = read_u8(blob_addr);
      std::string_view rules = read_str(blob_addr, rules_size);

      uint8_t term_tag_size = read_u8(blob_addr);
      std::string_view term_tags = read_str(blob_addr, term_tag_size);

      GlossaryEntry entry;
      entry.dict_name = name;
      entry.definition_tags = definition_tags;
      entry.term_tags = term_tags;
      entry.glossary = glossary;

      auto [it, inserted] = term_map.try_emplace({expr, reading});
      if (inserted) {
        it->second = {.expression = std::string(expr),
                      .reading = std::string(reading),
                      .rules = std::string(rules),
                      .score = score,
                      .glossaries = {},
                      .frequencies = {},
                      .pitches = {}};
      } else {
        if (!rules.empty()) {
          if (!it->second.rules.empty()) {
            it->second.rules += " ";
          }
          it->second.rules += rules;
        }
      }
      it->second.glossaries.push_back(std::move(entry));
    }
  }

  std::vector<TermResult> results;
  results.reserve(term_map.size());
  for (auto& [key, value] : term_map) {
    results.push_back(std::move(value));
  }
  query_freq(results);
  query_pitch(results);

  const auto freq_dict_order = get_freq_dict_order();
  std::ranges::stable_sort(results, [&expression, &freq_dict_order](const TermResult& a, const TermResult& b) {
    const int match_a = query_match_priority(a, expression);
    const int match_b = query_match_priority(b, expression);
    if (match_a != match_b) {
      return match_a < match_b;
    }

    if (is_kana_only(expression)) {
      const bool kana_expr_a = is_kana_only(a.expression);
      const bool kana_expr_b = is_kana_only(b.expression);
      if (kana_expr_a != kana_expr_b) {
        return kana_expr_a;
      }
    }

    if (contains_kanji(expression) && a.score != b.score) {
      return a.score > b.score;
    }

    if (freq_sort_order(a, b, freq_dict_order)) {
      return true;
    }
    if (freq_sort_order(b, a, freq_dict_order)) {
      return false;
    }

    if (a.expression != b.expression) {
      return a.expression < b.expression;
    }
    return a.reading < b.reading;
  });

  return results;
}

void DictionaryQuery::query_freq(std::vector<TermResult>& terms) const {
  for (auto& term : terms) {
    for (const auto& [name, styles, data] : freq_dicts_) {
      uint64_t offset_addr = data->table(term.expression);
      if (offset_addr == 0) {
        continue;
      }
      const uint8_t* index_addr = data->blobs + offset_addr;
      uint32_t count = read_u32(index_addr);

      std::vector<Frequency> frequencies;
      for (uint32_t i = 0; i < count; i++) {
        uint64_t offset = read_u64(index_addr);
        const uint8_t* blob_addr = data->blobs + offset;

        uint8_t type = read_u8(blob_addr);
        if (type != 1) {
          continue;
        }

        uint16_t expr_len = read_u16(blob_addr);
        std::string_view expr = read_str(blob_addr, expr_len);
        if (expr != term.expression) {
          continue;
        }

        uint8_t mode_len = read_u8(blob_addr);
        std::string_view mode = read_str(blob_addr, mode_len);
        if (mode != "freq") {
          continue;
        }

        uint32_t freq_data_size = read_u32(blob_addr);
        std::string_view freq_data = read_str(blob_addr, freq_data_size);

        ParsedFrequency parsed;
        if (yomitan_parser::parse_frequency(freq_data, parsed)) {
          if (!parsed.reading.empty() && parsed.reading != term.reading) {
            continue;
          }
          frequencies.emplace_back(
              Frequency{.value = parsed.value, .display_value = std::string(parsed.display_value)});
        }
      }
      if (!frequencies.empty()) {
        term.frequencies.emplace_back(FrequencyEntry{.dict_name = name, .frequencies = std::move(frequencies)});
      }
    }
  }
}

void DictionaryQuery::query_pitch(std::vector<TermResult>& terms) const {
  for (auto& term : terms) {
    for (const auto& [name, styles, data] : pitch_dicts_) {
      uint64_t offset_addr = data->table(term.expression);
      if (offset_addr == 0) {
        continue;
      }
      const uint8_t* index_addr = data->blobs + offset_addr;
      uint32_t count = read_u32(index_addr);

      std::vector<int> pitch_positions;
      for (uint32_t i = 0; i < count; i++) {
        uint64_t offset = read_u64(index_addr);
        const uint8_t* blob_addr = data->blobs + offset;

        uint8_t type = read_u8(blob_addr);
        if (type != 1) {
          continue;
        }

        uint16_t expr_len = read_u16(blob_addr);
        std::string_view expr = read_str(blob_addr, expr_len);
        if (expr != term.expression) {
          continue;
        }

        uint8_t mode_len = read_u8(blob_addr);
        std::string_view mode = read_str(blob_addr, mode_len);
        if (mode != "pitch") {
          continue;
        }

        uint32_t pitch_data_size = read_u32(blob_addr);
        std::string_view pitch_data = read_str(blob_addr, pitch_data_size);

        ParsedPitch parsed;
        if (yomitan_parser::parse_pitch(pitch_data, parsed)) {
          if (!parsed.reading.empty() && parsed.reading != term.reading) {
            continue;
          }
          pitch_positions.insert(pitch_positions.end(), parsed.pitches.begin(), parsed.pitches.end());
        }
      }
      if (!pitch_positions.empty()) {
        term.pitches.emplace_back(PitchEntry{.dict_name = name, .pitch_positions = std::move(pitch_positions)});
      }
    }
  }
}

std::string DictionaryQuery::decompress_glossary(const void* data, size_t size) {
  if (!data || size == 0) {
    return "";
  }

  unsigned long long decompressed_size = ZSTD_getFrameContentSize(data, size);
  if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return "";
  }

  std::string result;
  result.resize(decompressed_size);

  size_t actual_size = ZSTD_decompress(result.data(), result.size(), data, size);
  if (ZSTD_isError(actual_size)) {
    return "";
  }

  result.resize(actual_size);
  return result;
}

std::vector<char> DictionaryQuery::get_media_file(const std::string& dict_name, const std::string& media_path) const {
  for (const auto& [name, styles, data] : term_dicts_) {
    if (name != dict_name) {
      continue;
    }

    if (!data->media || !data->media_index) {
      return {};
    }

    const uint8_t* ptr = data->media_index;
    uint32_t count = read_u32(ptr);

    size_t left = 0;
    size_t right = count;
    while (left < right) {
      const size_t mid = left + (right - left) / 2;
      uint64_t record_offset;
      std::memcpy(&record_offset, data->media_index + sizeof(uint32_t) + mid * sizeof(uint64_t), sizeof(uint64_t));

      const uint8_t* record = data->media + record_offset;
      uint16_t path_size = read_u16(record);
      std::string_view indexed_path = read_str(record, path_size);
      if (indexed_path < media_path) {
        left = mid + 1;
      } else if (indexed_path > media_path) {
        right = mid;
      } else {
        uint32_t blob_size = read_u32(record);
        const char* blob_data = reinterpret_cast<const char*>(record);
        return {blob_data, blob_data + blob_size};
      }
    }
    return {};
  }
  return {};
}

std::vector<DictionaryStyle> DictionaryQuery::get_styles() const {
  std::vector<DictionaryStyle> styles;
  styles.reserve(term_dicts_.size());
  for (const auto& dict : term_dicts_) {
    if (!dict.styles.empty()) {
      styles.push_back(DictionaryStyle{dict.name, dict.styles});
    }
  }
  return styles;
}

std::vector<std::string> DictionaryQuery::get_freq_dict_order() const {
  std::vector<std::string> order;
  order.reserve(freq_dicts_.size());
  for (const auto& dict : freq_dicts_) {
    order.push_back(dict.name);
  }
  return order;
}
