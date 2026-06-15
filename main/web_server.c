/**
 * @file web_server.c
 * @brief Web服务器模块实现
 * @note 提供网页控制界面和RESTful API
 */

#include "web_server.h"
#include "water_purifier_fsm.h"
#include "tds_sensor.h"
#include "filter_manager.h"
#include "gpio_driver.h"
#include "wifi_manager.h"
#include "config_manager.h"
#include "pm_manager.h"
#include "history_logger.h"
#include "ota_update.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WEB";

// 系统启动时间（微秒），在编译时记录
static uint64_t g_boot_time = 0;

// 获取系统运行时间（秒），使用uint64防止49.7天回绕
static uint64_t get_uptime_sec(void)
{
    if (g_boot_time == 0) {
        g_boot_time = esp_timer_get_time();
    }
    return (uint64_t)((esp_timer_get_time() - g_boot_time) / 1000000);
}

// ==================== 私有变量 ====================

static struct {
    bool initialized;
    bool running;
    web_server_config_t config;
    httpd_handle_t server;
} ctx = {
    .initialized = false,
    .running = false,
    .config = { .port = 80, .enable_auth = false },
    .server = NULL,
};

// 防抖时间戳（微秒）
static uint64_t s_last_control_time = 0;     // 上次控制操作时间
static uint64_t s_last_wifi_scan_time = 0;   // 上次WiFi扫描时间
#define CONTROL_DEBOUNCE_MS 1000             // 控制按钮防抖间隔（1秒）
#define WIFI_SCAN_DEBOUNCE_MS 5000           // WiFi扫描防抖间隔（5秒）

// ==================== Basic Auth + Session ====================

#define SESSION_TIMEOUT_SEC 86400  // Session有效期24小时

static portMUX_TYPE session_spinlock = portMUX_INITIALIZER_UNLOCKED;

static struct {
    char token[33];           // hex session token (32 chars + null)
    uint64_t create_time_us;  // 创建时间
    bool valid;               // 是否有效
} session_ctx = {.valid = false};

// ==================== 日志拦截器 ====================

#define LOG_BUF_SIZE 8192
static char s_log_buf[LOG_BUF_SIZE];
static volatile uint32_t s_log_head = 0;
static volatile uint32_t s_log_tail = 0;
static volatile uint32_t s_log_generation = 0;  // 生成计数器，用于检测缓冲区覆盖
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_original_vprintf = NULL;

static void log_buf_write(const char *data, size_t len)
{
    taskENTER_CRITICAL(&s_log_lock);
    for (size_t i = 0; i < len; i++) {
        s_log_buf[s_log_head] = data[i];
        s_log_head = (s_log_head + 1) % LOG_BUF_SIZE;
        if (s_log_head == s_log_tail) {
            s_log_tail = (s_log_tail + 1) % LOG_BUF_SIZE;
            s_log_generation++;  // 缓冲区覆盖，递增生成计数器
        }
    }
    taskEXIT_CRITICAL(&s_log_lock);
}

static int log_vprintf_hook(const char *fmt, va_list args)
{
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if (len > 0) {
        // vsnprintf返回理论长度（如果缓冲区足够大），实际写入最多sizeof(buf)-1字节
        // 如果len >= sizeof(buf)，说明输出被截断，只写入sizeof(buf)-1字节（不含'\0'）
        if (len >= (int)sizeof(buf)) {
            len = (int)sizeof(buf) - 1;
        }
        log_buf_write(buf, len);
    }
    if (s_original_vprintf) {
        return s_original_vprintf(fmt, args);
    }
    return 0;
}

esp_err_t web_server_init_log_interceptor(void)
{
    if (s_original_vprintf != NULL) return ESP_OK; // 已初始化
    s_original_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    ESP_LOGI("WEB", "日志拦截器已启动");
    return ESP_OK;
}

// ==================== HTML页面 ====================

// ==================== 统一CSS样式（使用宏定义支持字符串拼接）====================

