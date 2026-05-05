/**
 * @file web_server.h
 * @brief Web服务器模块接口
 * @note 提供网页控制界面和RESTful API
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== Web服务器配置 ====================

/**
 * @brief Web服务器配置结构体
 */
typedef struct {
    uint16_t port;                // 服务端口
    bool enable_auth;             // 是否启用认证
    char username[32];            // 认证用户名
    char password[32];            // 认证密码
} web_server_config_t;

// ==================== 初始化和控制接口 ====================

/**
 * @brief 初始化Web服务器模块
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t web_server_init(void);

/**
 * @brief 初始化日志拦截器，将ESP_LOG输出捕获到内存缓冲区
 * @note 应在web_server_start()之前调用
 * @return ESP_OK 成功
 */
esp_err_t web_server_init_log_interceptor(void);

/**
 * @brief 反初始化Web服务器模块
 * @return ESP_OK 成功
 */
esp_err_t web_server_deinit(void);

/**
 * @brief 启动Web服务器
 * @return ESP_OK 成功
 */
esp_err_t web_server_start(void);

/**
 * @brief 停止Web服务器
 * @return ESP_OK 成功
 */
esp_err_t web_server_stop(void);

/**
 * @brief 设置Web服务器配置
 * @param config 配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t web_server_set_config(const web_server_config_t *config);

/**
 * @brief 获取Web服务器配置
 * @param config 输出：配置结构体指针
 * @return ESP_OK 成功
 */
esp_err_t web_server_get_config(web_server_config_t *config);

// ==================== API接口 ====================

/**
 * @brief 注册API处理程序
 * @param uri URI路径
 * @param handler 处理函数
 * @param method HTTP方法
 * @return ESP_OK 成功
 */
esp_err_t web_server_register_api_handler(const char *uri, esp_err_t (*handler)(httpd_req_t *r), httpd_method_t method);

/**
 * @brief 注销API处理程序
 * @param uri URI路径
 * @return ESP_OK 成功
 */
esp_err_t web_server_unregister_api_handler(const char *uri);

// ==================== WebSocket接口 ====================

/**
 * @brief 发送WebSocket消息
 * @param fd 文件描述符
 * @param data 数据
 * @param len 数据长度
 * @return ESP_OK 成功
 */
esp_err_t web_server_ws_send(int fd, const char *data, size_t len);

/**
 * @brief 广播WebSocket消息
 * @param data 数据
 * @param len 数据长度
 * @return ESP_OK 成功
 */
esp_err_t web_server_ws_broadcast(const char *data, size_t len);

/**
 * @brief 注册WebSocket消息回调
 * @param callback 回调函数
 * @return ESP_OK 成功
 */
esp_err_t web_server_register_ws_callback(esp_err_t (*callback)(const char *data, size_t len));

// ==================== 状态查询 ====================

/**
 * @brief 检查Web服务器是否运行中
 * @return true 运行中，false 已停止
 */
bool web_server_is_running(void);

/**
 * @brief 获取Web服务器状态信息
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return ESP_OK 成功
 */
esp_err_t web_server_get_status(char *buffer, size_t buffer_size);

// ==================== 配置存储 ====================

/**
 * @brief 保存Web服务器配置到NVS
 * @return ESP_OK 成功
 */
esp_err_t web_server_save_config(void);

/**
 * @brief 从NVS加载Web服务器配置
 * @return ESP_OK 成功
 */
esp_err_t web_server_load_config(void);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
