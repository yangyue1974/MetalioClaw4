#include "audiolib_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <vector>

#define TAG "AudiolibClient"

bool AudiolibClient::FetchTrack(const std::string& library, AudiolibTrack* out) {
    last_error_.clear();
    if (api_key_.empty()) {
        last_error_ = "api key empty";
        return false;
    }

    std::string body = "{\"library\":\"" + library + "\"}";
    std::vector<char> response;

    esp_http_client_config_t cfg = {};
    cfg.url = "https://api.audiolib.ai/v1/audio";
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        last_error_ = "http init failed";
        return false;
    }

    std::string auth = "Bearer " + api_key_;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");

    bool ok = false;
    // esp_http_client_open() 的 write_len 参数只是告诉库接下来要写多少字节的请求体,
    // 之后必须显式调用 esp_http_client_write() 才会真正把 body 发出去。
    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err != ESP_OK) {
        last_error_ = "open failed: " + std::string(esp_err_to_name(err));
    } else if (esp_http_client_write(client, body.data(), static_cast<int>(body.size())) < 0) {
        last_error_ = "write failed";
    } else if (esp_http_client_fetch_headers(client) < 0) {
        // esp_http_client_fetch_headers() 返回 int64_t:
        // >=0 表示 content-length(0 表示没有 content-length 头,或者是 chunked 编码,
        // 这两种都不算错误,后面照样能用 esp_http_client_read() 读到数据);
        // <0 才是真错误(ESP_FAIL 或 -ESP_ERR_HTTP_EAGAIN 超时)。
        last_error_ = "fetch headers failed";
    } else {
        int status = esp_http_client_get_status_code(client);
        response.resize(4096);
        int total = 0;
        while (total < static_cast<int>(response.size()) - 1) {
            int n = esp_http_client_read(client, response.data() + total,
                                         static_cast<int>(response.size()) - 1 - total);
            if (n <= 0) break;
            total += n;
        }
        response[total] = '\0';
        if (status != 200) {
            last_error_ = "http " + std::to_string(status) + ": " + std::string(response.data());
        } else {
            cJSON* root = cJSON_Parse(response.data());
            if (root == nullptr) {
                last_error_ = "bad json";
            } else {
                cJSON* data = cJSON_GetObjectItem(root, "data");
                cJSON* url  = data ? cJSON_GetObjectItem(data, "url") : nullptr;
                if (url == nullptr || !cJSON_IsString(url)) {
                    last_error_ = "no url in response";
                } else {
                    out->url = url->valuestring;
                    cJSON* t = cJSON_GetObjectItem(data, "title");
                    if (cJSON_IsString(t)) out->title = t->valuestring;
                    cJSON* d = cJSON_GetObjectItem(data, "duration_sec");
                    if (cJSON_IsNumber(d)) out->duration_sec = d->valueint;
                    cJSON* q = cJSON_GetObjectItem(data, "quota");
                    if (q != nullptr) {
                        cJSON* r = cJSON_GetObjectItem(q, "remaining_quota");
                        cJSON* tt = cJSON_GetObjectItem(q, "total_quota");
                        if (cJSON_IsNumber(r)) out->quota_remaining = r->valueint;
                        if (cJSON_IsNumber(tt)) out->quota_total = tt->valueint;
                    }
                    ok = true;
                }
                cJSON_Delete(root);
            }
        }
    }
    // 无论上面走到哪一步失败,client 都是 esp_http_client_init() 成功返回的同一个句柄,
    // close/cleanup 只在这里统一调用一次,不会漏调也不会重复调。
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (ok) {
        ESP_LOGI(TAG, "track=\"%s\" %ds quota=%d/%d", out->title.c_str(), out->duration_sec,
                 out->quota_remaining, out->quota_total);
    } else {
        ESP_LOGE(TAG, "FetchTrack failed: %s", last_error_.c_str());
    }
    return ok;
}
