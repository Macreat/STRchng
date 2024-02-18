/*
 * http_server.c
 *
 *  Created on: Oct 20, 2021
 *      Author: kjagu
 */

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "sys/param.h"
#include <stdlib.h>

#include "uart_control.h"
#include "driver/uart.h"

#include "ButtonTask.h"

#include "http_server.h"
#include "tasks_common.h"
#include "wifi_app.h"
#include "rgb_led.h"
#include "cJSON.h"
#include "NTC.h"

extern QueueHandle_t Temperaturas;

// Tag used for ESP serial console messages
static const char TAG[] = "http_server";

// Wifi connect status
static int g_wifi_connect_status = NONE;

// Firmware update status
static int g_fw_update_status = OTA_UPDATE_PENDING;

// HTTP server task handle
static httpd_handle_t http_server_handle = NULL;

// HTTP server monitor task handle
static TaskHandle_t task_http_server_monitor = NULL;

// Queue handle used to manipulate the main queue of events
static QueueHandle_t http_server_monitor_queue_handle;

/**
 * ESP32 timer configuration passed to esp_timer_create.
 */
const esp_timer_create_args_t fw_update_reset_args = {
    .callback = &http_server_fw_update_reset_callback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "fw_update_reset"};
esp_timer_handle_t fw_update_reset;

