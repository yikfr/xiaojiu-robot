// 小玖云端长期记忆客户端示例（ESP-IDF）。
// 本文件是独立参考代码，尚未接入现有固件构建。请从独立工作任务调用，
// 不要在 LVGL、音频采集或播放任务中直接执行网络请求。
//
// 建议的服务端 API 约定：
//   POST /v1/memory/recall  {"user_id":"...","query":"..."}
//     -> {"memory_context":"..."}
//   POST /v1/memory/upsert  {"user_id":"...","key":"...","value":"...",
//                           "source":"explicit_user_request"}
//     -> {"ok":true}
//   POST /v1/memory/forget  {"user_id":"...","key":"..."}
//     -> {"ok":true}
// 服务端必须把 user_id 与已认证设备绑定；数据库账号不得下发到设备。
// 只有用户明确同意后，才保存长期记忆。

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_err.h"

namespace xiaojiu_cloud_memory {

namespace {
constexpr char kTag[] = "XJCloudMemory";
constexpr size_t kResponseCapacity = 4096;
constexpr int kTimeoutMs = 1800;

struct ResponseBuffer {
    char data[kResponseCapacity]{};
    size_t used = 0;
    bool overflow = false;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    if (event == nullptr || event->user_data == nullptr || event->data == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }
    auto* response = static_cast<ResponseBuffer*>(event->user_data);
    const size_t incoming = static_cast<size_t>(event->data_len);
    const size_t remaining = sizeof(response->data) - 1 - response->used;
    const size_t copy_size = std::min(incoming, remaining);
    if (copy_size != 0) {
        std::memcpy(response->data + response->used, event->data, copy_size);
        response->used += copy_size;
        response->data[response->used] = '\0';
    }
    if (copy_size != incoming) response->overflow = true;
    return ESP_OK;
}

// 同步阻塞的 HTTP 请求辅助函数。仅可在独立的低优先级工作任务中调用。
esp_err_t PostJson(const char* api_base_url, const char* device_token,
                   const char* route, const char* payload,
                   ResponseBuffer* response) {
    if (api_base_url == nullptr || device_token == nullptr || route == nullptr ||
        payload == nullptr || response == nullptr || std::strncmp(api_base_url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256]{};
    const int url_len = std::snprintf(url, sizeof(url), "%s%s%s",
        api_base_url, api_base_url[std::strlen(api_base_url) - 1] == '/' ? "" : "/", route);
    if (url_len < 0 || static_cast<size_t>(url_len) >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kTimeoutMs;
    config.event_handler = OnHttpEvent;
    config.user_data = response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return ESP_FAIL;
    esp_http_client_set_header(client, "Authorization", (std::string("Bearer ") + device_token).c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, payload, static_cast<int>(std::strlen(payload)));

    esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    if (result == ESP_OK && (status < 200 || status >= 300)) result = ESP_FAIL;
    if (response->overflow) result = ESP_ERR_INVALID_SIZE;
    if (result != ESP_OK) ESP_LOGW(kTag, "Cloud memory request failed: %s, HTTP %d", esp_err_to_name(result), status);
    esp_http_client_cleanup(client);
    return result;
}

bool AddString(cJSON* object, const char* name, const char* value) {
    return value != nullptr && cJSON_AddStringToObject(object, name, value) != nullptr;
}
}  // namespace

// 获取精简记忆摘要，供下一次模型会话使用。
// 服务端应只返回相关且经过用户同意保存的记忆，并限制摘要长度。
esp_err_t Recall(const char* api_base_url, const char* device_token,
                 const char* user_id, const char* query,
                 char* memory_context, size_t context_capacity) {
    if (memory_context == nullptr || context_capacity == 0) return ESP_ERR_INVALID_ARG;
    memory_context[0] = '\0';
    cJSON* body = cJSON_CreateObject();
    if (body == nullptr) return ESP_ERR_NO_MEM;
    const bool valid = AddString(body, "user_id", user_id) && AddString(body, "query", query);
    char* payload = valid ? cJSON_PrintUnformatted(body) : nullptr;
    cJSON_Delete(body);
    if (payload == nullptr) return ESP_ERR_NO_MEM;

    ResponseBuffer response{};
    const esp_err_t result = PostJson(api_base_url, device_token, "v1/memory/recall", payload, &response);
    cJSON_free(payload);
    if (result != ESP_OK) return result;

    cJSON* root = cJSON_Parse(response.data);
    cJSON* context = root == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(root, "memory_context");
    if (context == nullptr || !cJSON_IsString(context) || context->valuestring == nullptr) {
        if (root != nullptr) cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t length = std::strlen(context->valuestring);
    if (length >= context_capacity) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(memory_context, context->valuestring, length + 1);
    cJSON_Delete(root);
    return ESP_OK;
}

// 保存一条用户明确要求记住的偏好或事实。内容应简短，避免保存密码、
// 健康/财务等敏感信息，也不要长期保存原始聊天记录。
esp_err_t Upsert(const char* api_base_url, const char* device_token,
                 const char* user_id, const char* key, const char* value) {
    cJSON* body = cJSON_CreateObject();
    if (body == nullptr) return ESP_ERR_NO_MEM;
    const bool valid = AddString(body, "user_id", user_id) && AddString(body, "key", key) &&
                       AddString(body, "value", value) &&
                       AddString(body, "source", "explicit_user_request");
    char* payload = valid ? cJSON_PrintUnformatted(body) : nullptr;
    cJSON_Delete(body);
    if (payload == nullptr) return ESP_ERR_NO_MEM;
    ResponseBuffer response{};
    const esp_err_t result = PostJson(api_base_url, device_token, "v1/memory/upsert", payload, &response);
    cJSON_free(payload);
    return result;
}

esp_err_t Forget(const char* api_base_url, const char* device_token,
                 const char* user_id, const char* key) {
    cJSON* body = cJSON_CreateObject();
    if (body == nullptr) return ESP_ERR_NO_MEM;
    const bool valid = AddString(body, "user_id", user_id) && AddString(body, "key", key);
    char* payload = valid ? cJSON_PrintUnformatted(body) : nullptr;
    cJSON_Delete(body);
    if (payload == nullptr) return ESP_ERR_NO_MEM;
    ResponseBuffer response{};
    const esp_err_t result = PostJson(api_base_url, device_token, "v1/memory/forget", payload, &response);
    cJSON_free(payload);
    return result;
}

}  // namespace xiaojiu_cloud_memory

/* 接入示意（网络请求不得放在实时音频或界面任务中）：

char memory[1024];
if (xiaojiu_cloud_memory::Recall(API_BASE_URL, DEVICE_TOKEN, USER_ID,
                                 latest_user_utterance, memory, sizeof(memory)) == ESP_OK) {
    // 创建新的实时会话前，将这段简短记忆加入模型会话指令。
    // 如果请求超时，就不带记忆继续对话，避免网络问题卡住语音。
}
// 仅当用户明确说“记住……”且提取结果经过校验后，才从工作任务调用
// Upsert；同时应向用户提供 Forget 删除入口。

*/
