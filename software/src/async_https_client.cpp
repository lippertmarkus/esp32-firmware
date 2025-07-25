/* esp32-firmware
 * Copyright (C) 2024 Matthias Bolte <matthias@tinkerforge.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#define EVENT_LOG_PREFIX "async_https_clnt"

#include <esp_tls.h>

#include "async_https_client.h"

#include "event_log_prefix.h"
#include "main_dependencies.h"
#include "build.h"
#include "options.h"

static constexpr micros_t ASYNC_HTTPS_CLIENT_TIMEOUT = 15_s;

extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

AsyncHTTPSClient::AsyncHTTPSClient(bool use_cookies) : use_cookies{use_cookies}
{
    this->add_default_headers();
}

AsyncHTTPSClient::~AsyncHTTPSClient()  {
    if (task_id != 0) {
        task_scheduler.cancel(task_id);
    }
    if (http_client != nullptr) {
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
    }
}

esp_err_t AsyncHTTPSClient::event_handler(esp_http_client_event_t *event)
{
    AsyncHTTPSClient *that = static_cast<AsyncHTTPSClient *>(event->user_data);
    AsyncHTTPSClientEvent async_event;
    int http_status;

    switch (event->event_id) {
    case HTTP_EVENT_ERROR:
        async_event.type = AsyncHTTPSClientEventType::Error;
        async_event.error = AsyncHTTPSClientError::HTTPError;
        async_event.error_http_client = ESP_OK;
        async_event.error_http_status = -1;

        if (event->data_len == 0) {
            async_event.error_handle = static_cast<esp_tls_error_handle_t>(event->data);
        } else {
            async_event.error_handle = nullptr;
            logger.printfln("event_handler received HTTP_EVENT_ERROR with unexpected data: %iB @ %p", event->data_len, event->data);
        }

        if (!that->abort_requested) {
            that->callback(&async_event);
        }

        break;

    case HTTP_EVENT_ON_CONNECTED:
        break;

    case HTTP_EVENT_HEADERS_SENT:
        break;

    case HTTP_EVENT_ON_HEADER:
        if (that->use_cookies) {
            for (int i = 0; event->header_key[i] != 0; i++) {
                event->header_key[i] = tolower(event->header_key[i]);
            }
            if (!strcmp("set-cookie", event->header_key)) {
                that->parse_cookie(event->header_value);
            }
        }
        break;

    case HTTP_EVENT_ON_DATA:
        that->last_async_alive = now_us();
        http_status = esp_http_client_get_status_code(that->http_client);

        if (http_status == 301 /* Moved Permanently */
         || http_status == 302 /* Found */
         || http_status == 303 /* See Other */
         || http_status == 307 /* Temporary Redirect */
         || http_status == 308 /* Permanent Redirect */) {
            break;
        }
        else if (http_status != 200) {
            that->in_progress = false;

            async_event.type = AsyncHTTPSClientEventType::Error;
            async_event.error = AsyncHTTPSClientError::HTTPStatusError;
            async_event.error_handle = nullptr;
            async_event.error_http_client = ESP_OK;
            async_event.error_http_status = http_status;

            if (!that->abort_requested) {
                that->callback(&async_event);
            }

            break;
        }

        async_event.type = AsyncHTTPSClientEventType::Data;
        async_event.data_chunk_offset = that->received_len;
        async_event.data_chunk = event->data;
        async_event.data_chunk_len = event->data_len;
        async_event.data_complete_len = (ssize_t)esp_http_client_get_content_length(that->http_client);
        async_event.data_is_complete = esp_http_client_is_complete_data_received(that->http_client);

        that->received_len += event->data_len;

        if (!that->abort_requested) {
            that->callback(&async_event);
        }

        break;

    case HTTP_EVENT_ON_FINISH:
        break;

    case HTTP_EVENT_DISCONNECTED:
        break;

    case HTTP_EVENT_REDIRECT:
        async_event.type = AsyncHTTPSClientEventType::Redirect;
        async_event.redirect_status_code = static_cast<esp_http_client_redirect_event_data *>(event->data)->status_code;

        if (!that->abort_requested) {
            that->callback(&async_event);
        }

        break;
    }

    return ESP_OK;
}

static const char *https_prefix = "https://";
static const size_t https_prefix_len = strlen(https_prefix);

