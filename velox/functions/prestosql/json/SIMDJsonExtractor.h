/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

#pragma once

#include <string>

#include "folly/Range.h"
#include "folly/dynamic.h"
#include "velox/common/base/Exceptions.h"
#include "velox/functions/prestosql/json/JsonPathTokenizer.h"
#include "velox/functions/prestosql/json/SIMDJsonWrapper.h"
#include "velox/type/StringView.h"

namespace facebook::velox::functions {

template <typename TConsumer>
bool simdJsonExtract(
    const velox::StringView& json,
    const velox::StringView& path,
    TConsumer&& consumer);

namespace detail {

using JsonVector = std::vector<simdjson::ondemand::value>;

class SIMDJsonExtractor {
 public:
  template <typename TConsumer>
  void extract(
      simdjson::ondemand::value& json,
      TConsumer& consumer,
      size_t tokenStartIndex = 0);

  // Returns true if this extractor was initialized with the trivial path "$".
  bool isRootOnlyPath() {
    return tokens_.empty();
  }

  simdjson::ondemand::document parse(const simdjson::padded_string& json);

 private:
  // Use this method to get an instance of SIMDJsonExtractor given a JSON path.
  // Given the nature of the cache, it's important this is only used by
  // simdJsonExtract.
  static SIMDJsonExtractor& getInstance(folly::StringPiece path);

  // Shouldn't instantiate directly - use getInstance().
  explicit SIMDJsonExtractor(const std::string& path) {
    if (!tokenize(path)) {
      VELOX_USER_FAIL("Invalid JSON path: {}", path);
    }
  }

  bool tokenize(const std::string& path);

  // Max number of extractors cached in extractorCache.
  static const uint32_t kMaxCacheSize{32};

  std::vector<std::string> tokens_;

  template <typename TConsumer>
  friend bool facebook::velox::functions::simdJsonExtract(
      const velox::StringView& json,
      const velox::StringView& path,
      TConsumer&& consumer);
};

void extractObject(
    simdjson::ondemand::value& jsonObj,
    const std::string& key,
    std::optional<simdjson::ondemand::value>& ret);

void extractArray(
    simdjson::ondemand::value& jsonValue,
    const std::string& index,
    std::optional<simdjson::ondemand::value>& ret);

template <typename TConsumer>
void SIMDJsonExtractor::extract(
    simdjson::ondemand::value& json,
    TConsumer& consumer,
    size_t tokenStartIndex) {
  simdjson::ondemand::value input = json;
  // Temporary extraction result holder.
  std::optional<simdjson::ondemand::value> result;

  for (int tokenIndex = tokenStartIndex; tokenIndex < tokens_.size();
       tokenIndex++) {
    auto& token = tokens_[tokenIndex];
    if (input.type() == simdjson::ondemand::json_type::object) {
      extractObject(input, token, result);
    } else if (input.type() == simdjson::ondemand::json_type::array) {
      if (token == "*") {
        for (auto child : input.get_array()) {
          if (tokenIndex == tokens_.size() - 1) {
            // If this is the last token in the path, consume each element in
            // the array.
            consumer(child.value());
          } else {
            // If not, then recursively call the extract function on each
            // element in the array.
            extract(child.value(), consumer, tokenIndex + 1);
          }
        }

        return;
      } else {
        extractArray(input, token, result);
      }
    }
    if (!result) {
      return;
    }

    input = result.value();
    result.reset();
  }

  consumer(input);
}
} // namespace detail

/**
 * Extract element(s) from a JSON object using the given path.
 * @param json: A JSON object
 * @param path: Path to locate a JSON object. Following operators are supported.
 *              "$"      Root member of a JSON structure no matter if it's an
 *                       object, an array, or a scalar.
 *              "."      Child operator to get a child object.
 *              "[]"     Subscript operator for array.
 *              "*"      Wildcard for [], get all the elements of an array.
 * @param consumer: Function to consume the extracted elements. Should be able
 *                  to take an argument that can either be a
 *                  simdjson::ondemand::document or a simdjson::ondemand::value.
 *                  Note that once consumer returns, it should be assumed that
 *                  the argument passed in is no longer valid, so do not attempt
 *                  to store it as is in the consumer.
 * @return Return true on success.
 *         If any errors are encountered parsing the JSON, returns false.
 */

template <typename TConsumer>
bool simdJsonExtract(
    const velox::StringView& json,
    const velox::StringView& path,
    TConsumer&& consumer) {
  try {
    // If extractor fails to parse the path, this will throw a VeloxUserError,
    // and we want to let this exception bubble up to the client. We only catch
    // JSON parsing failures (in which cases we return folly::none instead of
    // throw).
    auto& extractor = detail::SIMDJsonExtractor::getInstance(path);
    simdjson::padded_string paddedJson(json.data(), json.size());
    simdjson::ondemand::document jsonDoc = extractor.parse(paddedJson);

    if (extractor.isRootOnlyPath()) {
      // If the path is just to return the original object, call consumer on the
      // document.  Note, we cannot convert this to a value as this is not
      // supported if the object is a scalar.
      consumer(jsonDoc);
    } else {
      auto value = jsonDoc.get_value().value();
      extractor.extract(value, std::forward<TConsumer>(consumer));
    }
  } catch (const simdjson::simdjson_error&) {
    // simdjson might throw a conversion error while parsing the input JSON.
    return false;
  }

  return true;
}

template <typename TConsumer>
bool simdJsonExtract(
    const std::string& json,
    const std::string& path,
    TConsumer&& consumer) {
  return simdJsonExtract(
      velox::StringView(json),
      velox::StringView(path),
      std::forward<TConsumer>(consumer));
}
} // namespace facebook::velox::functions
