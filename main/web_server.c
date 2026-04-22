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
#include "history_logger.h"
#include "ota_update.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>
#include <time.h>

static const char *TAG = "WEB";

// 系统启动时间（微秒），在编译时记录
static uint64_t g_boot_time = 0;

// 获取系统运行时间（秒）
static uint32_t get_uptime_sec(void)
{
    if (g_boot_time == 0) {
        g_boot_time = esp_timer_get_time();
    }
    return (uint32_t)((esp_timer_get_time() - g_boot_time) / 1000000);
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

// ==================== HTML页面 ====================

// 首页 - 只读显示
static const char html_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>净水器状态</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh}"
".container{max-width:600px;margin:0 auto}"
".card{background:rgba(255,255,255,0.95);padding:20px;margin:15px 0;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.1)}"
"h1{color:#fff;text-align:center;margin-bottom:20px;text-shadow:0 2px 4px rgba(0,0,0,0.2)}"
"h3{color:#333;margin:0 0 15px 0;padding-bottom:10px;border-bottom:2px solid #eee}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:10px}"
".stat{background:#f8f9fa;padding:12px;border-radius:12px;text-align:center}"
".stat-label{color:#666;font-size:11px;margin-bottom:4px}"
".stat-value{color:#333;font-size:20px;font-weight:600}"
".stat-value.good{color:#28a745}.stat-value.warn{color:#ffc107}.stat-value.error{color:#dc3545}"
".btn{padding:14px 24px;margin:10px;border:none;border-radius:12px;cursor:pointer;color:#fff;font-size:16px;font-weight:500;transition:all .2s;width:100%}"
".btn:hover{transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,0.2)}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2)}"
".filter-bar{height:8px;background:#e0e0e0;border-radius:4px;margin:8px 0;overflow:hidden}"
".filter-bar-fill{height:100%;border-radius:4px;transition:width .3s}"
".filter-bar-fill.good{background:linear-gradient(90deg,#28a745,#20c997)}"
".filter-bar-fill.warn{background:linear-gradient(90deg,#ffc107,#fd7e14)}"
".filter-bar-fill.error{background:linear-gradient(90deg,#dc3545,#c82333)}"
"</style></head><body>"
"<div class='container'>"
"<h1 style='color:#fff;margin:0 0 15px 0;text-align:center;text-shadow:0 2px 4px rgba(0,0,0,0.2)'>净水器</h1>"

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
"<div style='text-align:center;color:#666;font-size:12px;margin-top:10px'>总用水量: <span id='totalWater'>0</span> 升</div>"
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
"</div></div>"

"<button class='btn btn-primary' onclick=\"location.href='/admin'\">管理设置</button>"

"</div>"
"<script>"
"function $(id){return document.getElementById(id)}"
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
"$('cycles').textContent=d.cycles||0;"
"$('flushCycles').textContent=d.flushes||0;"
"$('prodTime').textContent=d.prodTime?(d.prodTime/3600).toFixed(1)+'h':'0h';"
"$('todayProd').textContent=d.todayProd?d.todayProd+'min':'0min';"
"var u=d.uptime||0,h=Math.floor(u/3600),m=Math.floor((u%3600)/60);if(h>=24){$('uptime').textContent=Math.floor(h/24)+'天'+(h%24)+'时';}else if(h>0){$('uptime').textContent=h+'时'+m+'分';}else{$('uptime').textContent=m+'分';}"
"$('wifiState').textContent=d.wifiState;"
"$('wifiIP').textContent=d.ip||'-';"
"})}"
"setInterval(update,3000);update();"
"</script></body></html>";