#define SHARED_CSS \
"*{box-sizing:border-box}" \
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh}" \
".container{max-width:600px;margin:0 auto}" \
".card{background:rgba(255,255,255,0.95);padding:20px;margin:15px 0;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.1)}" \
"h1{color:#fff;text-align:center;margin-bottom:20px;text-shadow:0 2px 4px rgba(0,0,0,0.2)}" \
"h3{color:#333;margin:0 0 15px 0;padding-bottom:10px;border-bottom:2px solid #eee}" \
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}" \
".stat{background:#f8f9fa;padding:12px;border-radius:12px;text-align:center}" \
".stat-label{color:#666;font-size:11px;margin-bottom:4px}" \
".stat-value{color:#333;font-size:20px;font-weight:600}" \
".stat-value.good{color:#28a745}.stat-value.warn{color:#ffc107}.stat-value.error{color:#dc3545}" \
".btn{padding:12px 20px;margin:5px;border:none;border-radius:12px;cursor:pointer;color:#fff;font-size:14px;font-weight:500;transition:all .2s}" \
".btn:hover{transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,0.2)}" \
".btn-group{display:flex;flex-wrap:wrap;gap:8px}" \
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2)}" \
".btn-success{background:linear-gradient(135deg,#28a745,#20c997)}" \
".btn-danger{background:linear-gradient(135deg,#dc3545,#c82333)}" \
".btn-warning{background:linear-gradient(135deg,#ffc107,#fd7e14)}" \
".btn-info{background:linear-gradient(135deg,#17a2b8,#20c997)}" \
".btn-dark{background:linear-gradient(135deg,#343a40,#495057)}" \
"input,select{padding:10px;border:2px solid #e0e0e0;border-radius:8px;font-size:14px;transition:border-color .2s;width:100%}" \
"input:focus,select:focus{outline:none;border-color:#667eea}" \
".form-row{margin:10px 0}" \
".form-row label{display:block;color:#555;margin-bottom:5px;font-size:13px}" \
".form-row small{color:#999;font-size:11px}" \
".back-btn{background:rgba(255,255,255,0.2);color:#fff;padding:10px 20px;border:none;border-radius:10px;cursor:pointer;font-size:14px;margin-bottom:10px}" \
".back-btn:hover{background:rgba(255,255,255,0.3)}" \
".filter-bar{height:8px;background:#e0e0e0;border-radius:4px;margin:4px 0;overflow:hidden}" \
".filter-bar-fill{height:100%;border-radius:4px;transition:width .3s}" \
".filter-bar-fill.good{background:linear-gradient(90deg,#28a745,#20c997)}" \
".filter-bar-fill.warn{background:linear-gradient(90deg,#ffc107,#fd7e14)}" \
".filter-bar-fill.error{background:linear-gradient(90deg,#dc3545,#c82333)}" \
".filter-stat{text-align:center;padding:10px 5px}" \
".filter-name{font-size:12px;color:#666;margin-bottom:4px}" \
".filter-pct{font-size:18px;font-weight:600;margin-bottom:2px}" \
".dim-label{font-size:10px;color:#999}" \
".wifi-item{background:#f0f0f0;padding:10px;margin:5px 0;border-radius:8px;cursor:pointer;transition:background .2s}" \
".wifi-item:hover{background:#e0e0e0}" \
".m0{margin:0}" \
".fs14{font-size:14px}" \
".btn-center{display:block;margin:20px auto;width:80%}" \
".sel-auto{width:auto}"

// 日志页特有样式
#define LOG_CSS \
".control-panel{display:flex;flex-wrap:wrap;gap:12px;align-items:center;padding:10px;background:#f8f9fa;border-radius:12px;margin-bottom:10px}" \
".control-panel>*{flex-shrink:0}" \
".toggle-label{display:flex;align-items:center;gap:8px;font-size:14px;color:#333;cursor:pointer}" \
".toggle-label input[type='checkbox']{width:18px;height:18px;cursor:pointer}" \
"#logContent{background:#f8f9fa;color:#333;font-family:'Cascadia Code','Fira Code',monospace;font-size:12px;padding:12px;border-radius:8px;white-space:pre-wrap;word-break:break-all;line-height:1.6;overflow-y:auto;max-height:65vh;border:1px solid #e0e0e0}" \
".E{color:#d32f2f;font-weight:bold}.W{color:#f57c00}.I{color:#388e3c}.D{color:#1976d2}"

// ==================== HTML页面定义 ====================

// 首页 - 只读显示
static const char html_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>净水器状态</title>"
"<style>" SHARED_CSS "</style></head><body>"
"<div class='container'>"
"<h1>净水器</h1>"

"<div class='card'><h3>系统状态</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>运行状态</div><div id='state' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>进水TDS</div><div id='tds_in' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>出水TDS</div><div id='tds_out' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>去除率</div><div id='rate' class='stat-value good'>-</div></div>"
"<div class='stat'><div class='stat-label'>漏水检测</div><div id='leak' class='stat-value good'>正常</div></div>"
"</div>"
"<hr style='border:none;border-top:1px solid #eee;margin:12px 0'>"
"<div style='text-align:right;font-size:13px;color:#999'>"
"<span id='sntpTime'>-</span>"
"</div>"
"</div>"

"<div class='card'><h3>滤芯寿命</h3>"
"<div id='filters'>"
"<div style='margin:10px 0'><div style='display:flex;justify-content:space-between;font-size:13px'><span>PP棉</span><span id='f0'>-</span></div><div class='filter-bar'><div id='b0' class='filter-bar-fill good' style='width:100%'></div></div></div>"
"<div style='margin:10px 0'><div style='display:flex;justify-content:space-between;font-size:13px'><span>颗粒活性炭</span><span id='f1'>-</span></div><div class='filter-bar'><div id='b1' class='filter-bar-fill good' style='width:100%'></div></div></div>"
"<div style='margin:10px 0'><div style='display:flex;justify-content:space-between;font-size:13px'><span>压缩活性炭</span><span id='f2'>-</span></div><div class='filter-bar'><div id='b2' class='filter-bar-fill good' style='width:100%'></div></div></div>"
"<div style='margin:10px 0'><div style='display:flex;justify-content:space-between;font-size:13px'><span>RO膜</span><span id='f3'>-</span></div><div class='filter-bar'><div id='b3' class='filter-bar-fill good' style='width:100%'></div></div></div>"
"<div style='margin:10px 0'><div style='display:flex;justify-content:space-between;font-size:13px'><span>后置活性炭</span><span id='f4'>-</span></div><div class='filter-bar'><div id='b4' class='filter-bar-fill good' style='width:100%'></div></div></div>"
"</div>"
"<div style='display:flex;justify-content:space-between;color:#666;font-size:12px;margin-top:10px;padding:0 10px'>"
"<span>总用水量: <span id='totalWater'>0</span> 升</span>"
"<span>总制水量: <span id='prodWater'>0</span> 升</span>"
"</div>"
"</div>"

"<div class='card'><h3>运行统计</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>运行时间</div><div id='uptime' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>制水周期</div><div id='cycles' class='stat-value'>0</div></div>"
"<div class='stat'><div class='stat-label'>冲洗周期</div><div id='flushCycles' class='stat-value'>0</div></div>"
"<div class='stat'><div class='stat-label'>总制水时间</div><div id='prodTime' class='stat-value'>0h</div></div>"
"<div class='stat'><div class='stat-label'>今日制水</div><div id='todayProd' class='stat-value'>0min</div></div>"
"</div></div>"

"<div class='card'><h3>网络信息</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>WiFi状态</div><div id='wifiState' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>IP地址</div><div id='wifiIP' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>信号强度</div><div id='wifiRSSI' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>发射功率</div><div id='wifiTXPower' class='stat-value'>-</div></div>"
"</div></div>"

"<div class='card'><h3>系统状态</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>可用内存</div><div id='freeHeap' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>历史最低</div><div id='minHeap' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>FSM栈</div><div id='stackFSM' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>HTTP栈</div><div id='stackHTTP' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>TDS栈</div><div id='stackTDS' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>监控栈</div><div id='stackMonitor' class='stat-value'>-</div></div>"
"</div></div>"

"<button class='btn btn-primary btn-center' onclick=\"location.href='/admin'\">管理设置</button>"

"</div>"
"<script>"
"function $(id){return document.getElementById(id)}"
"function formatSize(b){if(!b||b<=0)return'-';return b<1024?b+'B':b<1048576?(b/1024).toFixed(1)+'KB':(b/1048576).toFixed(2)+'MB';}"
"function update(){fetch('/api/status').then(r=>r.json()).then(d=>{"
"$('state').textContent=d.state;"
"$('state').className='stat-value '+(d.state==='制水'?'good':d.state==='停止'?'error':d.state==='缺水'?'warn':'');"
"$('tds_in').textContent=d.tds_in?d.tds_in.toFixed(1)+' ppm':'-';"
"$('tds_out').textContent=d.tds_out?d.tds_out.toFixed(1)+' ppm':'-';"
"$('rate').textContent=d.rate?d.rate.toFixed(1)+'%':'-';"
"$('rate').className='stat-value '+(d.rate>90?'good':d.rate>70?'warn':'error');"
"$('leak').textContent=d.leak?'⚠ 报警':'✓ 正常';"
"$('leak').className='stat-value '+(d.leak?'error':'good');"
"$('sntpTime').textContent=d.sntpTime||'未同步';"
"$('sntpTime').style.color=d.sntpTime?'#28a745':'#ffc107';"
"if(d.filters){for(let i=0;i<5;i++){const f=d.filters[i];if(f){$('f'+i).textContent=f.effPct+'%';$('b'+i).style.width=f.effPct+'%';$('b'+i).className='filter-bar-fill '+(f.effPct>50?'good':f.effPct>20?'warn':'error');}}}"
"$('totalWater').textContent=d.totalWater||0;"
"$('prodWater').textContent=d.prodWater||0;"
"$('cycles').textContent=d.cycles||0;"
"$('flushCycles').textContent=d.flushes||0;"
"$('prodTime').textContent=d.prodTime?(d.prodTime/3600).toFixed(1)+'h':'0h';"
"$('todayProd').textContent=d.todayProd?d.todayProd+'min':'0min';"
"var u=d.uptime||0,h=Math.floor(u/3600),m=Math.floor((u%3600)/60);if(h>=24){$('uptime').textContent=Math.floor(h/24)+'天'+(h%24)+'时';}else if(h>0){$('uptime').textContent=h+'时'+m+'分';}else{$('uptime').textContent=m+'分';}"
"$('wifiState').textContent=d.wifiState;"
"$('wifiIP').textContent=d.ip||'-';"
"$('wifiRSSI').textContent=d.rssi?d.rssi+'dBm':'-';"
"$('wifiRSSI').className='stat-value '+((d.rssi&&d.rssi>-60)?'good':(d.rssi&&d.rssi>-75)?'warn':'error');"
"$('wifiTXPower').textContent=d.txPower?d.txPower+'dBm':'-';"
"$('freeHeap').textContent=formatSize(d.freeHeap);"
"$('freeHeap').className='stat-value '+((d.freeHeap&&d.freeHeap>30720)?'good':(d.freeHeap&&d.freeHeap>15360)?'warn':'error');"
"$('minHeap').textContent=formatSize(d.minHeap);"
"$('minHeap').className='stat-value '+((d.minHeap&&d.minHeap>30720)?'good':(d.minHeap&&d.minHeap>15360)?'warn':'error');"
"$('stackFSM').textContent=formatSize(d.stackFSM);"
"$('stackFSM').className='stat-value '+((d.stackFSM&&d.stackFSM>512)?'good':(d.stackFSM&&d.stackFSM>256)?'warn':'error');"
"$('stackHTTP').textContent=formatSize(d.stackHTTP);"
"$('stackHTTP').className='stat-value '+((d.stackHTTP&&d.stackHTTP>2048)?'good':(d.stackHTTP&&d.stackHTTP>1024)?'warn':'error');"
"$('stackTDS').textContent=formatSize(d.stackTDS);"
"$('stackTDS').className='stat-value '+((d.stackTDS&&d.stackTDS>512)?'good':(d.stackTDS&&d.stackTDS>256)?'warn':'error');"
"$('stackMonitor').textContent=formatSize(d.stackMonitor);"
"$('stackMonitor').className='stat-value '+((d.stackMonitor&&d.stackMonitor>512)?'good':(d.stackMonitor&&d.stackMonitor>256)?'warn':'error');"
"})}"
"setInterval(update,3000);update();"
"</script></body></html>";

// 管理页面 - 配置和控制
static const char html_admin_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>净水器管理</title>"
"<style>" SHARED_CSS "</style></head><body>"
"<div class='container'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'>"
"<button class='back-btn m0' onclick=\"location.href='/'\">← 返回首页</button>"
"<button class='back-btn m0' onclick=\"location.href='/logs'\">日志 →</button>"
"<button class='back-btn m0' onclick=\"location.href='/ota'\">固件升级 →</button>"
"</div>"
"<h1>管理页面</h1>"

"<div class='card'><h3>控制面板</h3>"
"<div class='btn-group'>"
"<button class='btn btn-primary' onclick='normalFlush()'>常规冲洗</button>"
"<button class='btn btn-info' onclick='pureFlush()'>纯水洗膜</button>"
"<button class='btn btn-warning' onclick='filterFlush()'>换芯冲洗</button>"
"<button class='btn btn-danger' onclick='shutdown()'>停止</button>"
"<button class='btn btn-success' onclick='resetStop()'>复位</button>"
"<button class='btn btn-dark' onclick='goStandby()'>待机</button>"
"<button class='btn btn-danger' onclick='reboot()'>重启</button>"
"</div></div>"

"<div class='card'><h3>滤芯管理</h3>"
"<div class='grid'>"
"<div class='stat filter-stat'><div class='filter-name'>PP棉</div><div id='f0' class='filter-pct'>-</div><div class='dim-label'>水量 <span id='fw0'>-</span> 时间 <span id='ft0'>-</span></div><div class='filter-bar'><div id='bw0' class='filter-bar-fill good' style='width:100%'></div></div><div class='filter-bar'><div id='bt0' class='filter-bar-fill good' style='width:100%'></div></div><button class='btn btn-primary' style='font-size:11px;padding:4px 8px;margin-top:5px' onclick=\"resetFilter(0)\">重置</button></div>"
"<div class='stat filter-stat'><div class='filter-name'>颗粒炭</div><div id='f1' class='filter-pct'>-</div><div class='dim-label'>水量 <span id='fw1'>-</span> 时间 <span id='ft1'>-</span></div><div class='filter-bar'><div id='bw1' class='filter-bar-fill good' style='width:100%'></div></div><div class='filter-bar'><div id='bt1' class='filter-bar-fill good' style='width:100%'></div></div><button class='btn btn-primary' style='font-size:11px;padding:4px 8px;margin-top:5px' onclick=\"resetFilter(1)\">重置</button></div>"
"<div class='stat filter-stat'><div class='filter-name'>压缩炭</div><div id='f2' class='filter-pct'>-</div><div class='dim-label'>水量 <span id='fw2'>-</span> 时间 <span id='ft2'>-</span></div><div class='filter-bar'><div id='bw2' class='filter-bar-fill good' style='width:100%'></div></div><div class='filter-bar'><div id='bt2' class='filter-bar-fill good' style='width:100%'></div></div><button class='btn btn-primary' style='font-size:11px;padding:4px 8px;margin-top:5px' onclick=\"resetFilter(2)\">重置</button></div>"
"<div class='stat filter-stat'><div class='filter-name'>RO膜</div><div id='f3' class='filter-pct'>-</div><div class='dim-label'>水量 <span id='fw3'>-</span> 时间 <span id='ft3'>-</span></div><div class='filter-bar'><div id='bw3' class='filter-bar-fill good' style='width:100%'></div></div><div class='filter-bar'><div id='bt3' class='filter-bar-fill good' style='width:100%'></div></div><button class='btn btn-primary' style='font-size:11px;padding:4px 8px;margin-top:5px' onclick=\"resetFilter(3)\">重置</button></div>"
"<div class='stat filter-stat'><div class='filter-name'>后置炭</div><div id='f4' class='filter-pct'>-</div><div class='dim-label'>水量 <span id='fw4'>-</span> 时间 <span id='ft4'>-</span></div><div class='filter-bar'><div id='bw4' class='filter-bar-fill good' style='width:100%'></div></div><div class='filter-bar'><div id='bt4' class='filter-bar-fill good' style='width:100%'></div></div><button class='btn btn-primary' style='font-size:11px;padding:4px 8px;margin-top:5px' onclick=\"resetFilter(4)\">重置</button></div>"
"</div>"
"<p style='font-size:13px;color:#666;margin-top:15px'>滤芯水量寿命 (升):</p>"
"<div class='grid'>"
"<div class='form-row'><label>PP棉</label><select id='cap0'><option value='1000'>1000</option><option value='2000'>2000</option><option value='3000'>3000</option><option value='4000'>4000</option><option value='5000'>5000</option></select></div>"
"<div class='form-row'><label>颗粒炭</label><select id='cap1'><option value='2000'>2000</option><option value='3000'>3000</option><option value='4000'>4000</option><option value='5000'>5000</option><option value='6000'>6000</option><option value='8000'>8000</option><option value='10000'>10000</option></select></div>"
"<div class='form-row'><label>压缩炭</label><select id='cap2'><option value='2000'>2000</option><option value='3000'>3000</option><option value='4000'>4000</option><option value='5000'>5000</option><option value='6000'>6000</option><option value='8000'>8000</option><option value='10000'>10000</option></select></div>"
"<div class='form-row'><label>RO膜</label><select id='cap3'><option value='5000'>5000</option><option value='6000'>6000</option><option value='8000'>8000</option><option value='10000'>10000</option><option value='12000'>12000</option><option value='15000'>15000</option><option value='20000'>20000</option></select></div>"
"<div class='form-row'><label>后置炭</label><select id='cap4'><option value='1000'>1000</option><option value='2000'>2000</option><option value='3000'>3000</option><option value='4000'>4000</option><option value='5000'>5000</option></select></div>"
"</div>"
"<p style='font-size:13px;color:#666;margin-top:15px'>滤芯日历寿命 (月):</p>"
"<div class='grid'>"
"<div class='form-row'><label>PP棉</label><select id='time0'><option value='730'>1月</option><option value='1460'>2月</option><option value='2190'>3月</option><option value='2920'>4月</option><option value='3650'>5月</option><option value='4380'>6月</option></select></div>"
"<div class='form-row'><label>颗粒炭</label><select id='time1'><option value='2190'>3月</option><option value='2920'>4月</option><option value='3650'>5月</option><option value='4380'>6月</option><option value='5110'>7月</option><option value='5840'>8月</option><option value='6570'>9月</option></select></div>"
"<div class='form-row'><label>压缩炭</label><select id='time2'><option value='2190'>3月</option><option value='2920'>4月</option><option value='3650'>5月</option><option value='4380'>6月</option><option value='5110'>7月</option><option value='5840'>8月</option><option value='6570'>9月</option></select></div>"
"<div class='form-row'><label>RO膜</label><select id='time3'><option value='13140'>18月</option><option value='15330'>21月</option><option value='17520'>24月</option><option value='21900'>30月</option><option value='26280'>36月</option></select></div>"
"<div class='form-row'><label>后置炭</label><select id='time4'><option value='4380'>6月</option><option value='5110'>7月</option><option value='5840'>8月</option><option value='6570'>9月</option><option value='7300'>10月</option><option value='8030'>11月</option><option value='8760'>12月</option></select></div>"
"</div>"
"<div class='btn-group'><button class='btn btn-success' onclick='saveFilterCaps()'>保存滤芯配置</button></div>"
"</div>"

"<div class='card'><h3>硬件配置</h3>"
"<div class='form-row'><label>RO膜通量</label><select id='roMem'><option value='0'>汇通50G (7.8L/h)</option><option value='1'>汇通75G (12.0L/h)</option><option value='2'>汇通100G (15.6L/h)</option><option value='3'>汇通200G (31.2L/h)</option><option value='4'>汇通400G (62.4L/h)</option></select></div>"
"<div class='form-row'><label>增压泵</label><select id='pumpType'><option value='0'>三角洲50G 70psi 0.55L/min</option><option value='1'>三角洲75G 70psi 0.85L/min</option><option value='2'>三角洲100G 70psi 1.1L/min</option><option value='3'>三角洲200G 70psi 1.6L/min</option><option value='4'>三角洲300G 70psi 2.0L/min</option><option value='5'>三角洲400G 70psi 2.5L/min</option></select></div>"
"<div class='form-row'><label>压力桶大小</label><select id='tankSize'><option value='0'>3G (11.4L)</option><option value='1'>3.2G (12.1L)</option><option value='2'>4G (15.1L)</option><option value='3'>6G (22.7L)</option><option value='4'>10G (37.9L)</option></select></div>"
"<div class='form-row'><label>废水阀流量</label><select id='wasteFlow'><option value='200'>200CC (12L/h)</option><option value='300'>300CC (18L/h)</option><option value='450'>450CC (27L/h)</option><option value='550'>550CC (33L/h)</option></select></div>"
"<div class='btn-group'><button class='btn btn-success' onclick='saveHardware()'>保存硬件配置</button></div>"
"</div>"

"<div class='card'><h3>系统配置</h3>"
"<div class='form-row'><label>常规冲洗时间 (秒)</label><input type='number' id='normalFlushDur' value='30'></div>"
"<div class='form-row'><label>纯水洗膜时间 (秒)</label><input type='number' id='pureFlushDur' value='15'></div>"
"<div class='form-row'><label>换芯冲洗时间 (分钟)</label><input type='number' id='filterFlushDur' value='60'></div>"
"<div class='form-row'><label>制水超时 (分钟)</label><input type='number' id='prodTimeout' value='180'></div>"
"<div class='form-row'><label>漏水确认 (秒)</label><input type='number' id='leakConfirm' value='5'></div>"
"<div class='form-row'><label>Flash保存周期</label><select id='saveInterval'><option value='10'>10分钟</option><option value='60'>1小时</option><option value='120'>2小时 (默认)</option><option value='240'>4小时</option><option value='360'>6小时</option><option value='720'>12小时</option><option value='1440'>24小时</option></select></div>"
"<div class='form-row'><label>继电器触发电平</label><select id='relayLevel'><option value='0'>低电平触发</option><option value='1'>高电平触发</option></select></div>"
"<div class='form-row'><label>TDS进水阈值 (ppm)</label><input type='number' id='tdsInTh' value='500'></div>"
"<div class='form-row'><label>TDS出水阈值 (ppm)</label><input type='number' id='tdsOutTh' value='50'></div>"
"<div class='btn-group'><button class='btn btn-primary' onclick='saveConfig()'>保存系统配置</button></div>"
"</div>"

"<div class='card'><h3>水锤效应控制</h3>"
"<p style='color:#888;font-size:12px'>调节阀门和泵的开关顺序延时，减少水锤对管路和RO膜的冲击</p>"
"<div style='background:#f0f7ff;border:1px solid #d0e3ff;padding:12px;border-radius:8px;margin:12px 0;font-size:12px;line-height:1.9'>"
"<b style='color:#667eea'>▶ 制水启动</b><br>"
"&nbsp;&nbsp;&nbsp;&nbsp;开进水阀 → <b>开阀延时</b> → 开增压泵（废水阀关闭）<br>"
"<b style='color:#667eea'>▶ 冲洗启动</b><br>"
"&nbsp;&nbsp;&nbsp;&nbsp;开进水阀 → <b>开阀延时</b> → 开废水阀+增压泵<br>"
"<b style='color:#e67e22'>■ 冲洗→反冲洗过渡</b><br>"
"&nbsp;&nbsp;&nbsp;&nbsp;停泵 → <b>停泵延时</b> → 关进水阀 → <b>关阀延时</b> → 开回水阀+废水阀<br>"
"</div>"
"<div class='form-row'><label>▶ 开阀延时 (毫秒)</label><input type='number' id='whValveOpen' value='1000'><small style='display:block;color:#888;margin-top:2px'>开进水阀后等待，制水时再开泵，冲洗时再开废水阀+泵</small></div>"
"<div class='form-row'><label>■ 停泵延时 (毫秒)</label><input type='number' id='whPumpStop' value='1000'><small style='display:block;color:#888;margin-top:2px'>停泵后等待，再关进水阀，防止水流回流</small></div>"
"<div class='form-row'><label>■ 关阀延时 (毫秒)</label><input type='number' id='whValveClose' value='500'><small style='display:block;color:#888;margin-top:2px'>关进水阀后等待，再开回水阀+废水阀</small></div>"
"<div class='btn-group'><button class='btn btn-primary' onclick='saveWaterHammer()'>保存水锤配置</button></div>"
"</div>"

"<div class='card'><h3>TDS校准</h3>"
"<p style='color:#888;font-size:12px'>将传感器放入标准液或同一杯水中，输入标准值进行校准</p>"
"<div class='form-row'><label>进水TDS标准值 (ppm)</label><input type='number' id='tdsInCal' placeholder='输入标准值'></div>"
"<div class='form-row'><label>出水TDS标准值 (ppm)</label><input type='number' id='tdsOutCal' placeholder='输入标准值'></div>"
"<div class='btn-group'><button class='btn btn-warning' onclick='calibrateTDS(0)'>校准进水TDS</button><button class='btn btn-warning' onclick='calibrateTDS(1)'>校准出水TDS</button></div>"
"</div>"

"<div class='card'><h3>WiFi配置</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>状态</div><div id='wifiState' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>SSID</div><div id='wifiSSID' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>IP地址</div><div id='wifiIP' class='stat-value'>-</div></div>"
"</div>"
"<div class='btn-group'><button class='btn btn-info' onclick='scanWiFi()'>扫描网络</button></div>"
"<div id='wifiList'></div>"
"<div class='form-row' style='display:flex;gap:10px;flex-wrap:wrap'>"
"<input id='ssid' placeholder='WiFi名称' style='flex:1;min-width:100px'>"
"<input id='pass' type='password' placeholder='密码' style='flex:1;min-width:100px'>"
"</div>"
"<div class='btn-group'><button class='btn btn-success' onclick='saveWiFi()'>连接WiFi</button></div>"
"</div>"

"<div class='card'><h3>MQTT配置</h3>"
"<div class='form-row'><label>启用MQTT</label><select id='mqttEn'><option value='0'>禁用</option><option value='1'>启用</option></select></div>"
"<div class='form-row'><label>Broker地址</label><input type='text' id='mqttBroker' placeholder='mqtt://homeassistant.local:1883'></div>"
"<div class='form-row'><label>用户名</label><input type='text' id='mqttUser' placeholder='留空表示无认证'></div>"
"<div class='form-row'><label>密码</label><input type='password' id='mqttPass' placeholder='留空表示无认证'></div>"
"<div class='form-row'><label>主题前缀</label><input type='text' id='mqttPrefix' value='water-purifier'></div>"
"<div class='btn-group'><button class='btn btn-primary' onclick='saveMQTT()'>保存MQTT配置</button></div>"
"</div>"

"<div class='card'><h3>Web认证配置</h3>"
"<p style='color:#888;font-size:12px;margin-bottom:10px'>启用后，管理页面、OTA升级、系统日志需要登录认证。首页和WiFi配网无需认证。</p>"
"<div class='form-row'><label>启用认证</label><select id='webAuthEn'><option value='0'>禁用</option><option value='1'>启用</option></select></div>"
"<div class='form-row'><label>用户名</label><input type='text' id='webUser' placeholder='输入用户名' maxlength='31'></div>"
"<div class='form-row'><label>密码</label><input type='password' id='webPass' placeholder='输入密码' maxlength='31'></div>"
"<div class='btn-group'><button class='btn btn-warning' onclick='saveWebAuth()'>保存认证配置</button></div>"
"</div>"

"</div>"
"<script>"
"function $(id){return document.getElementById(id)}"
"function api(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)}).then(r=>r.json())}"
"function loadConfig(){fetch('/api/config').then(r=>r.json()).then(d=>{"
"$('normalFlushDur').value=d.normalFlushDur;"
"$('pureFlushDur').value=d.pureFlushDur;"
"$('filterFlushDur').value=d.filterFlushDur;"
"$('prodTimeout').value=d.prodTimeout;"
"$('leakConfirm').value=d.leakConfirm||5;"
"$('saveInterval').value=d.saveInterval||120;"
"$('relayLevel').value=d.relayLevel;"
"$('tdsInTh').value=d.tdsInTh;"
"$('tdsOutTh').value=d.tdsOutTh;"
"$('roMem').value=d.roMem;"
"$('pumpType').value=d.pumpType;"
"$('tankSize').value=d.tankSize;"
"$('wasteFlow').value=d.wasteFlow;"
"$('whValveOpen').value=d.whValveOpen||1000;"
"$('whPumpStop').value=d.whPumpStop||1000;"
"$('whValveClose').value=d.whValveClose||500;"
"})}"
"function loadFilters(){fetch('/api/status').then(r=>r.json()).then(d=>{"
"function selVal(id,val){let s=$(id),best=s.options[0].value,diff=Math.abs(val-best);for(let i=1;i<s.options.length;i++){let dd=Math.abs(s.options[i].value-val);if(dd<diff){diff=dd;best=s.options[i].value}};s.value=best}"
"if(d.filters){for(let i=0;i<5;i++){const f=d.filters[i];if(f){$('f'+i).textContent=f.effPct+'%';$('fw'+i).textContent=f.waterPct+'%';$('ft'+i).textContent=f.timePct+'%';$('bw'+i).style.width=f.waterPct+'%';$('bt'+i).style.width=f.timePct+'%';$('bw'+i).className='filter-bar-fill '+(f.waterPct>50?'good':f.waterPct>20?'warn':'error');$('bt'+i).className='filter-bar-fill '+(f.timePct>50?'good':f.timePct>20?'warn':'error');selVal('cap'+i,f.total);selVal('time'+i,f.timeLimit);}}}"
"$('wifiState').textContent=d.wifiState;"
"$('wifiSSID').textContent=d.ssid||'-';"
"$('wifiIP').textContent=d.ip||'-';"
"})}"
"function normalFlush(){api('/api/control',{action:'normal_flush'}).then(d=>alert(d.status||'已执行'))}"
"function pureFlush(){api('/api/control',{action:'pure_flush'}).then(d=>alert(d.status||'已执行'))}"
"function filterFlush(){if(confirm('确认启动换芯冲洗？将连续冲洗'+($('filterFlushDur').value)+'分钟。')){api('/api/control',{action:'filter_flush'}).then(d=>alert(d.status||'已执行'))}}"
"function resetStop(){api('/api/control',{action:'reset'}).then(d=>alert(d.status||'已执行'))}"
"function goStandby(){api('/api/control',{action:'standby'}).then(d=>alert(d.status||'已执行'))}"
"function shutdown(){api('/api/control',{action:'shutdown'}).then(d=>alert(d.status||'已执行'))}"
"function reboot(){if(confirm('确认重启设备？重启期间服务将暂时中断。')){api('/api/control',{action:'reboot'}).then(d=>alert(d.status||'重启中...'))}}"
"function resetFilter(i){if(confirm('确认重置该滤芯？')){api('/api/filter/reset',{filter:i}).then(d=>{alert(d.filter_name+' 已重置');loadFilters()})}}"
"function saveFilterCaps(){api('/api/filter/capacity',{caps:[parseInt($('cap0').value),parseInt($('cap1').value),parseInt($('cap2').value),parseInt($('cap3').value),parseInt($('cap4').value)],times:[parseInt($('time0').value),parseInt($('time1').value),parseInt($('time2').value),parseInt($('time3').value),parseInt($('time4').value)]}).then(d=>alert(d.status||'已保存'))}"
"function saveConfig(){api('/api/config',{normalFlushDur:parseInt($('normalFlushDur').value),pureFlushDur:parseInt($('pureFlushDur').value),filterFlushDur:parseInt($('filterFlushDur').value)*60,prodTimeout:parseInt($('prodTimeout').value)*60,leakConfirm:parseInt($('leakConfirm').value),saveInterval:parseInt($('saveInterval').value),relayLevel:parseInt($('relayLevel').value),tdsInTh:parseFloat($('tdsInTh').value),tdsOutTh:parseFloat($('tdsOutTh').value),whValveOpen:parseInt($('whValveOpen').value),whPumpStop:parseInt($('whPumpStop').value),whValveClose:parseInt($('whValveClose').value)}).then(d=>alert(d.status||'已保存'))}"
"function saveHardware(){api('/api/config/hardware',{roMem:parseInt($('roMem').value),pumpType:parseInt($('pumpType').value),tankSize:parseInt($('tankSize').value),wasteFlow:parseInt($('wasteFlow').value)}).then(d=>alert(d.status||'已保存'))}"
"function scanWiFi(){$('wifiList').innerHTML='扫描中...';fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{let h='';if(d.networks)d.networks.forEach(function(n){let s=encodeURIComponent(n.ssid);h+='<div class=\"wifi-item\" data-ssid=\"'+s+'\">'+n.ssid+' ('+n.rssi+'dBm)</div>'});$('wifiList').innerHTML=h||'未找到网络'})}"
"$('wifiList').addEventListener('click',function(e){var item=e.target.closest('.wifi-item');if(item){$('ssid').value=decodeURIComponent(item.getAttribute('data-ssid'))}});"
"function saveWiFi(){api('/api/wifi',{ssid:$('ssid').value,password:$('pass').value}).then(d=>alert(d.status))}"
"function calibrateTDS(sensor){const v=sensor===0?$('tdsInCal').value:$('tdsOutCal').value;if(!v)return alert('请输入标准值');api('/api/tds/calibrate',{sensor:parseInt(sensor),value:parseFloat(v)}).then(d=>alert(d.status||'校准完成'))}"
"function loadMQTT(){fetch('/api/mqtt/config').then(r=>r.json()).then(d=>{$('mqttEn').value=d.enabled?1:0;$('mqttBroker').value=d.broker||'';$('mqttUser').value=d.user||'';$('mqttPass').value='';$('mqttPrefix').value=d.prefix||'water-purifier'})}"
"function saveMQTT(){api('/api/mqtt/config',{enabled:$('mqttEn').value==1,broker:$('mqttBroker').value,user:$('mqttUser').value,password:$('mqttPass').value,prefix:$('mqttPrefix').value}).then(d=>alert(d.status||'已保存'))}"
"function loadWebAuth(){fetch('/api/config').then(r=>r.json()).then(d=>{$('webAuthEn').value=d.webAuthEnabled?1:0;$('webUser').value=d.webUsername||'admin';$('webPass').value='';})}"
"function saveWebAuth(){const u=$('webUser').value.trim();const p=$('webPass').value;if($('webAuthEn').value==1){if(!u)return alert('启用认证时必须设置用户名');if(u.length<3)return alert('用户名至少3个字符');if(!p)return alert('启用认证时必须设置密码');if(p.length<4)return alert('密码至少4个字符');}api('/api/config',{webAuthEnabled:$('webAuthEn').value==1,webUsername:u,webPassword:p}).then(d=>{alert(d.status||'已保存');if($('webAuthEn').value==1)setTimeout(()=>location.reload(),500);})}"
"loadConfig();loadFilters();loadMQTT();loadWebAuth();"
"</script></body></html>";

// 固件升级页面 - 独立页面
static const char html_ota_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>固件升级</title>"
"<style>" SHARED_CSS "</style></head><body>"
"<div class='container'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'>"
"<button class='back-btn m0' onclick=\"location.href='/admin'\">← 返回管理</button>"
"<button class='back-btn m0' onclick=\"location.href='/'\">首页 →</button>"
"</div>"
"<h1>固件升级 (OTA)</h1>"

"<div class='card'><h3>版本信息</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>当前版本</div><div id='fwVer' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>编译时间</div><div id='compileTime' class='stat-value fs14'>-</div></div>"
"<div class='stat'><div class='stat-label'>运行分区</div><div id='partition' class='stat-value fs14'>-</div></div>"
"</div>"
"</div>"

"<div class='card'><h3>固件状态</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>固件状态</div><div id='rollbackStatus' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>升级状态</div><div id='otaState' class='stat-value'>就绪</div></div>"
"<div class='stat'><div class='stat-label'>可升级</div><div id='canOta' class='stat-value'>-</div></div>"
"</div>"
"</div>"

"<div class='card'><h3>上传固件</h3>"
"<div class='form-row'>"
"<label>选择固件文件 (.bin)</label>"
"<input type='file' id='fwFile' accept='.bin' style='padding:8px' onchange='previewFirmware()'>"
"</div>"
"<div id='fwPreview' style='display:none;margin:10px 0;padding:10px;background:#f0f7ff;border-radius:8px;border:1px solid #667eea'>"
"<div style='font-size:13px;color:#555;margin-bottom:5px'>固件信息:</div>"
"<div class='grid' style='grid-template-columns:repeat(3,1fr)'>"
"<div><span style='color:#888'>版本:</span> <strong id='fwPreviewVer' style='color:#333'>-</strong></div>"
"<div><span style='color:#888'>日期:</span> <strong id='fwPreviewDate' style='color:#333'>-</strong></div>"
"<div><span style='color:#888'>时间:</span> <strong id='fwPreviewTime' style='color:#333'>-</strong></div>"
"</div>"
"</div>"
"<div id='otaProgress' style='display:none;margin:10px 0'>"
"<div style='display:flex;justify-content:space-between;font-size:13px;margin-bottom:4px'>"
"<span id='otaProgressText'>上传中...</span>"
"<span id='otaProgressPct'>0%</span>"
"</div>"
"<div style='height:8px;background:#e0e0e0;border-radius:4px;overflow:hidden'>"
"<div id='otaProgressBar' style='height:100%;width:0%;background:linear-gradient(90deg,#667eea,#764ba2);transition:width 0.3s'></div>"
"</div>"
"</div>"
"<div class='btn-group'><button class='btn btn-success' onclick='uploadFirmware()'>开始升级</button></div>"
"<p style='font-size:11px;color:#999;margin-top:8px'>升级过程中设备将自动重启，请勿断电</p>"
"</div>"

"<div class='card'><h3>恢复出厂固件</h3>"
"<p style='font-size:13px;color:#666;margin-bottom:10px'>恢复到出厂固件（factory分区），设备将自动重启。</p>"
"<div class='btn-group'><button class='btn btn-primary' onclick='revertFactory()'>恢复出厂固件</button></div>"
"</div>"

"<div class='card'><h3>回滚到上一OTA固件</h3>"
"<p style='font-size:13px;color:#666;margin-bottom:10px'>切换到另一个OTA分区（ota_0↔ota_1），设备将自动重启。仅当目标分区有有效固件时可用。</p>"
"<div class='btn-group'><button class='btn btn-danger' onclick='rollbackOTA()'>回滚到上一版本</button></div>"
"</div>"

"</div>"
"<script>"
"function $(id){return document.getElementById(id)}"
"function formatSize(b){if(!b||b<=0)return'-';return b<1024?b+'B':b<1048576?(b/1024).toFixed(1)+'KB':(b/1048576).toFixed(2)+'MB';}"
"function previewFirmware(){var f=$('fwFile').files[0];if(!f||!f.name.endsWith('.bin')){$('fwPreview').style.display='none';return}var r=new FileReader();r.onload=function(e){var d=new Uint8Array(e.target.result);fetch('/api/ota/preview',{method:'POST',body:d}).then(r=>r.json()).then(j=>{if(j.version){$('fwPreview').style.display='block';$('fwPreviewVer').textContent=j.version;$('fwPreviewDate').textContent=j.date;$('fwPreviewTime').textContent=j.time}else{$('fwPreview').style.display='none';alert(j.message||'固件解析失败')}}).catch(e=>{ $('fwPreview').style.display='none';alert('预览失败')})};r.readAsArrayBuffer(f.slice(0,512))}"
"function loadOTA(){fetch('/api/ota/status').then(r=>r.json()).then(d=>{$('fwVer').textContent=d.running_version||'-';$('compileTime').textContent=(d.compile_time||'')+' '+d.compile_date||'-';$('partition').textContent=d.partition||'-';$('rollbackStatus').textContent=d.rollback_status||'-';$('rollbackStatus').className=d.rollback_status=='出厂固件'||d.rollback_status=='有效'?'stat-value good':'stat-value';$('otaState').textContent=d.state==='idle'?'就绪':d.state;$('otaState').className=d.state==='idle'?'stat-value good':'stat-value';$('canOta').textContent=d.can_ota?'是':'否';$('canOta').className=d.can_ota?'stat-value good':'stat-value error'})}"
"function uploadFirmware(){var f=$('fwFile').files[0];if(!f)return alert('请选择固件文件');if(!f.name.endsWith('.bin'))return alert('只支持 .bin 文件');if(!confirm('确认升级固件？设备将自动重启。'))return;var fd=new FormData();fd.append('firmware',f);$('otaProgress').style.display='block';$('otaState').textContent='上传中...';$('otaState').className='stat-value warn';var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/update');xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);$('otaProgressText').textContent='上传中 '+formatSize(e.loaded)+'/'+formatSize(e.total);$('otaProgressPct').textContent=p+'%';$('otaProgressBar').style.width=p+'%';}};xhr.onload=function(){if(xhr.status===200){$('otaState').textContent='升级成功，重启中...';$('otaState').className='stat-value good';$('otaProgressText').textContent='升级完成，设备正在重启';$('otaProgressBar').style.width='100%';}else{try{var e=JSON.parse(xhr.responseText);alert(e.message||'升级失败');}catch(e){alert('升级失败: '+xhr.responseText);}$('otaState').textContent='升级失败';$('otaState').className='stat-value error';}};xhr.onerror=function(){$('otaState').textContent='网络错误';$('otaState').className='stat-value error';};xhr.send(fd);}"
"function revertFactory(){if(!confirm('确认恢复出厂固件？设备将自动重启。'))return;fetch('/api/ota/factory',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.status==='success'){alert('恢复成功，设备将重启');}else{alert(d.message||'恢复失败')}}).catch(e=>alert('请求失败'))}"
"function rollbackOTA(){if(!confirm('确认回滚到上一OTA固件？设备将自动重启。'))return;fetch('/api/ota/rollback',{method:'POST'}).then(r=>r.json()).then(d=>{if(d.status==='success'){alert('回滚成功，设备将重启');}else{alert(d.message||'回滚失败')}}).catch(e=>alert('请求失败'))}"
"loadOTA();"
"</script></body></html>";

// ==================== 日志查看器 ====================

// 日志查看器 - 增强版（带时间戳、日志级别过滤、自动刷新控制）
static const char html_log_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>系统日志</title>"
"<style>" SHARED_CSS LOG_CSS "</style></head><body>"
"<div class='container' style='max-width:900px'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'>"
"<button class='back-btn m0' onclick=\"location.href='/admin'\">← 返回管理</button>"
"<button class='back-btn m0' onclick=\"location.href='/'\">首页 →</button>"
"</div>"
"<h1>系统日志</h1>"

"<div class='card' style='padding:15px;margin-bottom:15px'>"
"<div class='control-panel'>"
"<label style='font-size:14px;color:#333'>日志级别:</label>"
"<select id='levelFilter' class='sel-auto' style='min-width:80px'>"
"<option value='all'>全部</option>"
"<option value='E'>仅错误 (E)</option>"
"<option value='W'>仅警告 (W)</option>"
"<option value='I'>仅信息 (I)</option>"
"<option value='D'>仅调试 (D)</option>"
"<option value='EW'>错误+警告</option>"
"<option value='EWI'>错误+警告+信息</option>"
"</select>"
"<label class='toggle-label'><input type='checkbox' id='autoRefresh' checked> 自动刷新</label>"
"<label style='font-size:14px;color:#333'>间隔:</label>"
"<select id='refreshInterval' class='sel-auto' style='min-width:60px'>"
"<option value='3'>3秒</option>"
"<option value='5'>5秒</option>"
"<option value='10' selected>10秒</option>"
"<option value='30'>30秒</option>"
"</select>"
"<button id='refreshBtn' style='padding:8px 16px;border-radius:8px;background:#667eea;color:#fff;border:none;cursor:pointer;font-size:14px'>刷新</button>"
"<button id='scrollTopBtn' style='padding:8px 12px;border-radius:8px;background:#28a745;color:#fff;border:none;cursor:pointer;font-size:14px'>↑顶部</button>"
"<button id='scrollBottomBtn' style='padding:8px 12px;border-radius:8px;background:#17a2b8;color:#fff;border:none;cursor:pointer;font-size:14px'>↓底部</button>"
"</div>"
"<div id='logContent'>加载中...</div>"
"</div>"

"</div>"
"<script>"
"let refreshTimer=null;"
"let currentLevel='all';"
"let refreshInterval=10;"
"function fetchLogs(){"
"const url='/api/logs/debug?level='+currentLevel;"
"fetch(url,{credentials:'include'}).then(r=>{"
"if(r.status===401){"
"document.getElementById('logContent').innerHTML='<span style=\"color:#f44747\">认证已过期，请<a href=\"/logs\" style=\"color:#667eea\">重新登录</a></span>';"
"if(refreshTimer)clearInterval(refreshTimer);"
"return;"
"}"
"return r.text();"
"}).then(html=>{"
"if(html)document.getElementById('logContent').innerHTML=html;"
"}).catch(e=>{"
"document.getElementById('logContent').innerHTML='<span style=\"color:#f44747\">获取日志失败: '+e+'</span>';"
"});}"
"function startRefresh(){"
"if(refreshTimer)clearInterval(refreshTimer);"
"if(document.getElementById('autoRefresh').checked){"
"refreshTimer=setInterval(fetchLogs,refreshInterval*1000);"
"}"
"}"
"document.getElementById('levelFilter').addEventListener('change',function(){"
"currentLevel=this.value;"
"fetchLogs();"
"});"
"document.getElementById('autoRefresh').addEventListener('change',startRefresh);"
"document.getElementById('refreshInterval').addEventListener('change',function(){"
"refreshInterval=parseInt(this.value);"
"startRefresh();"
"});"
"document.getElementById('refreshBtn').addEventListener('click',fetchLogs);"
"document.getElementById('scrollTopBtn').addEventListener('click',function(){var el=document.getElementById('logContent');if(el)el.scrollTop=0});"
"document.getElementById('scrollBottomBtn').addEventListener('click',function(){var el=document.getElementById('logContent');if(el)el.scrollTop=el.scrollHeight});"
"fetchLogs();"
"startRefresh();"
"</script></body></html>";

// ==================== Basic Auth函数 ====================

static void generate_session(void)
{
    uint8_t rand_bytes[16];
    esp_fill_random(rand_bytes, sizeof(rand_bytes));

    taskENTER_CRITICAL(&session_spinlock);
    for (int i = 0; i < 16; i++) {
        sprintf(&session_ctx.token[i*2], "%02x", rand_bytes[i]);
    }
    session_ctx.token[32] = '\0';
    session_ctx.create_time_us = esp_timer_get_time();
    session_ctx.valid = true;
    taskEXIT_CRITICAL(&session_spinlock);
}

static bool validate_session(const char *token)
{
    bool result = false;
    taskENTER_CRITICAL(&session_spinlock);
    if (session_ctx.valid && strcmp(token, session_ctx.token) == 0) {
        uint64_t age = (esp_timer_get_time() - session_ctx.create_time_us) / 1000000;
        if (age <= SESSION_TIMEOUT_SEC) {
            // 刷新session时间（滑动过期），活跃用户不会过期
            session_ctx.create_time_us = esp_timer_get_time();
            result = true;
        } else {
            session_ctx.valid = false;
        }
    }
    taskEXIT_CRITICAL(&session_spinlock);
    return result;
}

static bool check_auth(httpd_req_t *req)
{
    system_config_t cfg;
    config_manager_get_config(&cfg);

    // 未启用认证，直接通过
    if (!cfg.web_auth_enabled) return true;

    // 优先检查Session Cookie
    char cookie[128];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
        char *s = strstr(cookie, "session=");
        if (s) {
            s += 8;
            char tok[33] = {0};
            for (int i = 0; s[i] && s[i] != ';' && i < 32; i++) tok[i] = s[i];
            if (validate_session(tok)) return true;
        }
    }

    // 检查Basic Auth
    char auth[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof(auth)) != ESP_OK) return false;
    if (strncmp(auth, "Basic ", 6) != 0) return false;

    // Base64解码（增加缓冲区大小，防止溢出）
    // auth最大128字节，去掉"Basic "后最多122字节base64
    // Base64解码输出 = 输入 * 3/4，最多92字节
    unsigned char decoded[96] = {0};  // 增大到96字节（原64字节可能不足）
    size_t dec_len = 0;
    int ret = mbedtls_base64_decode(decoded, sizeof(decoded), &dec_len,
                          (unsigned char*)(auth + 6), strlen(auth + 6));
    if (ret != 0 || dec_len == 0) {
        ESP_LOGD(TAG, "Base64解码失败");
        return false;
    }

    // 格式: username:password
    char *colon = strchr((char*)decoded, ':');
    if (!colon) return false;

    int user_len = colon - (char*)decoded;
    char user[32] = {0}, pass[32] = {0};
    if (user_len > 0 && user_len < 32) {
        memcpy(user, decoded, user_len);
        strlcpy(pass, colon + 1, sizeof(pass));  // 安全拷贝，防止密码超长溢出
    }

    // 比较凭证
    if (strcmp(user, cfg.web_username) == 0 && strcmp(pass, cfg.web_password) == 0) {
        generate_session();
        return true;
    }

    return false;
}