// Embedded files: JQuery, index.html, app.css, app.js and favicon.ico files
extern const uint8_t jquery_3_3_1_min_js_start[] asm("_binary_jquery_3_3_1_min_js_start");
extern const uint8_t jquery_3_3_1_min_js_end[] asm("_binary_jquery_3_3_1_min_js_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t app_css_start[] asm("_binary_app_css_start");
extern const uint8_t app_css_end[] asm("_binary_app_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[] asm("_binary_app_js_end");
extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[] asm("_binary_favicon_ico_end");

/**
 * Checks the g_fw_update_status and creates the fw_update_reset timer if g_fw_update_status is true.
 */
static void http_server_fw_update_reset_timer(void)
{
    if (g_fw_update_status == OTA_UPDATE_SUCCESSFUL)
    {
        ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW updated successful starting FW update reset timer");

        // Give the web page a chance to receive an acknowledge back and initialize the timer
        ESP_ERROR_CHECK(esp_timer_create(&fw_update_reset_args, &fw_update_reset));
        ESP_ERROR_CHECK(esp_timer_start_once(fw_update_reset, 8000000));
    }
    else
    {
        ESP_LOGI(TAG, "http_server_fw_update_reset_timer: FW update unsuccessful");
    }
}

/**
 * HTTP server monitor task used to track events of the HTTP server
 * @param pvParameters parameter which can be passed to the task.
 */
static void http_server_monitor(void *parameter)
{
    http_server_queue_message_t msg;

    for (;;)
    {
        if (xQueueReceive(http_server_monitor_queue_handle, &msg, portMAX_DELAY))
        {
            switch (msg.msgID)
            {
            case HTTP_MSG_WIFI_CONNECT_INIT:
                ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_INIT");

                g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECTING;

                break;

            case HTTP_MSG_WIFI_CONNECT_SUCCESS:
                ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_SUCCESS");

                g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_SUCCESS;

                break;

            case HTTP_MSG_WIFI_CONNECT_FAIL:
                ESP_LOGI(TAG, "HTTP_MSG_WIFI_CONNECT_FAIL");

                g_wifi_connect_status = HTTP_WIFI_STATUS_CONNECT_FAILED;

                break;

            case HTTP_MSG_OTA_UPDATE_SUCCESSFUL:
                ESP_LOGI(TAG, "HTTP_MSG_OTA_UPDATE_SUCCESSFUL");
                g_fw_update_status = OTA_UPDATE_SUCCESSFUL;
                http_server_fw_update_reset_timer();

                break;

            case HTTP_MSG_OTA_UPDATE_FAILED:
                ESP_LOGI(TAG, "HTTP_MSG_OTA_UPDATE_FAILED");
                g_fw_update_status = OTA_UPDATE_FAILED;

                break;

            default:
                break;
            }
        }
    }
}

/**
 * Jquery get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_jquery_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Jquery requested");

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)jquery_3_3_1_min_js_start, jquery_3_3_1_min_js_end - jquery_3_3_1_min_js_start);

    return ESP_OK;
}

/**
 * Sends the index.html page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_index_html_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "index.html requested");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);

    return ESP_OK;
}

/**
 * app.css get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_app_css_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "app.css requested");

    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *)app_css_start, app_css_end - app_css_start);

    return ESP_OK;
}

/**
 * app.js get handler is requested when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_app_js_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "app.js requested");

    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *)app_js_start, app_js_end - app_js_start);

    return ESP_OK;
}

/**
 * Sends the .ico (icon) file when accessing the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
static esp_err_t http_server_favicon_ico_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "favicon.ico requested");

    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, (const char *)favicon_ico_start, favicon_ico_end - favicon_ico_start);

    return ESP_OK;
}

/**
 * Receives the .bin file fia the web page and handles the firmware update
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK, otherwise ESP_FAIL if timeout occurs and the update cannot be started.
 */
esp_err_t http_server_OTA_update_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle;

    char ota_buff[1024];
    int content_length = req->content_len;
    int content_received = 0;
    int recv_len;
    bool is_req_body_started = false;
    bool flash_successful = false;

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    do
    {
        // Read the data for the request
        if ((recv_len = httpd_req_recv(req, ota_buff, MIN(content_length, sizeof(ota_buff)))) < 0)
        {
            // Check if timeout occurred
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT)
            {
                ESP_LOGI(TAG, "http_server_OTA_update_handler: Socket Timeout");
                continue; ///> Retry receiving if timeout occurred
            }
            ESP_LOGI(TAG, "http_server_OTA_update_handler: OTA other Error %d", recv_len);
            return ESP_FAIL;
        }
        printf("http_server_OTA_update_handler: OTA RX: %d of %d\r", content_received, content_length);

        // Is this the first data we are receiving
        // If so, it will have the information in the header that we need.
        if (!is_req_body_started)
        {
            is_req_body_started = true;

            // Get the location of the .bin file content (remove the web form data)
            char *body_start_p = strstr(ota_buff, "\r\n\r\n") + 4;
            int body_part_len = recv_len - (body_start_p - ota_buff);

            printf("http_server_OTA_update_handler: OTA file size: %d\r\n", content_length);

            esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
            if (err != ESP_OK)
            {
                printf("http_server_OTA_update_handler: Error with OTA begin, cancelling OTA\r\n");
                return ESP_FAIL;
            }
            else
            {
                printf("http_server_OTA_update_handler: Writing to partition subtype %d at offset 0x%lx\r\n", update_partition->subtype, update_partition->address);
            }

            // Write this first part of the data
            esp_ota_write(ota_handle, body_start_p, body_part_len);
            content_received += body_part_len;
        }
        else
        {
            // Write OTA data
            esp_ota_write(ota_handle, ota_buff, recv_len);
            content_received += recv_len;
        }

    } while (recv_len > 0 && content_received < content_length);

    if (esp_ota_end(ota_handle) == ESP_OK)
    {
        // Lets update the partition
        if (esp_ota_set_boot_partition(update_partition) == ESP_OK)
        {
            const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
            ESP_LOGI(TAG, "http_server_OTA_update_handler: Next boot partition subtype %d at offset 0x%lx", boot_partition->subtype, boot_partition->address);
            flash_successful = true;
        }
        else
        {
            ESP_LOGI(TAG, "http_server_OTA_update_handler: FLASHED ERROR!!!");
        }
    }
    else
    {
        ESP_LOGI(TAG, "http_server_OTA_update_handler: esp_ota_end ERROR!!!");
    }

    // We won't update the global variables throughout the file, so send the message about the status
    if (flash_successful)
    {
        http_server_monitor_send_message(HTTP_MSG_OTA_UPDATE_SUCCESSFUL);
    }
    else
    {
        http_server_monitor_send_message(HTTP_MSG_OTA_UPDATE_FAILED);
    }

    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

/**
 * OTA status handler responds with the firmware update status after the OTA update is started
 * and responds with the compile time/date when the page is first requested
 * @param req HTTP request for which the uri needs to be handled
 * @return ESP_OK
 */
esp_err_t http_server_OTA_status_handler(httpd_req_t *req)
{
    char otaJSON[100];

    ESP_LOGI(TAG, "OTAstatus requested");

    sprintf(otaJSON, "{\"ota_update_status\":%d,\"compile_time\":\"%s\",\"compile_date\":\"%s\"}", g_fw_update_status, __TIME__, __DATE__);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, otaJSON, strlen(otaJSON));

    return ESP_OK;
}

