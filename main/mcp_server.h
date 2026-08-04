#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#include <string>
#include <atomic>
#include <vector>
#include <map>
#include <functional>
#include <variant>
#include <optional>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <mbedtls/base64.h>

#include <cJSON.h>

#include "checked_cjson.h"

class ImageContent {
private:
    std::string encoded_data_;
    std::string mime_type_;

    static std::string Base64Encode(const std::string& data) {
        size_t dlen = 0, olen = 0;
        mbedtls_base64_encode((unsigned char*)nullptr, 0, &dlen, (const unsigned char*)data.data(), data.size());
        std::string result(dlen, 0);
        mbedtls_base64_encode((unsigned char*)result.data(), result.size(), &olen, (const unsigned char*)data.data(), data.size());
        return result;
    }

public:
    ImageContent(const std::string& mime_type, const std::string& data) {
        mime_type_ = mime_type;
        // base64 encode data
        encoded_data_ = Base64Encode(data);
    }

    std::string to_json() const {
        auto json = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(json.get(), "type", "image");
        CheckedCJsonAddStringToObject(json.get(), "mimeType", mime_type_.c_str());
        CheckedCJsonAddStringToObject(json.get(), "data", encoded_data_.c_str());
        return CheckedCJsonPrint(json.get());
    }
};

// 添加类型别名
using ReturnValue = std::variant<bool, int, std::string, cJSON*, ImageContent*>;

struct PreparedMcpCall {};

// HIL mutations use this envelope so every cJSON allocation and both print
// buffers exist before the side effect. Finish only updates preallocated
// storage and calls cJSON_PrintPreallocated.
class PreparedMcpTextResult {
public:
    PreparedMcpTextResult(
        CheckedCJsonPtr payload,
        std::size_t max_payload_bytes,
        std::size_t max_result_bytes
    ) : payload_(std::move(payload)),
        payload_buffer_(max_payload_bytes),
        result_buffer_(max_result_bytes, '\0'),
        text_capacity_(max_payload_bytes) {
        if (!payload_ || max_payload_bytes < 16 || max_result_bytes < 32) {
            ThrowCJsonAllocationFailure();
        }

        wrapper_ = MakeCheckedCJsonObject();
        auto content = MakeCheckedCJsonArray();
        auto item = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(item.get(), "type", "text");
        const std::string reserved(max_payload_bytes - 1, ' ');
        CheckedCJsonAddStringToObject(item.get(), "text", reserved.c_str());
        text_node_ = cJSON_GetObjectItem(item.get(), "text");
        CheckedCJsonAddItemToArray(content.get(), std::move(item));
        CheckedCJsonAddItemToObject(wrapper_.get(), "content", std::move(content));
        CheckedCJsonAddBoolToObject(wrapper_.get(), "isError", false);
    }

    cJSON* payload() const { return payload_.get(); }

    std::string Finish() {
        if (!cJSON_PrintPreallocated(
                payload_.get(), payload_buffer_.data(),
                static_cast<int>(payload_buffer_.size()), false)) {
            ThrowCJsonAllocationFailure();
        }
        const std::size_t payload_size = std::strlen(payload_buffer_.data());
        if (text_node_ == nullptr || text_node_->valuestring == nullptr ||
            payload_size >= text_capacity_) {
            ThrowCJsonAllocationFailure();
        }
        std::memcpy(text_node_->valuestring, payload_buffer_.data(), payload_size + 1);
        if (!cJSON_PrintPreallocated(
                wrapper_.get(), result_buffer_.data(),
                static_cast<int>(result_buffer_.size()), false)) {
            ThrowCJsonAllocationFailure();
        }
        result_buffer_.resize(std::strlen(result_buffer_.data()));
        return std::move(result_buffer_);
    }

private:
    CheckedCJsonPtr payload_;
    CheckedCJsonPtr wrapper_;
    std::vector<char> payload_buffer_;
    std::string result_buffer_;
    cJSON* text_node_ = nullptr;
    std::size_t text_capacity_ = 0;
};

enum PropertyType {
    kPropertyTypeBoolean,
    kPropertyTypeInteger,
    kPropertyTypeString,
    kPropertyTypeObject
};

class Property {
private:
    std::string name_;
    PropertyType type_;
    std::variant<bool, int, std::string> value_;
    bool has_default_value_;
    std::optional<int> min_value_;  // 新增：整数最小值
    std::optional<int> max_value_;  // 新增：整数最大值

public:
    // Required field constructor
    Property(const std::string& name, PropertyType type)
        : name_(name), type_(type), has_default_value_(false) {}

    // Optional field constructor with default value
    template<typename T>
    Property(const std::string& name, PropertyType type, const T& default_value)
        : name_(name), type_(type), has_default_value_(true) {
        value_ = default_value;
    }