static void send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Water Purifier\"");
    httpd_resp_send(req, "Unauthorized", 11);
}

static void set_session_cookie(httpd_req_t *req)
{
    char token_copy[33];
    bool valid = false;

    taskENTER_CRITICAL(&session_spinlock);
    if (session_ctx.valid) {
        memcpy(token_copy, session_ctx.token, sizeof(token_copy));
        valid = true;
    }
    taskEXIT_CRITICAL(&session_spinlock);

    if (valid) {
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "session=%s; Path=/; Max-Age=%d",
                 token_copy, SESSION_TIMEOUT_SEC);
        httpd_resp_set_hdr(req, "Set-Cookie", hdr);
    }
}

// ==================== 页面和API处理函数 ====================

static esp_err_t handle_log_page(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html_log_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_log_api(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);

    // 使用cJSON构建JSON数组
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    cJSON *lines_arr = cJSON_CreateArray();
    uint32_t read_pos = s_log_tail;
    uint32_t tail = s_log_head;

    while (read_pos != tail) {
        char line[300];
        int ln = 0;
        int slen = 0;
        while (read_pos != tail && slen < 290) {
            char c = s_log_buf[read_pos];
            if (c == '\n' || c == '\r') break;
            line[ln++] = c;
            read_pos = (read_pos + 1) % LOG_BUF_SIZE;
            slen++;
        }
        if (slen == 0) {
            read_pos = (read_pos + 1) % LOG_BUF_SIZE;
            continue;
        }
        line[ln] = '\0';
        cJSON_AddItemToArray(lines_arr, cJSON_CreateString(line));

        // 跳过换行
        while (read_pos != tail && (s_log_buf[read_pos] == '\n' || s_log_buf[read_pos] == '\r')) {
            read_pos = (read_pos + 1) % LOG_BUF_SIZE;
        }
    }

    cJSON_AddItemToObject(root, "lines", lines_arr);
    cJSON_AddNumberToObject(root, "nextPos", read_pos);

    char *resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, resp, strlen(resp));
    free(resp);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "日志API发送失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief 日志端点：流式输出日志缓冲区内容（chunked transfer encoding）
 * @note 使用 httpd_resp_send_chunk 分块发送，避免 18KB 大块内存分配
 *       内存使用：~1KB chunk缓冲区 + ~800字节行位置数组（vs 旧方案 ~19KB）
 *       最多显示 MAX_LOG_LINES 行，从最旧到最新输出
 */