// 管理页面 - 配置和控制
static const char html_admin_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>净水器管理</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh}"
".container{max-width:600px;margin:0 auto}"
".card{background:rgba(255,255,255,0.95);padding:20px;margin:15px 0;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.1)}"
"h1{color:#fff;text-align:center;margin-bottom:20px;text-shadow:0 2px 4px rgba(0,0,0,0.2)}"
"h3{color:#333;margin:0 0 15px 0;padding-bottom:10px;border-bottom:2px solid #eee}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}"
".stat{background:#f8f9fa;padding:15px;border-radius:12px;text-align:center}"
".stat-label{color:#666;font-size:12px;margin-bottom:5px}"
".stat-value{color:#333;font-size:20px;font-weight:600}"
".btn{padding:12px 20px;margin:5px;border:none;border-radius:10px;cursor:pointer;color:#fff;font-size:14px;font-weight:500;transition:all .2s}"
".btn:hover{transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,0.2)}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2)}"
".btn-success{background:linear-gradient(135deg,#28a745,#20c997)}"
".btn-danger{background:linear-gradient(135deg,#dc3545,#c82333)}"
".btn-warning{background:linear-gradient(135deg,#ffc107,#fd7e14)}"
".btn-info{background:linear-gradient(135deg,#17a2b8,#20c997)}"
".btn-dark{background:linear-gradient(135deg,#343a40,#495057)}"
".btn-group{display:flex;flex-wrap:wrap;gap:8px}"
"input,select{padding:10px;border:2px solid #e0e0e0;border-radius:8px;font-size:14px;transition:border-color .2s;width:100%}"
"input:focus,select:focus{outline:none;border-color:#667eea}"
".form-row{margin:10px 0}"
".form-row label{display:block;color:#555;margin-bottom:5px;font-size:13px}"
".form-row small{color:#999;font-size:11px}"
".wifi-item{background:#f0f0f0;padding:10px;margin:5px 0;border-radius:8px;cursor:pointer;transition:background .2s}"
".wifi-item:hover{background:#e0e0e0}"
".back-btn{background:rgba(255,255,255,0.2);color:#fff;padding:10px 20px;border:none;border-radius:10px;cursor:pointer;font-size:14px;margin-bottom:10px}"
".back-btn:hover{background:rgba(255,255,255,0.3)}"
".filter-stat{text-align:center;padding:10px 5px}"
".filter-name{font-size:12px;color:#666;margin-bottom:4px}"
".filter-pct{font-size:18px;font-weight:600;margin-bottom:2px}"
".filter-sub{font-size:10px;color:#999}"
".filter-bar{height:8px;background:#e0e0e0;border-radius:4px;margin:4px 0;overflow:hidden}"
".filter-bar-fill{height:100%;border-radius:4px;transition:width .3s}"
".filter-bar-fill.good{background:linear-gradient(90deg,#28a745,#20c997)}"
".filter-bar-fill.warn{background:linear-gradient(90deg,#ffc107,#fd7e14)}"
".filter-bar-fill.error{background:linear-gradient(90deg,#dc3545,#c82333)}"
".dim-label{font-size:10px;color:#999}"
"</style></head><body>"
"<div class='container'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'>"
"<button class='back-btn' onclick=\"location.href='/'\" style='margin:0'>← 返回首页</button>"
"<button class='back-btn' onclick=\"location.href='/ota'\" style='margin:0'>固件升级 →</button>"
"</div>"
"<h1 style='color:#fff;margin:0 0 15px 0;text-align:center;text-shadow:0 2px 4px rgba(0,0,0,0.2)'>管理页面</h1>"

"<div class='card'><h3>控制面板</h3>"
"<div class='btn-group'>"
"<button class='btn btn-primary' onclick='normalFlush()'>常规冲洗</button>"
"<button class='btn btn-info' onclick='pureFlush()'>纯水洗膜</button>"
"<button class='btn btn-warning' onclick='filterFlush()'>换芯冲洗</button>"
"<button class='btn btn-dark' onclick='goStandby()'>待机</button>"
"<button class='btn btn-success' onclick='resetStop()'>复位</button>"
"<button class='btn btn-danger' onclick='shutdown()'>停止</button>"
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
"<div class='btn-group'><button class='btn btn-success' onclick='saveHardware()'>保存硬件配置</button></div>"
"</div>"