void AsyncHTTPSClient::fetch(const char *url, int cert_id, esp_http_client_method_t method, const char *body, int body_size, std::function<void(AsyncHTTPSClientEvent *event)> &&callback) {
    if (strncmp(url, https_prefix, https_prefix_len) != 0) {
        error_abort(AsyncHTTPSClientError::NoHTTPSURL);
        return;
    }

    if (in_progress) {
        error_abort(AsyncHTTPSClientError::Busy);
        return;
    }

    this->callback = std::move(callback);
    in_progress = true;
    abort_requested = false;
    received_len = 0;

    if (body != nullptr) {
        owned_body = String(body, body_size);
    }

    esp_http_client_config_t http_config = {};

    http_config.method = method;
    http_config.url = url;
    http_config.event_handler = event_handler;
    http_config.user_data = this;
    http_config.is_async = true;
    http_config.timeout_ms = 50;
    http_config.buffer_size = 1024;
    http_config.buffer_size_tx = 1024;

    if (cert_id < 0) {
        http_config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    else {
#if MODULE_CERTS_AVAILABLE()
        size_t cert_len = 0;
        cert = certs.get_cert(static_cast<uint8_t>(cert_id), &cert_len);

        if (cert == nullptr) {
            error_abort(AsyncHTTPSClientError::NoCert);
            return;
        }

        http_config.cert_pem = (const char *)cert.get();
        // http_config.skip_cert_common_name_check = true;
#else
        // defense in depth: it should not be possible to arrive here because in case
        // that the certs module is not available the cert_id should always be -1
        logger.printfln("Can't use custom certificate: certs module is not built into this firmware!");

        error_abort(AsyncHTTPSClientError::NoCert);
        return;
#endif
    }

    http_client = esp_http_client_init(&http_config);

    if (http_client == nullptr) {
        error_abort(AsyncHTTPSClientError::HTTPClientInitFailed);
        return;
    }

    if (owned_body.length() > 0 && esp_http_client_set_post_field(http_client, owned_body.c_str(), owned_body.length())) {
        error_abort(AsyncHTTPSClientError::HTTPClientSetBodyFailed);
        return;
    }

    if (cookies.length() > 0) {
        if (esp_http_client_set_header(http_client, "cookie", cookies.c_str()) != ESP_OK) {
            error_abort(AsyncHTTPSClientError::HTTPClientSetCookieFailed);
            return;
        }
    }
    if (headers.size() > 0) {
        for (std::pair<String, String> header : headers) {
            if (esp_http_client_set_header(http_client, header.first.c_str(), header.second.c_str()) != ESP_OK) {
                error_abort(AsyncHTTPSClientError::HTTPClientSetCookieFailed);
                return;
            }
        }
    }

    last_async_alive = now_us();

    task_id = task_scheduler.scheduleWithFixedDelay([this]() {
        bool no_response = false;
        bool short_read = false;
        esp_err_t err = ESP_OK;

        if (!abort_requested && in_progress) {
            if (deadline_elapsed(last_async_alive + ASYNC_HTTPS_CLIENT_TIMEOUT)) {
                no_response = true;
            }
            else {
                err = esp_http_client_perform(http_client);

                if (!abort_requested) {
                    if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_HTTP_FETCH_HEADER) {
                        return;
                    }

                    if (err == ESP_OK && !esp_http_client_is_complete_data_received(http_client)) {
                        short_read = true;
                    }
                }
            }
        }

        if (abort_requested) {
            AsyncHTTPSClientEvent async_event;
            async_event.type = AsyncHTTPSClientEventType::Aborted;
            this->callback(&async_event);
        }
        else if (no_response) {
            error_abort(AsyncHTTPSClientError::Timeout);
        }
        else if (short_read) {
            error_abort(AsyncHTTPSClientError::ShortRead);
        }
        else if (err != ESP_OK) {
            error_abort(AsyncHTTPSClientError::HTTPClientError, err);
        }
        else if (in_progress) {
            AsyncHTTPSClientEvent async_event;
            async_event.type = AsyncHTTPSClientEventType::Finished;
            this->callback(&async_event);
        }

        clear();

        task_scheduler.cancel(task_scheduler.currentTaskId());
        task_id = 0;
    }, 200_ms);
}

void AsyncHTTPSClient::download_async(const char *url, int cert_id, std::function<void(AsyncHTTPSClientEvent *event)> &&callback)
{
    fetch(url, cert_id, HTTP_METHOD_GET, nullptr, 0, std::move(callback));
}

void AsyncHTTPSClient::post_async(const char *url, int cert_id, const char *body, int body_size, std::function<void(AsyncHTTPSClientEvent *event)> &&callback)
{
    fetch(url, cert_id, HTTP_METHOD_POST, body, body_size, std::move(callback));
}

void AsyncHTTPSClient::put_async(const char *url, int cert_id, const char *body, int body_size, std::function<void(AsyncHTTPSClientEvent *event)> &&callback)
{
    fetch(url, cert_id, HTTP_METHOD_PUT, body, body_size, std::move(callback));
}

void AsyncHTTPSClient::delete_async(const char *url, int cert_id, const char *body, int body_size, std::function<void(AsyncHTTPSClientEvent *event)> &&callback)
{
    fetch(url, cert_id, HTTP_METHOD_DELETE, body, body_size, std::move(callback));
}

void AsyncHTTPSClient::set_header(const String &key, const String &value) {
    headers.emplace_back(std::pair<String, String>(key, value));
}

void AsyncHTTPSClient::set_header(const char *key, const char *value) {
    if (key == nullptr || value == nullptr) {
        return;
    }

    String k = key;
    String v = value;
    headers.push_back(std::pair<String, String>(k, v));
}