static esp_err_t handle_log_debug(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);

    // 解析查询参数中的日志级别过滤
    char query[64] = {0};
    char level_filter[8] = "all";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char level_val[16] = {0};
        if (httpd_query_key_value(query, "level", level_val, sizeof(level_val)) == ESP_OK) {
            strncpy(level_filter, level_val, sizeof(level_filter) - 1);
        }
    }

    // 获取启动墙钟时间（用于时间戳转换）
    time_t boot_wall = wifi_manager_get_boot_wall_clock_time();
    bool has_wall_time = (boot_wall > 0);

    // 分配小块chunk缓冲区（仅1KB，vs 旧方案 18KB）
    #define CHUNK_BUF_SIZE 1024
    char *chunk = malloc(CHUNK_BUF_SIZE);
    if (!chunk) {
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, "<span style='color:#d32f2f'>内存分配失败</span>", HTTPD_RESP_USE_STRLEN);
    }
    int cpos = 0;

    // ==================== 第一步：发送HTML头部（CSS + 信息栏）====================
    uint32_t tail = s_log_tail;
    uint32_t head = s_log_head;
    unsigned long used_bytes = (head >= tail) ? (head - tail) : (LOG_BUF_SIZE - tail + head);

    httpd_resp_set_type(req, "text/html");
    char header[384];
    int hlen = snprintf(header, sizeof(header),
        "<style>.E{color:#d32f2f;font-weight:bold}.W{color:#f57c00}.I{color:#388e3c}.D{color:#1976d2}</style>"
        "<div style='padding:4px 8px;border-bottom:1px solid #e0e0e0;font-size:10px;color:#888;background:#f0f0f0'>"
        "缓冲区 %lu/%lu 字节 | %s | 过滤: %s</div>",
        used_bytes, (unsigned long)LOG_BUF_SIZE,
        has_wall_time ? "时间戳已同步" : "时间戳未同步",
        level_filter);
    // snprintf截断安全：即使hlen >= sizeof(header)，snprintf已保证null终止
    // 但使用返回值作为实际长度，避免发送被截断的不完整HTML
    if (hlen >= (int)sizeof(header)) {
        hlen = sizeof(header) - 1;
    }
    httpd_resp_send_chunk(req, header, hlen);

    // ==================== 第二步：逆向扫描定位最近200行的起始位置 ====================
    #define MAX_LOG_LINES 200
    uint32_t line_starts[MAX_LOG_LINES];
    int line_count = 0;
    {
        uint32_t scan_pos = head;
        int scan_line_len = 0;
        while (scan_pos != tail && line_count < MAX_LOG_LINES) {
            scan_pos = (scan_pos == 0) ? (LOG_BUF_SIZE - 1) : (scan_pos - 1);
            char c = s_log_buf[scan_pos];
            if (c == '\n' || c == '\r') {
                if (scan_line_len > 0) {
                    uint32_t ls = scan_pos + 1;
                    if (ls >= LOG_BUF_SIZE) ls = 0;
                    line_starts[line_count++] = ls;
                    scan_line_len = 0;
                }
            } else {
                if (scan_line_len < 299) scan_line_len++;
            }
        }
    }

    // ==================== 第三步：正向逐行输出，流式发送 ====================
    char line_buf[300];

    for (int i = line_count - 1; i >= 0; i--) {
        uint32_t read_pos = line_starts[i];
        int line_len = 0;

        // 读取一行原始内容
        while (read_pos != head && line_len < 299) {
            char c = s_log_buf[read_pos];
            if (c == '\n' || c == '\r') break;
            line_buf[line_len++] = c;
            read_pos = (read_pos + 1) % LOG_BUF_SIZE;
        }
        line_buf[line_len] = '\0';

        // 解析日志级别和启动时间戳
        char lvl = 0;
        uint32_t boot_ms = 0;
        if (line_len >= 5 &&
            (line_buf[0] == 'E' || line_buf[0] == 'W' || line_buf[0] == 'I' || line_buf[0] == 'D')) {
            lvl = line_buf[0];
            if (line_buf[1] == ' ' && line_buf[2] == '(') {
                int ts_end = 3;
                while (ts_end < line_len && line_buf[ts_end] != ')') ts_end++;
                if (ts_end < line_len && line_buf[ts_end] == ')') {
                    char ts_str[16] = {0};
                    int ts_len = ts_end - 3;
                    if (ts_len > 0 && ts_len < 16) {
                        memcpy(ts_str, line_buf + 3, ts_len);
                        boot_ms = (uint32_t)atoi(ts_str);
                    }
                }
            }
        }

        // 级别过滤
        bool show_log = (strcmp(level_filter, "all") == 0);
        if (!show_log && lvl) {
            for (int j = 0; level_filter[j]; j++) {
                if (level_filter[j] == lvl) { show_log = true; break; }
            }
        }
        if (!show_log) continue;

        // 格式化行HTML到临时缓冲区
        char html[420];
        int hpos = 0;

        // <span class='X'>
        if (lvl) {
            html[hpos++] = '<'; html[hpos++] = 's'; html[hpos++] = 'p'; html[hpos++] = 'a';
            html[hpos++] = 'n'; html[hpos++] = ' '; html[hpos++] = 'c'; html[hpos++] = 'l';
            html[hpos++] = 'a'; html[hpos++] = 's'; html[hpos++] = 's'; html[hpos++] = '=';
            html[hpos++] = '\''; html[hpos++] = lvl; html[hpos++] = '\''; html[hpos++] = '>';
        }

        // 时间戳 [MM-DD HH:MM:SS]
        if (has_wall_time && boot_ms > 0 && lvl) {
            time_t log_time = boot_wall + (boot_ms / 1000);
            struct tm tm_log;
            localtime_r(&log_time, &tm_log);
            hpos += snprintf(html + hpos, sizeof(html) - hpos,
                "[%02d-%02d %02d:%02d:%02d] ",
                tm_log.tm_mon + 1, tm_log.tm_mday,
                tm_log.tm_hour, tm_log.tm_min, tm_log.tm_sec);
        }

        // 日志级别前缀 + 内容起始位置
        int content_start = 0;
        if (lvl && line_len >= 5) {
            int paren_end = 2;
            while (paren_end < line_len && line_buf[paren_end] != ')') paren_end++;
            if (paren_end < line_len && paren_end + 3 < line_len) {
                html[hpos++] = lvl;
                html[hpos++] = ' ';
                content_start = paren_end + 1;
                if (content_start < line_len && line_buf[content_start] == ' ') content_start++;
            }
        }

        // HTML转义输出日志内容
        for (int k = content_start; k < line_len && hpos < (int)sizeof(html) - 10; k++) {
            char ch = line_buf[k];
            if (ch == '<') { memcpy(html + hpos, "&lt;", 4); hpos += 4; }
            else if (ch == '>') { memcpy(html + hpos, "&gt;", 4); hpos += 4; }
            else if (ch == '&') { memcpy(html + hpos, "&amp;", 5); hpos += 5; }
            else html[hpos++] = ch;
        }

        // </span>\n
        if (lvl) {
            memcpy(html + hpos, "</span>\n", 8);
            hpos += 8;
        } else {
            html[hpos++] = '\n';
        }

        // 流式发送逻辑：
        // - 如果当前chunk装不下此行 → 先flush已有内容
        // - 如果单行超过chunk容量 → 直接作为独立chunk发送
        // - 否则 → 追加到chunk缓冲区
        if (cpos + hpos > CHUNK_BUF_SIZE) {
            if (cpos > 0) {
                httpd_resp_send_chunk(req, chunk, cpos);
                cpos = 0;
            }
            if (hpos > CHUNK_BUF_SIZE) {
                // 单行超大，直接发送
                httpd_resp_send_chunk(req, html, hpos);
                continue;
            }
        }
        memcpy(chunk + cpos, html, hpos);
        cpos += hpos;
    }

    // 发送剩余内容
    if (cpos > 0) {
        httpd_resp_send_chunk(req, chunk, cpos);
    }

    // 滚动到底部（显示最新日志）
    httpd_resp_sendstr_chunk(req,
        "<script>(function(){var el=document.getElementById('logContent');"
        "if(el)el.scrollTop=el.scrollHeight})()</script>");

    // 结束chunked响应
    httpd_resp_send_chunk(req, NULL, 0);

    free(chunk);
    return ESP_OK;
}

