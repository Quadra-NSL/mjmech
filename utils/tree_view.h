// Copyright 2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/date_time/posix_time/posix_time_types.hpp>

#include <imgui.h>

#include "mjlib/base/buffer_stream.h"
#include "mjlib/base/fail.h"
#include "mjlib/micro/serializable_handler.h"
#include "mjlib/telemetry/file_reader.h"

#include "utils/imgui_tree_archive.h"

#include "gl/gl_imgui.h"

namespace mjmech {
namespace utils {

/// Log data at an instance in time.
struct CurrentLogData {
  std::optional<std::string> get(std::string_view name) const {
    const auto it = names.find(std::string(name));
    if (it == names.end()) { return {}; }
    const auto it2 = data.find(it->second);
    if (it2 == data.end()) { return {}; }
    return it2->second;
  }

  std::map<const mjlib::telemetry::FileReader::Record*, std::string> data;
  std::map<std::string, const mjlib::telemetry::FileReader::Record*> names;
};

class TreeView {
 public:
  using FileReader = mjlib::telemetry::FileReader;
  using Element = mjlib::telemetry::BinarySchemaParser::Element;
  using Format = mjlib::telemetry::Format;
  using FT = Format::Type;

  TreeView(FileReader* reader, boost::posix_time::ptime log_start)
      : reader_(reader),
        log_start_(log_start) {
    for (const auto* record : reader->records()) {
      data_.names.insert(std::make_pair(record->name, record));
    }
  }

  template <typename DerivedOperator>
  void AddDerived(std::string_view name, DerivedOperator derived_operator) {
    derived_.push_back(std::make_unique<Concrete<DerivedOperator>>(
                           name, std::move(derived_operator)));
  }

  std::optional<std::string> data(const std::string& name) {
    return data_.get(name);
  }

  void Update(boost::posix_time::ptime timestamp) {
    if (last_timestamp_.is_not_a_date_time() ||
        timestamp < last_timestamp_ ||
        (timestamp - last_timestamp_) > boost::posix_time::seconds(1)) {
      Seek(timestamp);
    } else {
      Step(timestamp);
    }
    last_timestamp_ = timestamp;

    Render();
  }

  struct Parent {
    const Parent* parent = nullptr;
    const Element* element = nullptr;
    int64_t array_index = -1;

    std::string RenderToken(const Element* child) const {
      const std::string this_token =
          array_index >= 0 ? fmt::format("{}", array_index) : child->name;
      return (parent ? (parent->RenderToken(element) + ".") : "") + this_token;
    }
  };

  void Render() {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 620), ImGuiCond_FirstUseEver);
    gl::ImGuiWindow file_window("Data");

    std::vector<const FileReader::Record*> records;
    for (const auto& pair : data_.data) { records.push_back(pair.first); }
    std::sort(records.begin(), records.end(), [](auto lhs, auto rhs) {
        return lhs->name < rhs->name;
      });

    for (auto record : records) {
      const auto data = data_.data.at(record);
      ImGui::Columns(2, nullptr, true);
      mjlib::base::BufferReadStream stream{data};
      Parent parent;
      VisitElement(record->schema->root(), stream, &parent,
                   data.empty() ? kEmpty : kData);
      ImGui::Columns(1);
    }
    VisitDerived();
  }

  void VisitDerived() {
    ImGui::Columns(2);
    const bool expanded = ImGui::TreeNode("Derived");
    ImGui::NextColumn();
    ImGui::NextColumn();

    if (expanded) {
      for (const auto& derived : derived_) {
        derived->Visit(data_);
      }
      ImGui::TreePop();
    }
    ImGui::Columns(1);
  }

  static bool ElementHasChildren(const Element* element) {
    return (!element->children.empty() || !element->fields.empty()) &&
        (element->type != FT::kEnum);
  }

  enum VisitEmpty {
    kEmpty,
    kData,
  };