    Property(const std::string& name, PropertyType type, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(false), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
    }

    Property(const std::string& name, PropertyType type, int default_value, int min_value, int max_value)
        : name_(name), type_(type), has_default_value_(true), min_value_(min_value), max_value_(max_value) {
        if (type != kPropertyTypeInteger) {
            throw std::invalid_argument("Range limits only apply to integer properties");
        }
        if (default_value < min_value || default_value > max_value) {
            throw std::invalid_argument("Default value must be within the specified range");
        }
        value_ = default_value;
    }

    inline const std::string& name() const { return name_; }
    inline PropertyType type() const { return type_; }
    inline bool has_default_value() const { return has_default_value_; }
    inline bool has_range() const { return min_value_.has_value() && max_value_.has_value(); }
    inline int min_value() const { return min_value_.value_or(0); }
    inline int max_value() const { return max_value_.value_or(0); }

    template<typename T>
    inline T value() const {
        return std::get<T>(value_);
    }

    template<typename T>
    inline void set_value(const T& value) {
        // 添加对设置的整数值进行范围检查
        if constexpr (std::is_same_v<T, int>) {
            if (min_value_.has_value() && value < min_value_.value()) {
                throw std::invalid_argument("Value is below minimum allowed: " + std::to_string(min_value_.value()));
            }
            if (max_value_.has_value() && value > max_value_.value()) {
                throw std::invalid_argument("Value exceeds maximum allowed: " + std::to_string(max_value_.value()));
            }
        }
        value_ = value;
    }

    std::string to_json() const {
        auto json = MakeCheckedCJsonObject();
        
        if (type_ == kPropertyTypeBoolean) {
            CheckedCJsonAddStringToObject(json.get(), "type", "boolean");
            if (has_default_value_) {
                CheckedCJsonAddBoolToObject(json.get(), "default", value<bool>());
            }
        } else if (type_ == kPropertyTypeInteger) {
            CheckedCJsonAddStringToObject(json.get(), "type", "integer");
            if (has_default_value_) {
                CheckedCJsonAddNumberToObject(json.get(), "default", value<int>());
            }
            if (min_value_.has_value()) {
                CheckedCJsonAddNumberToObject(json.get(), "minimum", min_value_.value());
            }
            if (max_value_.has_value()) {
                CheckedCJsonAddNumberToObject(json.get(), "maximum", max_value_.value());
            }
        } else if (type_ == kPropertyTypeString) {
            CheckedCJsonAddStringToObject(json.get(), "type", "string");
            if (has_default_value_) {
                CheckedCJsonAddStringToObject(
                    json.get(), "default", value<std::string>().c_str());
            }
        } else if (type_ == kPropertyTypeObject) {
            CheckedCJsonAddStringToObject(json.get(), "type", "object");
        }

        return CheckedCJsonPrint(json.get());
    }
};

class PropertyList {
private:
    std::vector<Property> properties_;

public:
    PropertyList() = default;
    PropertyList(const std::vector<Property>& properties) : properties_(properties) {}
    void AddProperty(const Property& property) {
        properties_.push_back(property);
    }

    const Property& operator[](const std::string& name) const {
        for (const auto& property : properties_) {
            if (property.name() == name) {
                return property;
            }
        }
        throw std::runtime_error("Property not found: " + name);
    }

    auto begin() { return properties_.begin(); }
    auto end() { return properties_.end(); }

    std::vector<std::string> GetRequired() const {
        std::vector<std::string> required;
        for (auto& property : properties_) {
            if (!property.has_default_value()) {
                required.push_back(property.name());
            }
        }
        return required;
    }

    std::string to_json() const {
        auto json = MakeCheckedCJsonObject();
        
        for (const auto& property : properties_) {
            const std::string property_json = property.to_json();
            auto parsed_property = ParseCheckedCJson(property_json.c_str());
            CheckedCJsonAddItemToObject(
                json.get(), property.name().c_str(), std::move(parsed_property));
        }

        return CheckedCJsonPrint(json.get());
    }
};

class McpTool {
private:
    std::string name_;
    std::string description_;
    PropertyList properties_;
    std::function<ReturnValue(const PropertyList&)> callback_;
    std::function<std::string(const PropertyList&)> prepared_callback_;
    bool user_only_ = false;

public:
    McpTool(const std::string& name, 
            const std::string& description, 
            const PropertyList& properties, 
            std::function<ReturnValue(const PropertyList&)> callback)
        : name_(name), 
        description_(description), 
        properties_(properties), 
        callback_(callback) {}

    McpTool(
        const std::string& name,
        const std::string& description,
        const PropertyList& properties,
        PreparedMcpCall,
        std::function<std::string(const PropertyList&)> callback
    ) : name_(name), description_(description), properties_(properties),
        prepared_callback_(std::move(callback)) {}