// ==================== 辅助函数 ====================

/**
 * @brief 消费剩余 HTTP 请求体，防止连接被重置导致客户端网络错误
 */
static void drain_http_body(httpd_req_t *req)
{
    char drain_buf[256];
    while (httpd_req_recv(req, drain_buf, sizeof(drain_buf)) > 0) { }
}

/**
 * @brief 发送 JSON 错误响应（自动先消费请求体）
 */
static esp_err_t send_json_error(httpd_req_t *req, int status_code, const char *msg)
{
    drain_http_body(req);
    httpd_resp_set_status(req, status_code == 500 ? "500 Internal Server Error" : "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "message", msg);
    char *resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (resp) {
        httpd_resp_send(req, resp, strlen(resp));
        free(resp);
    } else {
        httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"internal error\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_FAIL;
}

// ==================== API处理函数 ====================

static esp_err_t handle_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

static esp_err_t handle_admin(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_admin_page, strlen(html_admin_page));
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_ota_page, strlen(html_ota_page));
    return ESP_OK;
}

// favicon处理器 - 返回空内容避免404警告
static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    fsm_state_t state = fsm_get_state();
    fsm_runtime_data_t data = {0};
    if (fsm_get_runtime_data(&data) != ESP_OK) {
        ESP_LOGW(TAG, "获取运行数据超时，使用默认值");
    }

    tds_dual_measurement_t tds;
    tds_sensor_get_latest_dual(&tds);

    filters_status_t filters_status;
    filter_mgr_get_filters_status(&filters_status);

    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));

    char ssid_buf[33] = "";
    wifi_manager_get_ssid(ssid_buf, sizeof(ssid_buf));

    uint32_t today_prod_min = 0;
    daily_stats_t today;
    if (history_get_today_stats(&today) == ESP_OK) {
        today_prod_min = today.production_sec / 60;
    }

    char sntp_time[64] = "未同步";
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {
        snprintf(sntp_time, sizeof(sntp_time), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }

    uint32_t prod_water_l = filter_mgr_get_total_production_water();

    // 使用cJSON构建JSON响应
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(root, "state", fsm_get_state_name(state));
    cJSON_AddNumberToObject(root, "cycles", data.total_production_cycles);
    cJSON_AddNumberToObject(root, "flushes", data.total_flush_cycles);
    cJSON_AddNumberToObject(root, "prodTime", data.total_production_time_sec);
    cJSON_AddNumberToObject(root, "uptime", get_uptime_sec());
    cJSON_AddNumberToObject(root, "todayProd", today_prod_min);
    cJSON_AddNumberToObject(root, "prodWater", prod_water_l);
    cJSON_AddNumberToObject(root, "tds_in", tds.inlet.valid ? tds.inlet.tds_value : 0.0);
    cJSON_AddNumberToObject(root, "tds_out", tds.outlet.valid ? tds.outlet.tds_value : 0.0);
    cJSON_AddNumberToObject(root, "rate", tds.both_valid ? tds.reduction_rate : 0.0);
    cJSON_AddBoolToObject(root, "leak", gpio_driver_read_water_leak());
    cJSON_AddStringToObject(root, "sntpTime", sntp_time);

    // 滤芯数组
    cJSON *filters_arr = cJSON_CreateArray();
    if (!filters_arr) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
        return ESP_FAIL;
    }
    for (int i = 0; i < FILTER_COUNT; i++) {
        cJSON *f = cJSON_CreateObject();
        if (!f) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
            return ESP_FAIL;
        }
        cJSON_AddNumberToObject(f, "waterPct", filters_status.filters[i].percentage);
        cJSON_AddNumberToObject(f, "timePct", filters_status.filters[i].time_percentage);
        cJSON_AddNumberToObject(f, "effPct", filters_status.filters[i].effective_percentage);
        cJSON_AddNumberToObject(f, "used", filters_status.filters[i].used_liters);
        cJSON_AddNumberToObject(f, "total", filters_status.filters[i].total_liters);
        cJSON_AddNumberToObject(f, "timeLimit", filters_status.filters[i].time_limit_hours);
        cJSON_AddBoolToObject(f, "needReplace", filters_status.filters[i].replacement_needed);
        cJSON_AddItemToArray(filters_arr, f);
    }
    cJSON_AddItemToObject(root, "filters", filters_arr);

    cJSON_AddNumberToObject(root, "totalWater", filters_status.total_water_used);
    cJSON_AddStringToObject(root, "wifiState", wifi_manager_get_state_name(wifi_manager_get_state()));
    cJSON_AddStringToObject(root, "ssid", ssid_buf);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddNumberToObject(root, "txPower", pm_manager_get_wifi_tx_power() / 4);
    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minHeap", esp_get_minimum_free_heap_size());

    TaskHandle_t fsm_task = xTaskGetHandle("fsm_task");
    TaskHandle_t httpd_task = xTaskGetHandle("httpd");
    TaskHandle_t tds_task = xTaskGetHandle("tds_task");
    TaskHandle_t monitor_task = xTaskGetHandle("monitor");
    cJSON_AddNumberToObject(root, "stackFSM", fsm_task ? uxTaskGetStackHighWaterMark2(fsm_task) : 0);
    cJSON_AddNumberToObject(root, "stackHTTP", httpd_task ? uxTaskGetStackHighWaterMark2(httpd_task) : 0);
    cJSON_AddNumberToObject(root, "stackTDS", tds_task ? uxTaskGetStackHighWaterMark2(tds_task) : 0);
    cJSON_AddNumberToObject(root, "stackMonitor", monitor_task ? uxTaskGetStackHighWaterMark2(monitor_task) : 0);

    char *resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!resp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON print failed");
        return ESP_FAIL;
    }

    httpd_resp_send(req, resp, strlen(resp));
    free(resp);
    return ESP_OK;
}