  void VisitElement(const Element* element,
                    mjlib::base::ReadStream& stream,
                    const Parent* parent,
                    VisitEmpty empty,
                    const char* name_override = nullptr) {
    // Union types we just forward through to the appropriate typed
    // child.
    if (element->type == FT::kUnion) {
      if (empty == kData) {
        const auto index = element->ReadUnionIndex(stream);
        VisitElement(element->children[index], stream, parent, empty);
        return;
      } else {
        // Do the first non-null child.
        const auto* first_non_null = [&]() -> const Element* {
          for (const auto* child : element->children) {
            if (child->type != FT::kNull) { return child; }
          }
          return nullptr;
        }();
        if (first_non_null) { return; }  // ???
        return VisitElement(element, stream, parent, empty, name_override);
      }
    }

    const bool children = ElementHasChildren(element);

    int flags = 0;
    if (!children) {
      flags |= ImGuiTreeNodeFlags_Leaf;
    }
    const bool expanded = ImGui::TreeNodeEx(
        element, flags, "%s",
        name_override ? name_override : element->name.c_str());

    if (!children && ImGui::BeginDragDropSource()) {
      const std::string token = parent->RenderToken(element);
      ImGui::SetDragDropPayload("DND_TLOG", token.data(), token.size());
      ImGui::TextUnformatted(token.c_str());
      ImGui::EndDragDropSource();
    }

    ImGui::NextColumn();

    // Read the scalar data to display.
    const auto value = [&]() -> std::string {
      if (empty == kEmpty) { return ""; }
      switch (element->type) {
        case FT::kBoolean: {
          return element->ReadBoolean(stream) ? "true" : "false";
        }
        case FT::kFixedInt:
        case FT::kVarint: {
          return fmt::format("{}", element->ReadIntLike(stream));
        }
        case FT::kFixedUInt:
        case FT::kVaruint: {
          return fmt::format("{}", element->ReadUIntLike(stream));
        }
        case FT::kFloat32:
        case FT::kFloat64: {
          return fmt::format("{}", element->ReadFloatLike(stream));
        }
        case FT::kBytes: {
          return fmt::format("b'{}'", element->ReadString(stream));
        }
        case FT::kString: {
          return element->ReadString(stream);
        }
        case FT::kTimestamp:
        case FT::kDuration: {
          // TODO: Optionally (or always) display calendar time.
          return fmt::format(
              "{:.3f}", element->ReadIntLike(stream) / 1000000.0);
        }
        case FT::kEnum: {
          const auto code = element->children.front()->ReadUIntLike(stream);
          const auto it = element->enum_items.find(code);
          if (it != element->enum_items.end()) { return it->second; }
          return fmt::format("{}", code);
        }
        case FT::kFinal:
        case FT::kNull:
        case FT::kObject:
        case FT::kArray:
        case FT::kFixedArray:
        case FT::kMap:
        case FT::kUnion: {
          break;
        }
      }
      return "";
    }();

    ImGui::Text("%s", value.c_str());
    ImGui::NextColumn();

    auto do_array = [&](uint64_t nelements) {
      for (uint64_t i = 0; i < nelements; i++) {
        ImGui::PushID(i);
        Parent new_parent;
        new_parent.parent = parent;
        new_parent.element = element;
        new_parent.array_index = i;
        VisitElement(element->children.front(), stream, &new_parent, empty,
                     fmt::format("{}", i).c_str());
        ImGui::PopID();
      }
    };

    if (expanded) {
      switch (element->type) {
        case FT::kObject: {
          for (const auto& field : element->fields) {
            Parent new_parent;
            new_parent.parent = parent;
            new_parent.element = element;
            VisitElement(field.element, stream, &new_parent, empty);
          }
          break;
        }
        case FT::kArray: {
          if (empty == kEmpty) {
            do_array(1);
          } else {
            const auto nelements = element->ReadArraySize(stream);
            do_array(nelements);
          }
          break;
        }
        case FT::kFixedArray: {
          do_array(element->array_size);
          break;
        }
        case FT::kMap: {
          // TODO
          if (empty != kEmpty) {
            element->Ignore(stream);
          }
          break;
        }
        case FT::kUnion: {
          mjlib::base::AssertNotReached();
        }
        default: {
          break;
        }
      }
      ImGui::TreePop();
    } else {
      // We still need to skip any children to keep our stream
      // consistent.
      switch (element->type) {
        case FT::kObject:
        case FT::kArray:
        case FT::kFixedArray:
        case FT::kMap: {
          if (empty != kEmpty) {
            element->Ignore(stream);
          }
          break;
        }
        case FT::kUnion: {
          mjlib::base::AssertNotReached();
        }
        default: {
          break;
        }
      }
    }
  }