    void set_user_only(bool user_only) { user_only_ = user_only; }
    inline const std::string& name() const { return name_; }
    inline const std::string& description() const { return description_; }
    inline const PropertyList& properties() const { return properties_; }
    inline bool user_only() const { return user_only_; }

    std::string to_json() const {
        std::vector<std::string> required = properties_.GetRequired();
        
        auto json = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(json.get(), "name", name_.c_str());
        CheckedCJsonAddStringToObject(json.get(), "description", description_.c_str());
        
        auto input_schema = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(input_schema.get(), "type", "object");
        
        const std::string properties_json = properties_.to_json();
        auto properties = ParseCheckedCJson(properties_json.c_str());
        CheckedCJsonAddItemToObject(
            input_schema.get(), "properties", std::move(properties));
        
        if (!required.empty()) {
            auto required_array = MakeCheckedCJsonArray();
            for (const auto& property : required) {
                CheckedCJsonAddItemToArray(
                    required_array.get(), MakeCheckedCJsonString(property.c_str()));
            }
            CheckedCJsonAddItemToObject(
                input_schema.get(), "required", std::move(required_array));
        }
        
        CheckedCJsonAddItemToObject(json.get(), "inputSchema", std::move(input_schema));

        // Add audience annotation if the tool is user only (invisible to AI)
        if (user_only_) {
            auto annotations = MakeCheckedCJsonObject();
            auto audience = MakeCheckedCJsonArray();
            CheckedCJsonAddItemToArray(audience.get(), MakeCheckedCJsonString("user"));
            CheckedCJsonAddItemToObject(
                annotations.get(), "audience", std::move(audience));
            CheckedCJsonAddItemToObject(
                json.get(), "annotations", std::move(annotations));
        }

        return CheckedCJsonPrint(json.get());
    }

    std::string Call(const PropertyList& properties) {
        if (prepared_callback_) {
            return prepared_callback_(properties);
        }
        ReturnValue return_value = callback_(properties);
        std::string payload;
        if (std::holds_alternative<ImageContent*>(return_value)) {
            std::unique_ptr<ImageContent> image(std::get<ImageContent*>(return_value));
            if (!image) ThrowCJsonAllocationFailure();
            payload = image->to_json();
        } else if (std::holds_alternative<std::string>(return_value)) {
            payload = std::get<std::string>(return_value);
        } else if (std::holds_alternative<bool>(return_value)) {
            payload = std::get<bool>(return_value) ? "true" : "false";
        } else if (std::holds_alternative<int>(return_value)) {
            payload = std::to_string(std::get<int>(return_value));
        } else if (std::holds_alternative<cJSON*>(return_value)) {
            CheckedCJsonPtr json(std::get<cJSON*>(return_value));
            if (!json) ThrowCJsonAllocationFailure();
            payload = CheckedCJsonPrint(json.get());
        }

        auto result = MakeCheckedCJsonObject();
        auto content = MakeCheckedCJsonArray();
        auto item = MakeCheckedCJsonObject();
        CheckedCJsonAddStringToObject(
            item.get(), "type",
            std::holds_alternative<ImageContent*>(return_value) ? "image" : "text");
        CheckedCJsonAddStringToObject(
            item.get(),
            std::holds_alternative<ImageContent*>(return_value) ? "image" : "text",
            payload.c_str());
        CheckedCJsonAddItemToArray(content.get(), std::move(item));
        CheckedCJsonAddItemToObject(result.get(), "content", std::move(content));
        CheckedCJsonAddBoolToObject(result.get(), "isError", false);
        return CheckedCJsonPrint(result.get());
    }
};

class McpServer {
public:
    static McpServer& GetInstance() {
        static McpServer instance;
        return instance;
    }

    void AddCommonTools();
    void AddUserOnlyTools();
    void AddTool(McpTool* tool);
    void AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback);
    void AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback);
    void AddUserOnlyTool(
        const std::string& name,
        const std::string& description,
        const PropertyList& properties,
        PreparedMcpCall mode,
        std::function<std::string(const PropertyList&)> callback);
    void ParseMessage(const cJSON* json);
    void ParseMessage(const std::string& message);

private:
    McpServer();
    ~McpServer();

    void ParseCapabilities(const cJSON* capabilities);

    void ReplyResult(int id, const std::string& result);
    void ReplyError(int id, const std::string& message);

    void GetToolsList(int id, const std::string& cursor, bool list_user_only_tools);
    void DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments);
    bool StartLessonAssetSyncTask(int id, McpTool* tool, PropertyList arguments);
    static void LessonAssetSyncTask(void* arg);

    std::vector<McpTool*> tools_;
    std::atomic<bool> lesson_asset_sync_in_flight_{false};
};

#endif // MCP_SERVER_H