static esp_err_t handle_control(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    // 防抖检查：短时间内不允许重复控制操作
    uint64_t now = esp_timer_get_time();
    uint64_t elapsed_ms = (now - s_last_control_time) / 1000;
    if (elapsed_ms < CONTROL_DEBOUNCE_MS) {
        ESP_LOGW(TAG, "控制操作防抖，请等待 %llu 毫秒", CONTROL_DEBOUNCE_MS - elapsed_ms);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"操作过快，请稍候\"}");
        return ESP_OK;
    }
    s_last_control_time = now;

    /* 检查请求体长度，防止恶意超大请求 */
    int content_len = req->content_len;
    if (content_len > 127) {
        ESP_LOGW(TAG, "control请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        // 客户端断开连接，静默返回
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action) { cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action"); return ESP_FAIL; }

    const char *a = action->valuestring;
    if (strcmp(a, "start_production") == 0) fsm_manual_start_production();
    else if (strcmp(a, "normal_flush") == 0) fsm_manual_normal_flush();
    else if (strcmp(a, "pure_flush") == 0) fsm_manual_pure_flush();
    else if (strcmp(a, "filter_flush") == 0) fsm_manual_filter_flush();
    else if (strcmp(a, "reset") == 0) fsm_clear_stop();
    else if (strcmp(a, "standby") == 0) fsm_manual_go_standby();
    else if (strcmp(a, "shutdown") == 0) fsm_manual_shutdown();
    else if (strcmp(a, "reboot") == 0) {
        ESP_LOGI(TAG, "用户请求重启设备");
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"重启中...\"}");
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待HTTP响应完成
        esp_restart();
        return ESP_OK;  // 不会执行到这里
    }
    else {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"未知操作\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_config_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    httpd_resp_set_type(req, "application/json");

    system_config_t cfg;
    config_manager_get_config(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "normalFlushDur", cfg.normal_flush_duration_sec);
    cJSON_AddNumberToObject(root, "pureFlushDur", cfg.pure_flush_duration_sec);
    cJSON_AddNumberToObject(root, "filterFlushDur", cfg.filter_flush_duration_sec / 60);
    cJSON_AddNumberToObject(root, "prodTimeout", cfg.production_timeout_sec / 60);
    cJSON_AddNumberToObject(root, "leakConfirm", cfg.leak_confirm_time_sec);
    cJSON_AddNumberToObject(root, "saveInterval", cfg.runtime_save_interval_min);
    cJSON_AddNumberToObject(root, "relayLevel", cfg.relay_trigger_level);
    cJSON_AddNumberToObject(root, "tdsInTh", cfg.tds_inlet_threshold);
    cJSON_AddNumberToObject(root, "tdsOutTh", cfg.tds_outlet_threshold);
    cJSON_AddNumberToObject(root, "roMem", cfg.ro_membrane_type);
    cJSON_AddNumberToObject(root, "pumpType", cfg.pump_type);
    cJSON_AddNumberToObject(root, "tankSize", cfg.tank_size);
    cJSON_AddNumberToObject(root, "wasteFlow", cfg.waste_valve_flow_cc);
    cJSON_AddNumberToObject(root, "whValveOpen", cfg.water_hammer_valve_open_delay_ms);
    cJSON_AddNumberToObject(root, "whPumpStop", cfg.water_hammer_pump_stop_delay_ms);
    cJSON_AddNumberToObject(root, "whValveClose", cfg.water_hammer_valve_close_delay_ms);
    cJSON_AddBoolToObject(root, "webAuthEnabled", cfg.web_auth_enabled);
    cJSON_AddStringToObject(root, "webUsername", cfg.web_username);

    char *resp = cJSON_PrintUnformatted(root);
    if (!resp) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send(req, resp, strlen(resp));
    free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handle_config_set(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 512) {
        ESP_LOGW(TAG, "config_set请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[512];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    system_config_t cfg;
    config_manager_get_config(&cfg);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "normalFlushDur"))) cfg.normal_flush_duration_sec = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "pureFlushDur"))) cfg.pure_flush_duration_sec = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "filterFlushDur"))) cfg.filter_flush_duration_sec = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "prodTimeout"))) cfg.production_timeout_sec = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "leakConfirm"))) cfg.leak_confirm_time_sec = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "saveInterval"))) cfg.runtime_save_interval_min = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "relayLevel"))) cfg.relay_trigger_level = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "tdsInTh"))) cfg.tds_inlet_threshold = v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "tdsOutTh"))) cfg.tds_outlet_threshold = v->valuedouble;
    if ((v = cJSON_GetObjectItem(root, "whValveOpen"))) cfg.water_hammer_valve_open_delay_ms = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "whPumpStop"))) cfg.water_hammer_pump_stop_delay_ms = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "whValveClose"))) cfg.water_hammer_valve_close_delay_ms = v->valueint;

    // Web认证配置
    if ((v = cJSON_GetObjectItem(root, "webAuthEnabled"))) cfg.web_auth_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(root, "webUsername")) && v->valuestring && strlen(v->valuestring) > 0) {
        strncpy(cfg.web_username, v->valuestring, sizeof(cfg.web_username) - 1);
        cfg.web_username[sizeof(cfg.web_username) - 1] = '\0';
    }
    if ((v = cJSON_GetObjectItem(root, "webPassword")) && v->valuestring && strlen(v->valuestring) > 0) {
        strncpy(cfg.web_password, v->valuestring, sizeof(cfg.web_password) - 1);
        cfg.web_password[sizeof(cfg.web_password) - 1] = '\0';
    }

    // 验证配置值范围（防止恶意/错误请求设置非法值如 prodTimeout=0）
    if (!config_manager_validate(&cfg)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"配置值超出有效范围\"}");
        return ESP_OK;
    }

    config_manager_set_config(&cfg);

    // 先应用配置到各模块，成功后再保存NVS（确保一致性）
    fsm_set_normal_flush_duration(cfg.normal_flush_duration_sec);
    fsm_set_pure_flush_duration(cfg.pure_flush_duration_sec);
    fsm_set_filter_flush_duration(cfg.filter_flush_duration_sec);
    fsm_set_production_timeout(cfg.production_timeout_sec);
    fsm_set_leak_confirm_time(cfg.leak_confirm_time_sec);
    fsm_set_water_hammer_delays(cfg.water_hammer_valve_open_delay_ms,
                                 cfg.water_hammer_pump_stop_delay_ms,
                                 cfg.water_hammer_valve_close_delay_ms);
    fsm_set_runtime_save_interval(cfg.runtime_save_interval_min);
    gpio_driver_set_relay_trigger_level(cfg.relay_trigger_level);
    tds_sensor_set_alarm_threshold(TDS_SENSOR_INLET, cfg.tds_inlet_threshold);
    tds_sensor_set_alarm_threshold(TDS_SENSOR_OUTLET, cfg.tds_outlet_threshold);

    // 配置应用成功后，持久化到NVS
    config_manager_save();

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_hardware_config_set(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 127) {
        ESP_LOGW(TAG, "hardware_config请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    system_config_t cfg;
    config_manager_get_config(&cfg);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "roMem"))) cfg.ro_membrane_type = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "pumpType"))) cfg.pump_type = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "tankSize"))) cfg.tank_size = v->valueint;
    if ((v = cJSON_GetObjectItem(root, "wasteFlow"))) cfg.waste_valve_flow_cc = v->valueint;

    config_manager_set_config(&cfg);

    // 先应用硬件配置，成功后再保存NVS
    fsm_set_production_rate_by_membrane(cfg.ro_membrane_type);
    float waste_lph = cfg.waste_valve_flow_cc * 60.0f / 1000.0f;
    filter_mgr_set_waste_flow_lph(waste_lph);

    config_manager_save();

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"已保存\"}");
    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    // 防抖检查：短时间内不允许重复扫描
    uint64_t now = esp_timer_get_time();
    uint64_t elapsed_ms = (now - s_last_wifi_scan_time) / 1000;
    if (elapsed_ms < WIFI_SCAN_DEBOUNCE_MS) {
        ESP_LOGW(TAG, "WiFi扫描防抖，请等待 %llu 毫秒", WIFI_SCAN_DEBOUNCE_MS - elapsed_ms);
        httpd_resp_sendstr(req, "{\"networks\":[],\"error\":\"scan_too_fast\"}");
        return ESP_OK;
    }
    s_last_wifi_scan_time = now;

    // 停止之前的扫描（如果有）
    esp_wifi_scan_stop();

    // 开始扫描
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi扫描启动失败: %s", esp_err_to_name(ret));
        httpd_resp_sendstr(req, "{\"networks\":[],\"error\":\"scan_failed\"}");
        return ESP_OK;
    }

    // 获取扫描结果
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        httpd_resp_sendstr(req, "{\"networks\":[]}");
        return ESP_OK;
    }

    wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        ESP_LOGE(TAG, "WiFi扫描结果内存分配失败");
        httpd_resp_sendstr(req, "{\"networks\":[],\"error\":\"no_memory\"}");
        return ESP_OK;
    }

    esp_err_t scan_ret = esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    if (scan_ret != ESP_OK) {
        ESP_LOGW(TAG, "获取扫描结果失败: %s", esp_err_to_name(scan_ret));
        free(ap_list);
        httpd_resp_sendstr(req, "{\"networks\":[],\"error\":\"scan_failed\"}");
        return ESP_OK;
    }

    // 构建JSON响应
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count && i < 20; i++) {  // 最多返回20个网络
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", (char*)ap_list[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(net, "channel", ap_list[i].primary);
        cJSON_AddNumberToObject(net, "auth", ap_list[i].authmode);
        cJSON_AddItemToArray(networks, net);
    }

    cJSON_AddItemToObject(root, "networks", networks);
    cJSON_AddNumberToObject(root, "count", ap_count);

    char *resp = cJSON_PrintUnformatted(root);
    if (!resp) {
        cJSON_Delete(root);
        free(ap_list);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, resp);

    free(resp);
    cJSON_Delete(root);
    free(ap_list);

    return ESP_OK;
}