"<div class='card'><h3>系统配置</h3>"
"<div class='form-row'><label>常规冲洗时间 (秒)</label><input type='number' id='normalFlushDur' value='30'></div>"
"<div class='form-row'><label>纯水洗膜时间 (秒)</label><input type='number' id='pureFlushDur' value='15'></div>"
"<div class='form-row'><label>换芯冲洗时间 (分钟)</label><input type='number' id='filterFlushDur' value='60'></div>"
"<div class='form-row'><label>制水超时 (分钟)</label><input type='number' id='prodTimeout' value='180'></div>"
"<div class='form-row'><label>漏水确认 (秒)</label><input type='number' id='leakConfirm' value='5'></div>"
"<div class='form-row'><label>Flash保存周期</label><select id='saveInterval'><option value='10'>10分钟</option><option value='60'>1小时</option><option value='120'>2小时 (推荐)</option><option value='360'>6小时</option><option value='720'>12小时</option><option value='1440'>24小时</option></select></div>"
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
"function resetFilter(i){if(confirm('确认重置该滤芯？')){api('/api/filter/reset',{filter:i}).then(d=>{alert(d.filter_name+' 已重置');loadFilters()})}}"
"function saveFilterCaps(){api('/api/filter/capacity',{caps:[parseInt($('cap0').value),parseInt($('cap1').value),parseInt($('cap2').value),parseInt($('cap3').value),parseInt($('cap4').value)],times:[parseInt($('time0').value),parseInt($('time1').value),parseInt($('time2').value),parseInt($('time3').value),parseInt($('time4').value)]}).then(d=>alert(d.status||'已保存'))}"
"function saveConfig(){api('/api/config',{normalFlushDur:parseInt($('normalFlushDur').value),pureFlushDur:parseInt($('pureFlushDur').value),filterFlushDur:parseInt($('filterFlushDur').value)*60,prodTimeout:parseInt($('prodTimeout').value)*60,leakConfirm:parseInt($('leakConfirm').value),saveInterval:parseInt($('saveInterval').value),relayLevel:parseInt($('relayLevel').value),tdsInTh:parseFloat($('tdsInTh').value),tdsOutTh:parseFloat($('tdsOutTh').value),whValveOpen:parseInt($('whValveOpen').value),whPumpStop:parseInt($('whPumpStop').value),whValveClose:parseInt($('whValveClose').value)}).then(d=>alert(d.status||'已保存'))}"
"function saveHardware(){api('/api/config/hardware',{roMem:parseInt($('roMem').value),pumpType:parseInt($('pumpType').value),tankSize:parseInt($('tankSize').value)}).then(d=>alert(d.status||'已保存'))}"
"function scanWiFi(){$('wifiList').innerHTML='扫描中...';fetch('/api/wifi/scan').then(r=>r.json()).then(d=>{let h='';if(d.networks)d.networks.forEach(function(n){h+='<div class=\"wifi-item\" data-ssid=\"'+n.ssid.replace(/\"/g,'&quot;')+'\">'+n.ssid+' ('+n.rssi+'dBm)</div>'});$('wifiList').innerHTML=h||'未找到网络'})}"
"$('wifiList').addEventListener('click',function(e){var item=e.target.closest('.wifi-item');if(item){$('ssid').value=item.getAttribute('data-ssid')}});"
"function saveWiFi(){api('/api/wifi',{ssid:$('ssid').value,password:$('pass').value}).then(d=>alert(d.status))}"
"function calibrateTDS(sensor){const v=sensor===0?$('tdsInCal').value:$('tdsOutCal').value;if(!v)return alert('请输入标准值');api('/api/tds/calibrate',{sensor:parseInt(sensor),value:parseFloat(v)}).then(d=>alert(d.status||'校准完成'))}"
"function loadMQTT(){fetch('/api/mqtt/config').then(r=>r.json()).then(d=>{$('mqttEn').value=d.enabled?1:0;$('mqttBroker').value=d.broker||'';$('mqttUser').value=d.user||'';$('mqttPass').value='';$('mqttPrefix').value=d.prefix||'water-purifier'})}"
"function saveMQTT(){api('/api/mqtt/config',{enabled:$('mqttEn').value==1,broker:$('mqttBroker').value,user:$('mqttUser').value,password:$('mqttPass').value,prefix:$('mqttPrefix').value}).then(d=>alert(d.status||'已保存'))}"
"loadConfig();loadFilters();loadMQTT();"
"</script></body></html>";