/**
 * wifiConnect.json handler is invoked after the connect button is pressed
 * and handles receiving the SSID and password entered by the user
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */
#include "cJSON.h"

static esp_err_t http_server_wifi_connect_json_handler(httpd_req_t *req)
{
    size_t header_len;
    char *header_value;
    char *ssid_str = NULL;
    char *pass_str = NULL;
    int content_length;

    ESP_LOGI(TAG, "/wifiConnect.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0)
    {
        // Content-Length header not found or invalid
        // httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char *)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK)
    {
        // Failed to get Content-Length header value
        free(header_value);
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0)
    {
        // Content length is not a valid positive integer
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char *data_buffer = (char *)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0)
    {
        // Handle error while receiving data
        free(data_buffer);
        // httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON *root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL)
    {
        // JSON parsing error
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

    cJSON *ssid_json = cJSON_GetObjectItem(root, "selectedSSID");
    cJSON *pwd_json = cJSON_GetObjectItem(root, "pwd");

    if (ssid_json == NULL || pwd_json == NULL || !cJSON_IsString(ssid_json) || !cJSON_IsString(pwd_json))
    {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }

    // Extract SSID and password from JSON
    ssid_str = strdup(ssid_json->valuestring);
    pass_str = strdup(pwd_json->valuestring);

    cJSON_Delete(root);

    // Now, you have the SSID and password in ssid_str and pass_str
    ESP_LOGI(TAG, "Received SSID: %s", ssid_str);
    ESP_LOGI(TAG, "Received Password: %s", pass_str);

    // Update the Wifi networks configuration and let the wifi application know
    wifi_config_t *wifi_config = wifi_app_get_wifi_config();
    memset(wifi_config, 0x00, sizeof(wifi_config_t));
    memcpy(wifi_config->sta.ssid, ssid_str, strlen(ssid_str));
    memcpy(wifi_config->sta.password, pass_str, strlen(pass_str));
    wifi_app_send_message(WIFI_APP_MSG_CONNECTING_FROM_HTTP_SERVER);

    free(ssid_str);
    free(pass_str);

    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

/**
 * wifiConnectStatus handler updates the connection status for the web page.
 * @param req HTTP request for which the uri needs to be handled.
 * @return ESP_OK
 */

static esp_err_t http_server_wifi_connect_status_json_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "/wifiConnectStatus requested");

    char statusJSON[100];

    sprintf(statusJSON, "{\"wifi_connect_status\":%d}", g_wifi_connect_status);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, statusJSON, strlen(statusJSON));

    return ESP_OK;
}

static esp_err_t http_server_setRGB_json_handler(httpd_req_t *req)
{
    size_t header_len;
    char *header_value;
    char *value_R = NULL;
    char *value_G = NULL;
    char *value_B = NULL;
    int content_length;

    ESP_LOGI(TAG, "/setRGB.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0)
    {
        // Content-Length header not found or invalid
        // httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char *)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK)
    {
        // Failed to get Content-Length header value
        free(header_value);
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0)
    {
        // Content length is not a valid positive integer
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char *data_buffer = (char *)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0)
    {
        // Handle error while receiving data
        free(data_buffer);
        // httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON *root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL)
    {
        // JSON parsing error
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

    cJSON *value_R_json = cJSON_GetObjectItem(root, "value_R");
    cJSON *value_G_json = cJSON_GetObjectItem(root, "value_G");
    cJSON *value_B_json = cJSON_GetObjectItem(root, "value_B");

    if (value_R_json == NULL || value_G_json == NULL || value_B_json == NULL || !cJSON_IsString(value_R_json) || !cJSON_IsString(value_G_json) | !cJSON_IsString(value_B_json))
    {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }

    // Extract SSID and password from JSON
    value_R = strdup(value_R_json->valuestring);
    value_G = strdup(value_G_json->valuestring);
    value_B = strdup(value_B_json->valuestring);

    int value_RED = 100 - atoi(value_R);
    int value_GREEN = 100 - atoi(value_G);
    int value_BLUE = 100 - atoi(value_B);

    cJSON_Delete(root);

    // Now, you have the SSID and password in ssid_str and pass_str
    ESP_LOGI(TAG, "value_RED: %d", value_RED);
    ESP_LOGI(TAG, "value_GREEN: %d", value_GREEN);
    ESP_LOGI(TAG, "value_BLUE: %d", value_BLUE);

    // Update the RGB PWM
    updateRGB(value_RED, value_GREEN, value_BLUE);

    free(value_R);
    free(value_G);
    free(value_B);

    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t http_server_setRGB2_json_handler(httpd_req_t *req)
{
    size_t header_len;
    char *header_value;
    char *value_R2 = NULL;
    char *value_G2 = NULL;
    char *value_B2 = NULL;
    int content_length;

    ESP_LOGI(TAG, "/setRGB2.json requested");

    // Get the "Content-Length" header to determine the length of the request body
    header_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (header_len <= 0)
    {
        // Content-Length header not found or invalid
        // httpd_resp_send_err(req, HTTP_STATUS_411_LENGTH_REQUIRED, "Content-Length header is missing or invalid");
        ESP_LOGI(TAG, "Content-Length header is missing or invalid");
        return ESP_FAIL;
    }

    // Allocate memory to store the header value
    header_value = (char *)malloc(header_len + 1);
    if (httpd_req_get_hdr_value_str(req, "Content-Length", header_value, header_len + 1) != ESP_OK)
    {
        // Failed to get Content-Length header value
        free(header_value);
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Failed to get Content-Length header value");
        ESP_LOGI(TAG, "Failed to get Content-Length header value");
        return ESP_FAIL;
    }

    // Convert the Content-Length header value to an integer
    content_length = atoi(header_value);
    free(header_value);

    if (content_length <= 0)
    {
        // Content length is not a valid positive integer
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid Content-Length value");
        ESP_LOGI(TAG, "Invalid Content-Length value");
        return ESP_FAIL;
    }

    // Allocate memory for the data buffer based on the content length
    char *data_buffer = (char *)malloc(content_length + 1);

    // Read the request body into the data buffer
    if (httpd_req_recv(req, data_buffer, content_length) <= 0)
    {
        // Handle error while receiving data
        free(data_buffer);
        // httpd_resp_send_err(req, HTTP_STATUS_INTERNAL_SERVER_ERROR, "Failed to receive request body");
        ESP_LOGI(TAG, "Failed to receive request body");
        return ESP_FAIL;
    }

    // Null-terminate the data buffer to treat it as a string
    data_buffer[content_length] = '\0';

    // Parse the received JSON data
    cJSON *root = cJSON_Parse(data_buffer);
    free(data_buffer);

    if (root == NULL)
    {
        // JSON parsing error
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Invalid JSON data");
        ESP_LOGI(TAG, "Invalid JSON data");
        return ESP_FAIL;
    }

    cJSON *value_R2_json = cJSON_GetObjectItem(root, "value_R2");
    cJSON *value_G2_json = cJSON_GetObjectItem(root, "value_G2");
    cJSON *value_B2_json = cJSON_GetObjectItem(root, "value_B2");

    if (value_R2_json == NULL || value_G2_json == NULL || value_B2_json == NULL ||
        !cJSON_IsString(value_R2_json) || !cJSON_IsString(value_G2_json) | !cJSON_IsString(value_B2_json))
    {
        cJSON_Delete(root);
        // Missing or invalid JSON fields
        // httpd_resp_send_err(req, HTTP_STATUS_BAD_REQUEST, "Missing or invalid JSON data fields");
        ESP_LOGI(TAG, "Missing or invalid JSON data fields");
        return ESP_FAIL;
    }

    // Extract SSID and password from JSON
    value_R2 = strdup(value_R2_json->valuestring);
    value_G2 = strdup(value_G2_json->valuestring);
    value_B2 = strdup(value_B2_json->valuestring);

    int value_RED2 = 200 - atoi(value_R2);
    int value_GREEN2 = 200 - atoi(value_G2);
    int value_BLUE2 = 200 - atoi(value_B2);

    cJSON_Delete(root);

    // Now, you have the SSID and password in ssid_str and pass_str
    ESP_LOGI(TAG, "value_RED2: %d", value_RED2);
    ESP_LOGI(TAG, "value_GREEN2: %d", value_GREEN2);
    ESP_LOGI(TAG, "value_BLUE2: %d", value_BLUE2);

    // Update the RGB PWM
    updateRGB(value_RED2, value_GREEN2, value_BLUE2);

    free(value_R2);
    free(value_G2);
    free(value_B2);

    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t http_server_show_temp(httpd_req_t *req)
{
    float static value = 0;

    xQueueReceive(Temperaturas, &value, pdMS_TO_TICKS(100));

    char buffer[12];
    sprintf(buffer, "%.1f", value);
    char *value_str = buffer;
    ESP_LOGI(TAG, "Actual temperature: %f °C", value);

    httpd_resp_set_type(req, "text/text");
    httpd_resp_send(req, value_str, strlen(value_str));
    vTaskDelay(pdMS_TO_TICKS(2000));

    return ESP_OK;
}

static esp_err_t http_server_get_button_count_handler(httpd_req_t *req)
{
    char resp[100];
    int button1_count = get_button_1_press_count();
    int button2_count = get_button_2_press_count();

    sprintf(resp, "{\"button1\": %d, \"button2\": %d}", button1_count, button2_count);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    return ESP_OK;
}
static esp_err_t http_server_rgb_uart(httpd_req_t *req)
{
    char content[100];
    int ret = httpd_req_recv(req, content, sizeof(content));
    if (ret <= 0)
    { // Error o conexión cerrada
        return ESP_FAIL;
    }

    // Aquí asumimos que content tiene el formato "R100G100B100"
    char response[200];
    if (uart_control_send_rgb_command(content))
    {
        sprintf(response, "RGB command sent: %s", content);
    }
    else
    {
        sprintf(response, "Failed to send RGB command");
    }

    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

static esp_err_t text_command_handler(httpd_req_t *req)
{
    char content[100];
    int received = httpd_req_recv(req, content, sizeof(content));
    if (received <= 0)
    {
        ESP_LOGI(tag, "Error en el envío de texto");

        return ESP_FAIL; // Manejar errores de recepción aquí
    }

    content[received] = '\0'; // Asegurarse de que la cadena esté terminada correctamente

    if (strstr(content, "RED"))
    {
        // turnOnRedLED();
        // setStateLed(1, 0, 0);
        ESP_LOGI(tag, "LED ROJO ENCENDIDO");
        gpio_set_level(RGB_LED_RED_GPIO_TEXT, 0);
        gpio_set_level(RGB_LED_GREEN_GPIO_TEXT, 1);

        gpio_set_level(RGB_LED_BLUE_GPIO_TEXT, 1);
    }
    else if (strstr(content, "GREEN"))
    {
        // turnOnGreenLED();
        // setStateLed(0, 1, 0);
        gpio_set_level(RGB_LED_RED_GPIO_TEXT, 1);

        gpio_set_level(RGB_LED_GREEN_GPIO_TEXT, 0);

        gpio_set_level(RGB_LED_BLUE_GPIO_TEXT, 1);
        ESP_LOGI(tag, "LED VERDE ENCENDIDO");
    }
    else if (strstr(content, "BLUE"))
    {
        // turnOnBlueLED();
        // setStateLed(0, 0, 1);
        gpio_set_level(RGB_LED_RED_GPIO_TEXT, 1);

        gpio_set_level(RGB_LED_GREEN_GPIO_TEXT, 1);

        gpio_set_level(RGB_LED_BLUE_GPIO_TEXT, 0);
        ESP_LOGI(tag, "LED AZÚL ENCENDIDO");
    }

    httpd_resp_send(req, "LED Command Received", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/**
 * Sets up the default httpd server configuration.
 * @return http server instance handle if successful, NULL otherwise.
 */
static httpd_handle_t http_server_configure(void)
{
    // Generate the default configuration
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Create HTTP server monitor task
    xTaskCreatePinnedToCore(&http_server_monitor, "http_server_monitor", HTTP_SERVER_MONITOR_STACK_SIZE, NULL, HTTP_SERVER_MONITOR_PRIORITY, &task_http_server_monitor, HTTP_SERVER_MONITOR_CORE_ID);

    // Create the message queue
    http_server_monitor_queue_handle = xQueueCreate(3, sizeof(http_server_queue_message_t));

    // The core that the HTTP server will run on
    config.core_id = HTTP_SERVER_TASK_CORE_ID;

    // Adjust the default priority to 1 less than the wifi application task
    config.task_priority = HTTP_SERVER_TASK_PRIORITY;

    // Bump up the stack size (default is 4096)
    config.stack_size = HTTP_SERVER_TASK_STACK_SIZE;

    // Increase uri handlers
    config.max_uri_handlers = 20;

    // Increase the timeout limits
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    ESP_LOGI(TAG,
             "http_server_configure: Starting server on port: '%d' with task priority: '%d'",
             config.server_port,
             config.task_priority);

    // Start the httpd server
    if (httpd_start(&http_server_handle, &config) == ESP_OK)
    {
        ESP_LOGI(TAG, "http_server_configure: Registering URI handlers");

        // register query handler
        httpd_uri_t jquery_js = {
            .uri = "/jquery-3.3.1.min.js",
            .method = HTTP_GET,
            .handler = http_server_jquery_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &jquery_js);

        // register index.html handler
        httpd_uri_t index_html = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = http_server_index_html_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &index_html);

        // register app.css handler
        httpd_uri_t app_css = {
            .uri = "/app.css",
            .method = HTTP_GET,
            .handler = http_server_app_css_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &app_css);

        // register app.js handler
        httpd_uri_t app_js = {
            .uri = "/app.js",
            .method = HTTP_GET,
            .handler = http_server_app_js_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &app_js);

        // register favicon.ico handler
        httpd_uri_t favicon_ico = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = http_server_favicon_ico_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &favicon_ico);

        // register OTAupdate handler
        httpd_uri_t OTA_update = {
            .uri = "/OTAupdate",
            .method = HTTP_POST,
            .handler = http_server_OTA_update_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &OTA_update);

        // register OTAstatus handler
        httpd_uri_t OTA_status = {
            .uri = "/OTAstatus",
            .method = HTTP_POST,
            .handler = http_server_OTA_status_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &OTA_status);

        // register dhtSensor.json handler
        /*httpd_uri_t dht_sensor_json = {
                .uri = "/dhtSensor.json",
                .method = HTTP_GET,
                .handler = http_server_get_dht_sensor_readings_json_handler,
                .user_ctx = NULL
        };
        httpd_register_uri_handler(http_server_handle, &dht_sensor_json);
*/
        // register wifiConnect.json handler
        httpd_uri_t wifi_connect_json = {
            .uri = "/wifiConnect.json",
            .method = HTTP_POST,
            .handler = http_server_wifi_connect_json_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &wifi_connect_json);

        // register wifiConnectStatus.json handler
        httpd_uri_t wifi_connect_status_json = {
            .uri = "/wifiConnectStatus",
            .method = HTTP_POST,
            .handler = http_server_wifi_connect_status_json_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &wifi_connect_status_json);

        httpd_uri_t set_RGB_json = {
            .uri = "/setRGB.json",
            .method = HTTP_POST,
            .handler = http_server_setRGB_json_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &set_RGB_json);

        httpd_uri_t set_RGB2_json = {
            .uri = "/setRGB2.json",
            .method = HTTP_POST,
            .handler = http_server_setRGB2_json_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &set_RGB2_json);

        httpd_uri_t show_temp = {
            .uri = "/showTemp",
            .method = HTTP_GET,
            .handler = http_server_show_temp,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &show_temp);

        httpd_uri_t buttonCount = {
            .uri = "/getButtonCount",
            .method = HTTP_GET,
            .handler = http_server_get_button_count_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &buttonCount);

        httpd_uri_t rgbUart = {
            .uri = "/setRGBuart",
            .method = HTTP_POST,
            .handler = http_server_rgb_uart,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &rgbUart);

        httpd_uri_t text_command_uri = {
            .uri = "/text-command",
            .method = HTTP_POST,
            .handler = text_command_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(http_server_handle, &text_command_uri);

        return http_server_handle;
    }

    return NULL;
}

void http_server_start(void)
{
    if (http_server_handle == NULL)
    {
        http_server_handle = http_server_configure();
    }
}

void http_server_stop(void)
{
    if (http_server_handle)
    {
        httpd_stop(http_server_handle);
        ESP_LOGI(TAG, "http_server_stop: stopping HTTP server");
        http_server_handle = NULL;
    }
    if (task_http_server_monitor)
    {
        vTaskDelete(task_http_server_monitor);
        ESP_LOGI(TAG, "http_server_stop: stopping HTTP server monitor");
        task_http_server_monitor = NULL;
    }
}

BaseType_t http_server_monitor_send_message(http_server_message_e msgID)
{
    http_server_queue_message_t msg;
    msg.msgID = msgID;
    return xQueueSend(http_server_monitor_queue_handle, &msg, portMAX_DELAY);
}

void http_server_fw_update_reset_callback(void *arg)
{
    ESP_LOGI(TAG, "http_server_fw_update_reset_callback: Timer timed-out, restarting the device");
    esp_restart();
}