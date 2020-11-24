/*
 * Copyright 2020 OmniSci, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ForeignTable.h"
#include <boost/algorithm/string/predicate.hpp>
#include <regex>
#include "DataMgr/ForeignStorage/CsvDataWrapper.h"
#include "DataMgr/ForeignStorage/ParquetDataWrapper.h"
#include "Shared/DateTimeParser.h"

namespace foreign_storage {
ForeignTable::ForeignTable()
    : OptionsContainer({{foreign_storage::ForeignTable::REFRESH_TIMING_TYPE_KEY,
                         foreign_storage::ForeignTable::MANUAL_REFRESH_TIMING_TYPE},
                        {foreign_storage::ForeignTable::REFRESH_UPDATE_TYPE_KEY,
                         foreign_storage::ForeignTable::ALL_REFRESH_UPDATE_TYPE}}) {}

std::vector<std::string_view> ForeignTable::getSupportedDataWrapperOptions() const {
  if (foreign_server->data_wrapper_type == foreign_storage::DataWrapperType::CSV) {
    return foreign_storage::CsvDataWrapper::getSupportedOptions();
  } else if (foreign_server->data_wrapper_type ==
             foreign_storage::DataWrapperType::PARQUET) {
    return foreign_storage::ParquetDataWrapper::getSupportedOptions();
  }
  return {};
}

void ForeignTable::validateOptions() const {
  validateDataWrapperOptions();
  validateRefreshOptions();
}

bool ForeignTable::isAppendMode() const {
  auto update_mode = options.find(REFRESH_UPDATE_TYPE_KEY);
  return (update_mode != options.end() &&
          update_mode->second == APPEND_REFRESH_UPDATE_TYPE);
}

std::string ForeignTable::getFilePath() const {
  auto& server_options = foreign_server->options;
  auto file_path_entry = options.find("FILE_PATH");
  std::string file_path{};
  if (file_path_entry != options.end()) {
    file_path = file_path_entry->second;
  }
  std::string base_path{};
  if (server_options.find(ForeignServer::STORAGE_TYPE_KEY)->second ==
      ForeignServer::LOCAL_FILE_STORAGE_TYPE) {
    auto base_path_entry = server_options.find(ForeignServer::BASE_PATH_KEY);
    if (base_path_entry == server_options.end()) {
      throw std::runtime_error{"No base path found in foreign server options."};
    }
    base_path = base_path_entry->second;
    const std::string separator{boost::filesystem::path::preferred_separator};
    return std::regex_replace(
        base_path + separator + file_path, std::regex{separator + "{2,}"}, separator);
  } else {
    // Just return the file path as a prefix
    return file_path;
  }
}

void ForeignTable::initializeOptions(const rapidjson::Value& options) {
  // Create the options map first because the json version is not guaranteed to be
  // upper-case, which we need to compare reliably with alterable_options.
  auto options_map = create_options_map(options);
  validateSupportedOptions(options_map);
  populateOptionsMap(std::move(options_map));
  validateOptions();
}

// This function can't be static because it needs to know the data wrapper type.
void ForeignTable::validateSupportedOptions(const OptionsMap& options_map) const {
  const auto data_wrapper_options = getSupportedDataWrapperOptions();
  for (const auto& [key, value] : options_map) {
    if (!contains(supported_options, key) && !contains(data_wrapper_options, key)) {
      throw std::runtime_error{"Invalid foreign table option \"" + key + "\"."};
    }
  }
}

void ForeignTable::validateRefreshOptions() const {
  auto update_type_entry =
      options.find(foreign_storage::ForeignTable::REFRESH_UPDATE_TYPE_KEY);
  CHECK(update_type_entry != options.end());
  auto update_type_value = update_type_entry->second;
  if (update_type_value != ALL_REFRESH_UPDATE_TYPE &&
      update_type_value != APPEND_REFRESH_UPDATE_TYPE) {
    std::string error_message =
        "Invalid value \"" + update_type_value + "\" for " + REFRESH_UPDATE_TYPE_KEY +
        " option." + " Value must be \"" + std::string{APPEND_REFRESH_UPDATE_TYPE} +
        "\" or \"" + std::string{ALL_REFRESH_UPDATE_TYPE} + "\".";
    throw std::runtime_error{error_message};
  }

  auto refresh_timing_entry =
      options.find(foreign_storage::ForeignTable::REFRESH_TIMING_TYPE_KEY);
  CHECK(refresh_timing_entry != options.end());
  if (auto refresh_timing_value = refresh_timing_entry->second;
      refresh_timing_value == SCHEDULE_REFRESH_TIMING_TYPE) {
    auto start_date_entry = options.find(REFRESH_START_DATE_TIME_KEY);
    if (start_date_entry == options.end()) {
      throw std::runtime_error{std::string{REFRESH_START_DATE_TIME_KEY} +
                               " option must be provided for scheduled refreshes."};
    }
    auto start_date_time = dateTimeParse<kTIMESTAMP>(start_date_entry->second, 0);
    int64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    if (start_date_time < current_time) {
      throw std::runtime_error{std::string{REFRESH_START_DATE_TIME_KEY} +
                               " cannot be a past date time."};
    }

    auto interval_entry = options.find(REFRESH_INTERVAL_KEY);
    if (interval_entry != options.end()) {
      boost::regex interval_regex{"^\\d{1,}[SHD]$",
                                  boost::regex::extended | boost::regex::icase};
      if (!boost::regex_match(interval_entry->second, interval_regex)) {
        throw std::runtime_error{"Invalid value provided for the " +
                                 std::string{REFRESH_INTERVAL_KEY} + " option."};
      }
    }
  } else if (refresh_timing_value != MANUAL_REFRESH_TIMING_TYPE) {
    throw std::runtime_error{"Invalid value provided for the " +
                             std::string{REFRESH_TIMING_TYPE_KEY} +
                             " option. Value must be \"" + MANUAL_REFRESH_TIMING_TYPE +
                             "\" or \"" + SCHEDULE_REFRESH_TIMING_TYPE + "\"."};
  }
}

void ForeignTable::validateDataWrapperOptions() const {
  const auto wrapper_type = foreign_server->data_wrapper_type;
  if (wrapper_type == foreign_storage::DataWrapperType::CSV) {
    foreign_storage::CsvDataWrapper::validateOptions(this);
  } else if (wrapper_type == foreign_storage::DataWrapperType::PARQUET) {
    foreign_storage::ParquetDataWrapper::validateOptions(this);
  } else {
    UNREACHABLE() << "Unknown data wrapper type";
  }
}

OptionsMap ForeignTable::create_options_map(const rapidjson::Value& json_options) {
  OptionsMap options_map;
  CHECK(json_options.IsObject());
  for (const auto& member : json_options.GetObject()) {
    auto key = to_upper(member.name.GetString());
    if (std::find(upper_case_options.begin(), upper_case_options.end(), key) !=
        upper_case_options.end()) {
      options_map[key] = to_upper(member.value.GetString());
    } else {
      options_map[key] = member.value.GetString();
    }
  }
  return options_map;
}

void ForeignTable::validate_alter_options(const OptionsMap& options_map) {
  for (const auto& [key, value] : options_map) {
    if (!contains(alterable_options, key)) {
      throw std::runtime_error{std::string("Altering foreign table option \"") + key +
                               "\" is not currently supported."};
    }
  }
}

}  // namespace foreign_storage