// 固件升级页面 - 独立页面
static const char html_ota_page[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>固件升级</title>"
"<style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:0;padding:20px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh}"
".container{max-width:600px;margin:0 auto}"
".card{background:rgba(255,255,255,0.95);padding:20px;margin:15px 0;border-radius:16px;box-shadow:0 8px 32px rgba(0,0,0,0.1)}"
"h1{color:#fff;text-align:center;margin-bottom:20px;text-shadow:0 2px 4px rgba(0,0,0,0.2)}"
"h3{color:#333;margin:0 0 15px 0;padding-bottom:10px;border-bottom:2px solid #eee}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}"
".stat{background:#f8f9fa;padding:15px;border-radius:12px;text-align:center}"
".stat-label{color:#666;font-size:12px;margin-bottom:5px}"
".stat-value{color:#333;font-size:20px;font-weight:600}"
".btn{padding:12px 20px;margin:5px;border:none;border-radius:10px;cursor:pointer;color:#fff;font-size:14px;font-weight:500;transition:all .2s}"
".btn:hover{transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,0.2)}"
".btn-success{background:linear-gradient(135deg,#28a745,#20c997)}"
".btn-primary{background:linear-gradient(135deg,#667eea,#764ba2)}"
".btn-group{display:flex;flex-wrap:wrap;gap:8px}"
"input,select{padding:10px;border:2px solid #e0e0e0;border-radius:8px;font-size:14px;transition:border-color .2s;width:100%}"
"input:focus,select:focus{outline:none;border-color:#667eea}"
".form-row{margin:10px 0}"
".form-row label{display:block;color:#555;margin-bottom:5px;font-size:13px}"
".form-row small{color:#999;font-size:11px}"
".back-btn{background:rgba(255,255,255,0.2);color:#fff;padding:10px 20px;border:none;border-radius:10px;cursor:pointer;font-size:14px;margin-bottom:10px}"
".back-btn:hover{background:rgba(255,255,255,0.3)}"
"</style></head><body>"
"<div class='container'>"
"<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:15px'>"
"<button class='back-btn' onclick=\"location.href='/admin'\" style='margin:0'>← 返回管理</button>"
"<button class='back-btn' onclick=\"location.href='/'\" style='margin:0'>首页 →</button>"
"</div>"
"<h1 style='color:#fff;margin:0 0 15px 0;text-align:center;text-shadow:0 2px 4px rgba(0,0,0,0.2)'>固件升级 (OTA)</h1>"

"<div class='card'><h3>版本信息</h3>"
"<div class='grid'>"
"<div class='stat'><div class='stat-label'>当前版本</div><div id='fwVer' class='stat-value'>-</div></div>"
"<div class='stat'><div class='stat-label'>编译时间</div><div id='compileTime' class='stat-value' style='font-size:14px'>-</div></div>"
"<div class='stat'><div class='stat-label'>升级状态</div><div id='otaState' class='stat-value'>就绪</div></div>"
"</div>"
"</div>"

"<div class='card'><h3>上传固件</h3>"
"<div class='form-row'>"
"<label>选择固件文件 (.bin)</label>"
"<input type='file' id='fwFile' accept='.bin' style='padding:8px'>"
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

"</div>"
"<script>"
"function $(id){return document.getElementById(id)}"
"function formatSize(b){if(!b||b<=0)return'-';return b<1024?b+'B':b<1048576?(b/1024).toFixed(1)+'KB':(b/1048576).toFixed(2)+'MB';}"
"function loadOTA(){fetch('/api/ota/status').then(r=>r.json()).then(d=>{$('fwVer').textContent=d.running_version||'-';$('compileTime').textContent=(d.compile_time||'')+' '+(d.compile_date||'')||'-';$('otaState').textContent=d.state==='idle'?'就绪':d.state})}"
"function uploadFirmware(){var f=$('fwFile').files[0];if(!f)return alert('请选择固件文件');if(!f.name.endsWith('.bin'))return alert('只支持 .bin 文件');if(!confirm('确认升级固件？设备将自动重启。'))return;var fd=new FormData();fd.append('firmware',f);$('otaProgress').style.display='block';$('otaState').textContent='上传中...';$('otaState').className='stat-value warn';var xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/update');xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);$('otaProgressText').textContent='上传中 '+formatSize(e.loaded)+'/'+formatSize(e.total);$('otaProgressPct').textContent=p+'%';$('otaProgressBar').style.width=p+'%';}};xhr.onload=function(){if(xhr.status===200){$('otaState').textContent='升级成功，重启中...';$('otaState').className='stat-value good';$('otaProgressText').textContent='升级完成，设备正在重启';$('otaProgressBar').style.width='100%';}else{try{var e=JSON.parse(xhr.responseText);alert(e.message||'升级失败');}catch(e){alert('升级失败: '+xhr.responseText);}$('otaState').textContent='升级失败';$('otaState').className='stat-value error';}};xhr.onerror=function(){$('otaState').textContent='网络错误';$('otaState').className='stat-value error';};xhr.send(fd);}"
"loadOTA();"
"</script></body></html>";

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
    char json_buf[256];
    snprintf(json_buf, sizeof(json_buf), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_send(req, json_buf, HTTPD_RESP_USE_STRLEN);
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_admin_page, strlen(html_admin_page));
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t *req)
{
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
        // 超时使用零值，不影响响应
        ESP_LOGW(TAG, "获取运行数据超时，使用默认值");
    }

    tds_dual_measurement_t tds;
    tds_sensor_get_latest_dual(&tds);

    // 获取五级滤芯状态
    filters_status_t filters_status;
    filter_mgr_get_filters_status(&filters_status);

    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));

    // 今日统计
    uint32_t today_prod_min = 0;
    daily_stats_t today;
    if (history_get_today_stats(&today) == ESP_OK) {
        today_prod_min = today.production_sec / 60;
    }

    // SNTP系统时间
    char sntp_time[64] = "未同步";
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year > 100) {
        snprintf(sntp_time, sizeof(sntp_time), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    }

    // 构建JSON响应（使用snprintf避免cJSON堆分配）
    char buf[2048];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"state\":\"%s\","
        "\"cycles\":%lu,"
        "\"flushes\":%lu,"
        "\"prodTime\":%llu,"
        "\"uptime\":%lu,"
        "\"todayProd\":%lu,"
        "\"tds_in\":%.1f,"
        "\"tds_out\":%.1f,"
        "\"rate\":%.1f,"
        "\"leak\":%s,"
        "\"sntpTime\":\"%s\","
        "\"filters\":[",
        fsm_get_state_name(state),
        (unsigned long)data.total_production_cycles,
        (unsigned long)data.total_flush_cycles,
        (unsigned long long)data.total_production_time_sec,
        (unsigned long)get_uptime_sec(),
        (unsigned long)today_prod_min,
        tds.inlet.valid ? tds.inlet.tds_value : 0.0,
        tds.outlet.valid ? tds.outlet.tds_value : 0.0,
        tds.both_valid ? tds.reduction_rate : 0.0,
        gpio_driver_read_water_leak() ? "true" : "false",
        sntp_time);

    // 边界检查：确保有足够空间继续写入
    if (pos >= sizeof(buf) - 400) {
        ESP_LOGW(TAG, "JSON响应接近溢出，中止写入");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON overflow");
        return ESP_FAIL;
    }

    for (int i = 0; i < FILTER_COUNT; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "%s{\"waterPct\":%u,\"timePct\":%u,\"effPct\":%u,\"used\":%lu,\"total\":%lu,\"timeLimit\":%lu,\"needReplace\":%s}",
            i > 0 ? "," : "",
            filters_status.filters[i].percentage,
            filters_status.filters[i].time_percentage,
            filters_status.filters[i].effective_percentage,
            (unsigned long)filters_status.filters[i].used_liters,
            (unsigned long)filters_status.filters[i].total_liters,
            (unsigned long)filters_status.filters[i].time_limit_hours,
            filters_status.filters[i].replacement_needed ? "true" : "false");

        // 边界检查：确保有足够空间继续写入
        if (pos >= sizeof(buf) - 200) {
            ESP_LOGW(TAG, "JSON响应接近溢出，中止写入");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON overflow");
            return ESP_FAIL;
        }
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "],"
        "\"totalWater\":%lu,"
        "\"wifiState\":\"%s\","
        "\"ssid\":\"%s\","
        "\"ip\":\"%s\"}",
        (unsigned long)filters_status.total_water_used,
        wifi_manager_get_state_name(wifi_manager_get_state()),
        wifi_manager_get_ssid(),
        ip);

    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_control(httpd_req_t *req)
{
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
    cJSON_AddNumberToObject(root, "whValveOpen", cfg.water_hammer_valve_open_delay_ms);
    cJSON_AddNumberToObject(root, "whPumpStop", cfg.water_hammer_pump_stop_delay_ms);
    cJSON_AddNumberToObject(root, "whValveClose", cfg.water_hammer_valve_close_delay_ms);

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
    /* 检查请求体长度 */
    int content_len = req->content_len;
    if (content_len > 255) {
        ESP_LOGW(TAG, "config_set请求体过大: %d字节", content_len);
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

    config_manager_set_config(&cfg);

    // 先应用硬件配置，成功后再保存NVS
    fsm_set_production_rate_by_membrane(cfg.ro_membrane_type);

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
            // 数据太少，全部保留到尾缓冲
            memmove(buf, buf, total);
            tail_len = total;
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

    // 6. 延迟重启（HTTP响应已在socket buffer中）
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    result = ESP_OK;  // 成功

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
}

/**
 * @brief OTA 状态查询
 */
static esp_err_t ota_status_handler(httpd_req_t *req)
{
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

    // 获取当前运行分区的编译时间
    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc) {
        cJSON_AddStringToObject(root, "running_version", app_desc->version);
        cJSON_AddStringToObject(root, "compile_time", app_desc->date);
        cJSON_AddStringToObject(root, "compile_date", app_desc->time);
    } else {
        cJSON_AddStringToObject(root, "running_version", "unknown");
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(root, "partition", running->label);
        cJSON_AddNumberToObject(root, "partition_size", running->size);
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

// ==================== 公共接口 ====================

esp_err_t web_server_init(void)
{
    if (ctx.initialized) return ESP_OK;
    ESP_LOGI(TAG, "初始化Web服务器");

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
    cfg.max_uri_handlers = 18;

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

    uri.uri = "/ota", uri.method = HTTP_GET, uri.handler = handle_ota;
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