static esp_err_t handle_wifi_config(httpd_req_t *req)
{
    // WiFi配置安全策略：已连接WiFi时要求认证（防止局域网内设备劫持）
    // AP配网模式下免认证（初始配置场景）
    if (wifi_manager_is_connected()) {
        if (!check_auth(req)) {
            send_401(req);
            return ESP_OK;
        }
        set_session_cookie(req);
    }

    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 127) {
        ESP_LOGW(TAG, "wifi_config请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    cJSON *pass = cJSON_GetObjectItem(root, "password");

    if (ssid && ssid->valuestring) {
        wifi_manager_set_config(ssid->valuestring, pass ? pass->valuestring : "");
        wifi_manager_save_config();
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t handle_filter_reset(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 63) {
        ESP_LOGW(TAG, "filter_reset请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *filter_idx = cJSON_GetObjectItem(root, "filter");
    if (!filter_idx) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filter index");
        return ESP_FAIL;
    }

    int idx = filter_idx->valueint;
    if (idx < 0 || idx >= FILTER_COUNT) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filter index");
        return ESP_FAIL;
    }
    esp_err_t ret = filter_mgr_reset_filter((filter_type_t)idx);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");

    if (ret == ESP_OK) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "filter_name", filter_mgr_get_filter_name(idx));
        char *json_str = cJSON_PrintUnformatted(resp);
        if (!json_str) {
            cJSON_Delete(resp);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_sendstr(req, json_str);
        free(json_str);
        cJSON_Delete(resp);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\"}");
    }
    return ESP_OK;
}

static esp_err_t handle_filter_capacity(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 255) {
        ESP_LOGW(TAG, "filter_capacity请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        return ESP_FAIL;
    }
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *caps = cJSON_GetObjectItem(root, "caps");
    if (!caps || !cJSON_IsArray(caps)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing caps array");
        return ESP_FAIL;
    }

    int array_size = cJSON_GetArraySize(caps);
    if (array_size != FILTER_COUNT) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid caps array size");
        return ESP_FAIL;
    }

    // 收集所有容量，一次性写入NVs
    uint32_t capacities[FILTER_COUNT];
    for (int i = 0; i < FILTER_COUNT; i++) {
        cJSON *cap = cJSON_GetArrayItem(caps, i);
        // 验证数组元素必须是有效数字
        capacities[i] = (cap && cJSON_IsNumber(cap)) ? (uint32_t)cap->valueint : 0;
    }
    filter_mgr_set_all_filter_capacity(capacities);

    // 解析可选的时间寿命
    cJSON *times = cJSON_GetObjectItem(root, "times");
    if (times && cJSON_IsArray(times) && cJSON_GetArraySize(times) == FILTER_COUNT) {
        uint32_t th[FILTER_COUNT];
        for (int i = 0; i < FILTER_COUNT; i++) {
            cJSON *item = cJSON_GetArrayItem(times, i);
            th[i] = item && cJSON_IsNumber(item) ? (uint32_t)item->valueint : 0;
        }
        filter_mgr_set_all_filter_times(th);
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t tds_calibrate_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 127) {
        ESP_LOGW(TAG, "tds_calibrate请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { drain_http_body(req); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *sensor = cJSON_GetObjectItem(root, "sensor");
    cJSON *value = cJSON_GetObjectItem(root, "value");

    if (!sensor || !value || !cJSON_IsNumber(sensor) || !cJSON_IsNumber(value)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"参数错误\"}");
        return ESP_OK;
    }

    int sensor_id = sensor->valueint;
    float cal_value = (float)value->valuedouble;

    // sensor_id边界验证：必须是0(进水)或1(出水)
    if (sensor_id < 0 || sensor_id > 1) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"传感器编号无效(0=进水,1=出水)\"}");
        return ESP_OK;
    }

    // 校准前检查：传感器必须有效连接才能校准
    tds_measurement_t meas;
    if (tds_sensor_measure((tds_sensor_id_t)sensor_id, &meas) != ESP_OK || !meas.valid) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"传感器未连接，无法校准\"}");
        return ESP_OK;
    }

    // 校准值必须合理（TDS范围0-2000 ppm）
    if (cal_value <= 0 || cal_value > 2000) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"校准值范围无效（0-2000 ppm）\"}");
        return ESP_OK;
    }

    esp_err_t err = tds_sensor_calibrate((tds_sensor_id_t)sensor_id, cal_value);

    // 校准成功后同步到config_manager持久化
    if (err == ESP_OK) {
        tds_calibration_t cal;
        if (tds_sensor_get_calibration((tds_sensor_id_t)sensor_id, &cal) == ESP_OK) {
            // 校准数据有效性检查：scale必须在合理范围内(0.01~100)
            // 防止测量值接近0导致scale异常大，或异常测量导致scale为负数
            if (cal.scale > 0.01f && cal.scale < 100.0f) {
                system_config_t cfg;
                config_manager_get_config(&cfg);
                cfg.tds_calibration_offset[sensor_id] = cal.offset;
                cfg.tds_calibration_scale[sensor_id] = cal.scale;
                config_manager_set_config(&cfg);
                config_manager_save();  // 立即写入Flash
            } else {
                ESP_LOGW(TAG, "校准scale异常: %.4f，不保存（有效范围: 0.01~100）", cal.scale);
                err = ESP_ERR_INVALID_STATE;
            }
        } else {
            ESP_LOGW(TAG, "获取校准参数失败，校准数据未持久化");
            err = ESP_FAIL;
        }
    }

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");

    if (err == ESP_OK) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"status\":\"校准成功，标准值: %.1f ppm\"}", cal_value);
        httpd_resp_sendstr(req, resp);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"校准失败\"}");
    }
    return ESP_OK;
}

// ==================== MQTT配置API ====================

static esp_err_t handle_mqtt_config_get(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    system_config_t cfg;
    config_manager_get_config(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", cfg.mqtt_enabled);
    cJSON_AddStringToObject(root, "broker", cfg.mqtt_broker);
    cJSON_AddStringToObject(root, "user", cfg.mqtt_username);
    cJSON_AddStringToObject(root, "prefix", cfg.mqtt_topic_prefix);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_mqtt_config_set(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 255) {
        ESP_LOGW(TAG, "mqtt_config_set请求体过大: %d字节", content_len);
        drain_http_body(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        return ESP_FAIL;
    }

    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) return ESP_FAIL;
    buf[len] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_FAIL;

    system_config_t cfg;
    config_manager_get_config(&cfg);

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "enabled"))) {
        cfg.mqtt_enabled = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "broker")) && cJSON_IsString(item)) {
        strncpy(cfg.mqtt_broker, item->valuestring, sizeof(cfg.mqtt_broker) - 1);
        cfg.mqtt_broker[sizeof(cfg.mqtt_broker) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(root, "user")) && cJSON_IsString(item)) {
        strncpy(cfg.mqtt_username, item->valuestring, sizeof(cfg.mqtt_username) - 1);
        cfg.mqtt_username[sizeof(cfg.mqtt_username) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(root, "password")) && cJSON_IsString(item)) {
        strncpy(cfg.mqtt_password, item->valuestring, sizeof(cfg.mqtt_password) - 1);
        cfg.mqtt_password[sizeof(cfg.mqtt_password) - 1] = '\0';
    }
    if ((item = cJSON_GetObjectItem(root, "prefix")) && cJSON_IsString(item)) {
        strncpy(cfg.mqtt_topic_prefix, item->valuestring, sizeof(cfg.mqtt_topic_prefix) - 1);
        cfg.mqtt_topic_prefix[sizeof(cfg.mqtt_topic_prefix) - 1] = '\0';
    }

    cJSON_Delete(root);
    config_manager_set_config(&cfg);
    config_manager_save();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"已保存\"}");
    return ESP_OK;
}

// ==================== OTA升级 ====================

// OTA会话锁：防止并发OTA请求（静态缓冲区不支持并发）
static volatile bool s_ota_session_active = false;
static portMUX_TYPE s_ota_spinlock = portMUX_INITIALIZER_UNLOCKED;

// OTA接收超时配置
#define OTA_RECV_TIMEOUT_MS 30000  // 30秒总接收超时
#define OTA_INACTIVITY_TIMEOUT_MS 5000  // 5秒无数据接收超时
#define OTA_BUF_SIZE 2048  // OTA接收缓冲区大小

/**
 * @brief OTA 固件上传处理器
 * @note 接收 multipart/form-data 上传的 .bin 固件，流式写入 OTA 分区
 * @note 使用静态缓冲区避免 httpd 任务栈溢出（会话锁保护）
 */
static esp_err_t ota_update_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    esp_err_t result = ESP_FAIL;  // 默认返回失败，成功时修改

    // 检查是否已有OTA会话进行中
    taskENTER_CRITICAL(&s_ota_spinlock);
    if (s_ota_session_active) {
        taskEXIT_CRITICAL(&s_ota_spinlock);
        ESP_LOGW(TAG, "OTA会话已存在，拒绝并发请求");
        return send_json_error(req, 400, "已有OTA上传进行中，请等待完成");
    }
    s_ota_session_active = true;
    taskEXIT_CRITICAL(&s_ota_spinlock);

    // 动态分配缓冲区（OTA不频繁使用，节省静态内存）
    char *buf = malloc(OTA_BUF_SIZE);  // 2048字节足够处理multipart boundary
    char *close_bnd = malloc(160);  // 足够容纳 \r\n-- + boundary + --
    char *bnd_copy = malloc(130);   // 足够容纳 boundary

    if (!buf || !close_bnd || !bnd_copy) {
        ESP_LOGE(TAG, "OTA内存分配失败");
        free(buf); free(close_bnd); free(bnd_copy);
        // 释放OTA会话锁（使用自旋锁保护）
        taskENTER_CRITICAL(&s_ota_spinlock);
        s_ota_session_active = false;
        taskEXIT_CRITICAL(&s_ota_spinlock);
        return send_json_error(req, 500, "内存不足");
    }

    // OTA超时计时器
    uint64_t start_time = esp_timer_get_time();  // 开始时间（微秒）
    uint64_t last_recv_time = start_time;        // 上次成功接收时间

    // 1. 解析 Content-Type 中的 boundary
    char content_type[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 Content-Type");
        goto cleanup;
    }

    char *bs = strstr(content_type, "boundary=");
    if (!bs) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺少 boundary 参数");
        goto cleanup;
    }
    bs += 9;  // 跳过 "boundary="

    // 处理引号包裹的 boundary（如 boundary="----WebKitFormBoundary..."）
    if (*bs == '"') {
        bs++;
    }

    // 构建关闭边界标记: "\r\n--<boundary>--"
    memset(close_bnd, 0, 160);
    memcpy(close_bnd, "\r\n--", 4);
    memset(bnd_copy, 0, 130);
    size_t bnd_len = 0;
    while (*bs && *bs != '"' && *bs != ';' && *bs != ' ' && bnd_len < 129) {
        bnd_copy[bnd_len++] = *bs++;
    }
    memcpy(close_bnd + 4, bnd_copy, bnd_len);

    ESP_LOGD(TAG, "OTA boundary: %s, close marker len: %u", bnd_copy, (unsigned)strlen(close_bnd));

    // 2. 开始 OTA 会话
    esp_err_t ret = ota_update_begin();
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_ARG) {
            send_json_error(req, 500, "版本相同，已拒绝。请编译新版本后再升级。");
        } else {
            char err_msg[128];
            snprintf(err_msg, sizeof(err_msg), "OTA 初始化失败: %s", esp_err_to_name(ret));
            send_json_error(req, 500, err_msg);
        }
        goto cleanup;
    }

    // 3. 流式接收，用尾缓冲处理跨 chunk 边界
    size_t tail_len = 0;       // 尾缓冲中的字节数（来自上一次迭代）
    bool headers_done = false; // 是否已跳过 multipart 头部
    size_t total_written = 0;  // 累计写入字节数（用于日志）

    while (true) {
        int len = httpd_req_recv(req, buf + tail_len, OTA_BUF_SIZE - tail_len);
        uint64_t now = esp_timer_get_time();

        if (len < 0) {
            if (len == HTTPD_SOCK_ERR_TIMEOUT) {
                // 检查总超时和无活动超时
                uint64_t total_elapsed_ms = (now - start_time) / 1000;
                uint64_t inactivity_ms = (now - last_recv_time) / 1000;

                if (total_elapsed_ms > OTA_RECV_TIMEOUT_MS) {
                    ESP_LOGE(TAG, "OTA总接收超时: %llu秒", total_elapsed_ms / 1000);
                    ota_update_abort();
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"上传总超时(30秒)\"}", HTTPD_RESP_USE_STRLEN);
                    goto cleanup;
                }

                if (inactivity_ms > OTA_INACTIVITY_TIMEOUT_MS) {
                    ESP_LOGE(TAG, "OTA无数据超时: %llu秒", inactivity_ms / 1000);
                    ota_update_abort();
                    httpd_resp_set_status(req, "500 Internal Server Error");
                    httpd_resp_set_type(req, "application/json");
                    httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"无数据接收超时(5秒)\"}", HTTPD_RESP_USE_STRLEN);
                    goto cleanup;
                }
                continue;  // 继续等待数据
            }
            ESP_LOGE(TAG, "OTA 接收失败: %d", len);
            ota_update_abort();
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"status\":\"error\",\"message\":\"网络接收失败\"}", HTTPD_RESP_USE_STRLEN);
            goto cleanup;
        }
        if (len == 0) {
            // 连接关闭，用已接收数据结束 OTA
            ESP_LOGW(TAG, "OTA 连接提前关闭，已接收 %lu 字节", (unsigned long)total_written);
            break;
        }

        // 成功接收数据，更新最后接收时间
        last_recv_time = now;

        size_t total = tail_len + len;  // 缓冲中总数据量
        tail_len = 0;

        // 首个 chunk: 跳过 multipart 头部
        if (!headers_done) {
            for (size_t i = 0; i + 3 < total; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' &&
                    buf[i+2] == '\r' && buf[i+3] == '\n') {
                    size_t remain = total - (i + 4);
                    if (remain > 0) {
                        memmove(buf, buf + i + 4, remain);
                    }
                    total = remain;
                    headers_done = true;
                    ESP_LOGD(TAG, "OTA multipart 头部已跳过，文件数据 %u 字节", (unsigned)total);
                    break;
                }
            }
            if (!headers_done) {
                tail_len = total;
                continue;
            }
        }

        if (total == 0) {
            continue;
        }

        // 搜索关闭边界 "\r\n--<boundary>--"
        const size_t cb_len = strlen(close_bnd);
        int found = -1;
        for (size_t i = 0; i + cb_len <= total; i++) {
            if (memcmp(buf + i, close_bnd, cb_len) == 0) {
                found = (int)i;
                break;
            }
        }

        if (found >= 0) {
            // 找到关闭边界，写入边界之前的数据
            if (found > 0) {
                ret = ota_update_write((const uint8_t *)buf, found);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "OTA 最终写入失败: %s", esp_err_to_name(ret));
                    ota_update_abort();
                    char err_buf[128];
                    snprintf(err_buf, sizeof(err_buf), "OTA 写入失败: %s", esp_err_to_name(ret));
                    send_json_error(req, 500, err_buf);
                    goto cleanup;
                }
                total_written += found;
            }
            ESP_LOGD(TAG, "OTA 接收完成，找到关闭边界，总计写入 %lu 字节", (unsigned long)total_written);
            goto done_receiving;
        }

        // 未找到关闭边界
        if (total > cb_len) {
            // 写入 total - cb_len 字节（保留最后 cb_len 字节用于下一轮边界检测）
            size_t write_len = total - cb_len;
            ret = ota_update_write((const uint8_t *)buf, write_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "OTA 流式写入失败: %s", esp_err_to_name(ret));
                ota_update_abort();
                char err_buf[128];
                snprintf(err_buf, sizeof(err_buf), "OTA 写入失败: %s", esp_err_to_name(ret));
                send_json_error(req, 500, err_buf);
                goto cleanup;
            }
            total_written += write_len;
            // 保留最后 cb_len 字节作为尾缓冲
            memmove(buf, buf + write_len, cb_len);
            tail_len = cb_len;

            if (total_written % (64 * 1024) < write_len) {
                ESP_LOGD(TAG, "OTA 已接收 %lu 字节", (unsigned long)total_written);
            }
        } else {
            // 数据太少（total <= cb_len），全部保留到尾缓冲等待下一轮
            tail_len = total;
            // 数据已在buf原位，无需搬移
        }
    }