void AsyncHTTPSClient::error_abort(AsyncHTTPSClientError error, esp_err_t error_http_client, int error_http_status)
{
    AsyncHTTPSClientEvent async_event;

    async_event.type = AsyncHTTPSClientEventType::Error;
    async_event.error = error;
    async_event.error_handle = nullptr;
    async_event.error_http_client = error_http_client;
    async_event.error_http_status = error_http_status;

    clear();

    callback(&async_event);
}

void AsyncHTTPSClient::clear()
{
    if (http_client != nullptr) {
        esp_http_client_close(http_client);
        esp_http_client_cleanup(http_client);
        http_client = nullptr;
    }

    cert.reset();
    headers = std::vector<std::pair<String, String>>();
    this->add_default_headers();
    owned_body = String();
    in_progress = false;
}

void AsyncHTTPSClient::parse_cookie(const char *cookie) {
    char *i = strchr(cookie, ';');
    if (i != nullptr) {
        *i = 0;
    }
    cookies += String(cookie) + ';';
}

void AsyncHTTPSClient::abort_async()
{
    abort_requested = true;
}

void AsyncHTTPSClient::add_default_headers() {
    this->set_header("User-Agent", String(OPTIONS_MANUFACTURER_USER_AGENT() "-" OPTIONS_PRODUCT_NAME_USER_AGENT() "/") + build_version_full_str());
}

const char *translate_error(AsyncHTTPSClientEvent *event) {
    if (event->type != AsyncHTTPSClientEventType::Error) {
        return "";
    }

    switch (event->error) {
        case AsyncHTTPSClientError::NoHTTPSURL:
            return "Not a HTTPS url";

        case AsyncHTTPSClientError::Busy:
            return "AsyncHTTPSClient busy";

        case AsyncHTTPSClientError::NoCert:
            return "Certificate not found";

        case AsyncHTTPSClientError::Timeout:
            return "Connection timed out";

        case AsyncHTTPSClientError::ShortRead:
            return "Received incomplete response";

        case AsyncHTTPSClientError::HTTPError:
            if (!event->error_handle) {
                return "Error during execution";
            } else {
                return esp_err_to_name(event->error_handle->last_error);
            }

        case AsyncHTTPSClientError::HTTPClientInitFailed:
            return "Initializing HTTP-Client failed";

        case AsyncHTTPSClientError::HTTPClientSetCookieFailed:
            return "Setting request-cookie failed";

        case AsyncHTTPSClientError::HTTPClientSetHeaderFailed:
            return "Setting request-header failed";

        case AsyncHTTPSClientError::HTTPClientSetBodyFailed:
            return "Setting request-body failed";

        case AsyncHTTPSClientError::HTTPClientError:
            return esp_err_to_name(event->error_http_client);

        case AsyncHTTPSClientError::HTTPStatusError:
            return "Received HTTP-Error status-code";
    }

    return "";
}

size_t translate_HTTPError_detailed(const esp_tls_error_handle_t error_handle, char *buf, size_t buflen, bool include_sock_errno)
{
    StringWriter sw(buf, buflen);

    if (!error_handle) {
        sw.printf("Unknown error (no handle)");
    } else {
        const uint32_t esp_tls_flags = static_cast<uint32_t>(error_handle->esp_tls_flags);
        if (esp_tls_flags != 0) {
            char *remaining = sw.getRemainingPtr();
            int len = mbedtls_x509_crt_verify_info(remaining, sw.getRemainingLength(), "", esp_tls_flags);
            if (len > 0) {
                if (remaining[len - 1] == '\n') {
                    len--;
                    remaining[len] = '\0';
                }
                for (int i = 0; i < len; i++) {
                    if (remaining[i] == '\n') {
                        remaining[i] = ';';
                    }
                }

                sw.setLength(sw.getLength() + static_cast<size_t>(len));
            }
        } else {
            bool needs_divider = false;

            const esp_err_t last_error = error_handle->last_error;
            if (last_error != ESP_OK) {
                const char *last_error_str = esp_err_to_name(last_error);
                if (!last_error_str) {
                    last_error_str = "Unknown ESP_ERR_ESP_TLS_BASE error code";
                }
                sw.printf("%s (0x%lX)", last_error_str, static_cast<uint32_t>(last_error));
                needs_divider = true;
            }

            const int error_code = error_handle->esp_tls_error_code;
            if (error_code != 0) {
                if (needs_divider) {
                    sw.puts("; ");
                }
                char *remaining = sw.getRemainingPtr();
                mbedtls_strerror(error_code, remaining, sw.getRemainingLength());
                sw.setLength(sw.getLength() + strlen(remaining));
                sw.printf(" (0x%lX)", static_cast<uint32_t>(error_code));
                needs_divider = true;
            }

            if (include_sock_errno) {
                int sock_errno = 0;
                esp_tls_get_and_clear_error_type(error_handle, ESP_TLS_ERR_TYPE_SYSTEM, &sock_errno);
                if (sock_errno != 0) {
                    if (needs_divider) {
                        sw.puts("; ");
                    }
                    const char *error_str = strerror(sock_errno);
                    if (!error_str) {
                        error_str = "Unknown system error code";
                    }
                    sw.printf("%s (%i)", error_str, sock_errno);
                }
            }
        }
    }

    return sw.getLength();
}