  void Seek(boost::posix_time::ptime timestamp) {
    data_.data = {};
    for (auto record : reader_->records()) { data_.data[record] = ""; }
    last_index_ = {};

    const auto records = reader_->Seek(timestamp);
    for (const auto& pair : records) {
      const auto item = (*reader_->items([&]() {
          FileReader::ItemsOptions options;
          options.start = pair.second;
          return options;
        }()).begin());
      if (item.index > last_index_) { last_index_ = item.index; }
      data_.data[pair.first] = item.data;
    }
  }

  void Step(boost::posix_time::ptime timestamp) {
    // We are some small distance into the future from our last
    // operation.  Step until we get there.
    auto items = reader_->items([&]() {
        FileReader::ItemsOptions options;
        options.start = last_index_;
        return options;
      }());
    for (const auto& item : items) {
      if (item.timestamp > timestamp) {
        // We're done!
        break;
      }
      if (item.index > last_index_) { last_index_ = item.index; }
      data_.data[item.record] = item.data;
    }
  }

  struct ExtractResult {
    std::vector<boost::posix_time::ptime> timestamps;
    std::vector<double> xvals;
    std::vector<double> yvals;
  };

  ExtractResult ExtractDeriv(const std::string& token) {
    mjlib::base::Tokenizer tokenizer(token, ".");
    auto prefix = tokenizer.next();
    for (const auto& derived : derived_) {
      if (derived->name() != prefix) { continue; }

      ExtractResult result;
      auto current_data = data_;
      current_data.data = {};
      for (const auto& item : reader_->items()) {
        current_data.data[item.record] = item.data;
        result.timestamps.push_back(item.timestamp);
        result.xvals.push_back(
            mjlib::base::ConvertDurationToSeconds(item.timestamp - log_start_));
        result.yvals.push_back(
            derived->Extract(tokenizer.remaining(), current_data));
      }
      return result;
    }
    return {};
  }

 private:
  class DerivedBase {
   public:
    virtual ~DerivedBase() {}
    virtual std::string_view name() const = 0;

    virtual void Visit(const CurrentLogData&) const = 0;
    virtual double Extract(std::string_view token,
                           const CurrentLogData& log_data) const = 0;
  };

  class StringStream : public mjlib::micro::AsyncWriteStream {
   public:
    void AsyncWriteSome(const std::string_view& data,
                        const mjlib::micro::SizeCallback& cbk) {
      ostr_.write(data.data(), data.size());
      cbk({}, data.size());
    }

    std::string str() const { return ostr_.str(); }

   private:
    std::ostringstream ostr_;
  };

  template <typename DerivedOperator>
  class Concrete : public DerivedBase {
   public:
    Concrete(std::string_view name, DerivedOperator derived_operator) :
        name_(name),
        derived_operator_(std::move(derived_operator)) {}

    std::string_view name() const override { return name_; }

    void Visit(const CurrentLogData& log_data) const override {
      auto result = derived_operator_(log_data);
      const bool expanded = ImGui::TreeNode(name_.c_str());
      ImGui::NextColumn();
      ImGui::NextColumn();

      if (expanded) {
        ImGuiTreeArchive(name_ + ".").Accept(&result);
        ImGui::TreePop();
      }
    }

    double Extract(std::string_view token,
                   const CurrentLogData& log_data) const override {
      auto result = derived_operator_(log_data);
      char buf[256] = {};
      StringStream streambuf;
      mjlib::micro::SerializableHandler handler(&result);
      handler.Read(token, buf, streambuf, [](auto&&) {});
      return std::stod(streambuf.str());
    };

    const std::string name_;
    DerivedOperator derived_operator_;
  };

  FileReader* const reader_;
  boost::posix_time::ptime log_start_;
  boost::posix_time::ptime last_timestamp_;
  FileReader::Index last_index_ = {};
  CurrentLogData data_;

  std::vector<std::unique_ptr<DerivedBase>> derived_;
};


}
}