done_receiving:
    // 4. 完成 OTA
    ret = ota_update_end();
    if (ret != ESP_OK) {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "OTA 校验失败: %s", esp_err_to_name(ret));
        send_json_error(req, 500, err_buf);
        goto cleanup;
    }

    // 5. 发送成功响应
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"固件升级成功，系统正在重启\"}");

    // 6. 释放资源后重启（必须在响应发送完成后释放锁和缓冲区）
    result = ESP_OK;
    goto cleanup_restart;

cleanup:
    // 释放动态分配的缓冲区
    free(buf);
    free(close_bnd);
    free(bnd_copy);

    // 释放OTA会话锁
    taskENTER_CRITICAL(&s_ota_spinlock);
    s_ota_session_active = false;
    taskEXIT_CRITICAL(&s_ota_spinlock);
    return result;

cleanup_restart:
    free(buf);
    free(close_bnd);
    free(bnd_copy);
    taskENTER_CRITICAL(&s_ota_spinlock);
    s_ota_session_active = false;
    taskEXIT_CRITICAL(&s_ota_spinlock);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return result;  // 不可达，保留以消除编译器警告
}

/**
 * @brief OTA 状态查询
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    cJSON *root = cJSON_CreateObject();
    // FAILED 状态无活跃会话时，自动恢复为 idle
    ota_state_t ota_st = ota_update_get_state();
    if (ota_st == OTA_STATE_FAILED && ota_update_get_bytes_written() == 0) {
        ota_st = OTA_STATE_IDLE;
    }
    cJSON_AddStringToObject(root, "state",
        ota_st == OTA_STATE_IDLE ? "idle" :
        ota_st == OTA_STATE_UPLOADING ? "uploading" :
        ota_st == OTA_STATE_WRITING ? "writing" :
        ota_st == OTA_STATE_COMPLETE ? "complete" : "failed");
    cJSON_AddNumberToObject(root, "written", ota_update_get_bytes_written());

    // 获取当前运行分区信息
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(root, "partition", running->label);
        cJSON_AddNumberToObject(root, "partition_size", running->size);

        // factory分区是出厂固件，永远有效可OTA
        const char *rollback_status = "有效";
        bool can_ota = true;

        if (running->type == ESP_PARTITION_TYPE_APP &&
            running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
            // factory分区：出厂固件，永远有效
            rollback_status = "出厂固件";
            can_ota = true;
        } else {
            // OTA分区：检查固件状态
            esp_ota_img_states_t img_state;
            esp_err_t err = esp_ota_get_state_partition(running, &img_state);
            if (err == ESP_OK) {
                switch (img_state) {
                    case ESP_OTA_IMG_NEW:
                        rollback_status = "新固件";
                        can_ota = false;
                        break;
                    case ESP_OTA_IMG_VALID:
                        rollback_status = "有效";
                        can_ota = true;
                        break;
                    case ESP_OTA_IMG_PENDING_VERIFY:
                        rollback_status = "待验证";
                        can_ota = false;
                        break;
                    case ESP_OTA_IMG_INVALID:
                        rollback_status = "无效";
                        can_ota = false;
                        break;
                    case ESP_OTA_IMG_ABORTED:
                        rollback_status = "已中止";
                        can_ota = false;
                        break;
                    default:
                        rollback_status = "未知";
                        can_ota = false;
                        break;
                }
            } else {
                rollback_status = "读取失败";
                can_ota = false;
            }
        }
        cJSON_AddStringToObject(root, "rollback_status", rollback_status);
        cJSON_AddBoolToObject(root, "can_ota", can_ota);
    } else {
        cJSON_AddStringToObject(root, "partition", "unknown");
        cJSON_AddStringToObject(root, "rollback_status", "未知");
        cJSON_AddBoolToObject(root, "can_ota", false);
    }

    // 获取固件版本信息
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        cJSON_AddStringToObject(root, "running_version", app_desc->version);
        cJSON_AddStringToObject(root, "compile_date", app_desc->date);    // 编译日期
        cJSON_AddStringToObject(root, "compile_time", app_desc->time);    // 编译时间
    } else {
        cJSON_AddStringToObject(root, "running_version", "unknown");
        cJSON_AddStringToObject(root, "compile_date", "");
        cJSON_AddStringToObject(root, "compile_time", "");
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        cJSON_AddNumberToObject(root, "next_partition_size", next->size);
    }

    char *resp = cJSON_PrintUnformatted(root);
    if (!resp) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    free(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 固件预览API处理器（解析上传固件的版本信息）
 * @note 接收固件头部数据（至少256字节），返回版本、日期、时间
 */
static esp_err_t ota_preview_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);

    // 接收固件头部数据（至少256字节）
    char buf[512];  // 缓冲区足够解析app_desc
    int total_len = httpd_req_recv(req, buf, sizeof(buf));
    if (total_len <= 0) {
        ESP_LOGE(TAG, "接收固件数据失败");
        return send_json_error(req, 400, "接收数据失败");
    }

    // 解析固件信息
    char version[32] = {0};
    char date[16] = {0};
    char time[16] = {0};

    esp_err_t ret = ota_update_preview((uint8_t*)buf, total_len, version, date, time);
    if (ret != ESP_OK) {
        return send_json_error(req, 400, "无效的固件文件");
    }

    // 返回固件信息
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", version);
    cJSON_AddStringToObject(root, "date", date);
    cJSON_AddStringToObject(root, "time", time);

    char *resp = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    free(resp);

    return ESP_OK;
}

/**
 * @brief 恢复出厂固件API处理器
 */
static esp_err_t ota_factory_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    ESP_LOGI(TAG, "收到恢复出厂固件请求");
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = ota_update_revert_to_factory();
    if (err == ESP_OK) {
        // 成功设置，即将重启，返回响应
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"已设置启动分区为factory，设备将重启\"}");
        return ESP_OK;
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"未找到factory分区\"}");
        return ESP_OK;
    } else {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"设置失败: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }
}

/**
 * @brief 回滚到上一OTA固件API处理器
 */
static esp_err_t ota_rollback_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        send_401(req);
        return ESP_OK;
    }
    set_session_cookie(req);
    ESP_LOGI(TAG, "收到回滚到上一OTA固件请求");
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = ota_update_rollback();
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"已设置回滚分区，设备将重启\"}");
        return ESP_OK;
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"未找到可回滚的OTA分区\"}");
        return ESP_OK;
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"当前不在OTA分区运行，无法回滚\"}");
        return ESP_OK;
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"目标分区无有效固件\"}");
        return ESP_OK;
    } else {
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"回滚失败: %s\"}", esp_err_to_name(err));
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }
}

// ==================== 公共接口 ====================

esp_err_t web_server_init(void)
{
    if (ctx.initialized) return ESP_OK;

    // 抑制httpd_txrx模块的socket连接重置警告(error in recv: 104)
    // 这是浏览器关闭连接时的正常现象，无需警告
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    // 在启动时记录boot时间，避免延迟初始化的竞态
    if (g_boot_time == 0) {
        g_boot_time = esp_timer_get_time();
    }

    ctx.initialized = true;
    return ESP_OK;
}

esp_err_t web_server_deinit(void)
{
    web_server_stop();
    ctx.initialized = false;
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (!ctx.initialized) return ESP_ERR_INVALID_STATE;
    if (ctx.running) return ESP_OK;

    ESP_LOGI(TAG, "启动Web服务器端口 %d", ctx.config.port);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = ctx.config.port;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 26;
    cfg.stack_size = 8192;   // 流式输出后降至8KB（原20KB），峰值使用~5KB
    cfg.max_open_sockets = 3;  // ESP-IDF v6.0限制：LWIP_MAX_SOCKETS=6，httpd内部占用3
    cfg.recv_wait_timeout = 10;    // 接收超时10秒（默认5秒）
    cfg.send_wait_timeout = 10;    // 发送超时10秒（默认5秒）

    if (httpd_start(&ctx.server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "启动失败");
        return ESP_FAIL;
    }

    // 注册URI
    httpd_uri_t uri;
    uri.user_ctx = NULL;

    uri.uri = "/", uri.method = HTTP_GET, uri.handler = handle_index;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/favicon.ico", uri.method = HTTP_GET, uri.handler = handle_favicon;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/status", uri.method = HTTP_GET, uri.handler = handle_status;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/control", uri.method = HTTP_POST, uri.handler = handle_control;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/config", uri.method = HTTP_GET, uri.handler = handle_config_get;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/config", uri.method = HTTP_POST, uri.handler = handle_config_set;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/config/hardware", uri.method = HTTP_POST, uri.handler = handle_hardware_config_set;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/wifi/scan", uri.method = HTTP_GET, uri.handler = handle_wifi_scan;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/wifi", uri.method = HTTP_POST, uri.handler = handle_wifi_config;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/filter/reset", uri.method = HTTP_POST, uri.handler = handle_filter_reset;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/filter/capacity", uri.method = HTTP_POST, uri.handler = handle_filter_capacity;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/tds/calibrate", uri.method = HTTP_POST, uri.handler = tds_calibrate_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/mqtt/config", uri.method = HTTP_GET, uri.handler = handle_mqtt_config_get;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/mqtt/config", uri.method = HTTP_POST, uri.handler = handle_mqtt_config_set;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/admin", uri.method = HTTP_GET, uri.handler = handle_admin;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/ota/update", uri.method = HTTP_POST, uri.handler = ota_update_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/ota/status", uri.method = HTTP_GET, uri.handler = ota_status_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/ota/preview", uri.method = HTTP_POST, uri.handler = ota_preview_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/ota/factory", uri.method = HTTP_POST, uri.handler = ota_factory_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/ota/rollback", uri.method = HTTP_POST, uri.handler = ota_rollback_handler;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/ota", uri.method = HTTP_GET, uri.handler = handle_ota;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/logs", uri.method = HTTP_GET, uri.handler = handle_log_page;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/logs", uri.method = HTTP_GET, uri.handler = handle_log_api;
    httpd_register_uri_handler(ctx.server, &uri);

    uri.uri = "/api/logs/debug", uri.method = HTTP_GET, uri.handler = handle_log_debug;
    httpd_register_uri_handler(ctx.server, &uri);

    ctx.running = true;
    ESP_LOGI(TAG, "Web服务器已启动");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!ctx.running) return ESP_OK;
    if (ctx.server) { httpd_stop(ctx.server); ctx.server = NULL; }
    ctx.running = false;
    return ESP_OK;
}

esp_err_t web_server_set_config(const web_server_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memcpy(&ctx.config, config, sizeof(web_server_config_t));
    return ESP_OK;
}

esp_err_t web_server_get_config(web_server_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    memcpy(config, &ctx.config, sizeof(web_server_config_t));
    return ESP_OK;
}

bool web_server_is_running(void) { return ctx.running; }

esp_err_t web_server_get_status_string(char *buf, size_t len)
{
    if (!buf) return ESP_ERR_INVALID_ARG;
    snprintf(buf, len, "Web服务器: %s, 端口: %d", ctx.running ? "运行中" : "停止", ctx.config.port);
    return ESP_OK;
}