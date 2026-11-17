/*
 * SPDX-FileCopyrightText: 2026 Ly-Systems-Lab
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:WebConsole"
#include "esp_lib_utils.h"

#include "esp_brookesia_web_console.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
/* 注意：IDF 的 newlib **没有** <sys/statvfs.h>（编译报 fatal error 踩过）。
 * 查 FAT 容量用 IDF 自己的 esp_vfs_fat_info()，见 esp_vfs_fat.h。 */

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

/* 只为拿 BSP_SD_MOUNT_POINT（SD 卡的挂载点）。刻意用 BSP 的宏而不是自己写死 "/sdcard"，
 * 免得挂载点哪天改了这里跟着漂（相机、音乐播放器已经各有一份拷贝了，不再添第三份）。
 * 相应地 CMakeLists 的 PRIV_REQUIRES 要加 esp32_p4_wifi6_touch_lcd_x。 */
#include "bsp/esp-bsp.h"

namespace esp_brookesia::services
{

/* ======================================================================
 * 前置说明：上传为什么不用 multipart
 * ----------------------------------------------------------------------
 * 早年另一个小电视项目用的是 multipart 表单上传，配 ESPAsyncWebServer 的分块回调。
 * 但 IDF 的 esp_http_server **不带 multipart 解析器**，自己手写 boundary 扫描既啰嗦又容易错。
 * 前端是我们自己写的，所以直接改用「原始请求体 + 文件名走 query」：
 *
 *     POST /api/upload?name=<urlencoded>   Content-Type: application/octet-stream
 *     <文件原始字节就是 body>
 *
 * 好处：服务端只管按 Content-Length 流式读，不碰 boundary；前端 XHR 还能拿到上传进度。
 * ====================================================================== */

namespace
{

/* ---------------- 小工具 ---------------- */

/* SD 卡根。分区由 main.cpp 在开机时挂好。 */
constexpr const char *kSdRoot = BSP_SD_MOUNT_POINT;
/* 数据分区上的上传落点（与 WebConsole::UPLOAD_DIR 同一个值，这里只是省一次限定名）。 */
constexpr const char *kDataRoot = WebConsole::UPLOAD_DIR;

struct FileEntry {
    std::string name;
    size_t      size;
    bool        is_dir;     /* 列表里要能点进子目录 */
};

/* 收/发用的 4 KB 流式缓冲 —— **一律放堆上**。
 *
 * 为什么必须这样（2026-10-14 亲自踩过）：
 *    httpd 任务的栈只有 8 KB。把 4 KB 收包缓冲摆在栈上 ⇒ 一旦再加点东西
 *    （这次是给每个 handler 加的 parseTarget，784 字节）就会**把栈顶穿** ——
 *    现象是"设备自己重启"，串口上是 `Guru Meditation ... Stack protection fault`，
 *    崩在 `snprintf → _svfprintf_r`（newlib 的格式化函数自己也要一两百字节栈）。
 *    实测溢出 640 字节，且**连 /api/status 这种小 handler 都会崩**。
 * 用 RAII 而不是裸 malloc：本文件有好几个早退分支，裸 malloc 迟早漏一次。 */
struct StreamBuf {
    static constexpr size_t kSize = 4096;

    char *p = nullptr;

    StreamBuf()
    {
        p = static_cast<char *>(malloc(kSize));
    }
    ~StreamBuf()
    {
        free(p);
    }
    bool ok() const
    {
        return p != nullptr;
    }

    StreamBuf(const StreamBuf &)            = delete;
    StreamBuf &operator=(const StreamBuf &) = delete;
};

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void urlDecode(const char *in, std::string &out)
{
    out.clear();
    if (in == nullptr) return;
    for (const char *p = in; *p != '\0'; ++p) {
        if (*p == '%' && p[1] != '\0' && p[2] != '\0') {
            const int hi = hexVal(p[1]);
            const int lo = hexVal(p[2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                p += 2;
                continue;
            }
        }
        out.push_back(*p == '+' ? ' ' : *p);
    }
}

bool isAscii(const std::string &s)
{
    for (unsigned char c : s) {
        if (c >= 0x80) return false;
    }
    return true;
}

/* 只留 FAT 上安全且各系统都不会踩坑的字符 */
std::string keepSafeChars(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '~';
        out.push_back(ok ? static_cast<char>(c) : '_');
    }
    return out;
}

/* 把浏览器给的文件名变成"能直接 fopen 的名字"。传进来的字符串保证不含路径分隔符。
 *
 * 注意：本工程的 FAT 用 CONFIG_FATFS_CODEPAGE_437（西欧代码页），**存不了中文名** ——
 * 硬写进去会变成乱码甚至 FR_INVALID_NAME。所以主名含非 ASCII 时直接换成时间戳名，
 * 而不是留下一串无意义的下划线。扩展名通常仍是 ASCII，尽量保住。 */
std::string makeSafeName(const std::string &raw, bool &renamed)
{
    renamed = false;

    /* 1) 只取最后一段，挡掉 ../ 和 \ 之类的路径穿越 */
    const size_t slash = raw.find_last_of("/\\");
    const std::string base = (slash == std::string::npos) ? raw : raw.substr(slash + 1);

    /* 2) 拆主名 / 扩展名 */
    std::string stem = base;
    std::string ext;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = base.substr(0, dot);
        ext  = base.substr(dot + 1);
    }

    /* 3) 非 ASCII 主名 ⇒ 换成时间戳名 */
    if (!stem.empty() && !isAscii(stem)) {
        renamed = true;
        char buf[32];
        snprintf(buf, sizeof(buf), "upload_%lld", static_cast<long long>(time(nullptr)));
        stem = buf;
    }

    stem = keepSafeChars(stem);
    while (!stem.empty() && stem[0] == '.') {
        stem.erase(stem.begin());          /* 别造出隐藏文件，也别撞上 "." / ".." */
    }
    if (stem.empty()) stem = "file";
    if (stem.size() > 48) stem.resize(48);

    std::string e = keepSafeChars(ext);
    if (e.size() > 8) e.resize(8);
    if (!e.empty() && e.find_first_not_of('_') == std::string::npos) {
        e.clear();                          /* 扩展名全被替换掉了就没意义 */
    }

    return e.empty() ? stem : stem + "." + e;
}

/* 同名不覆盖：插 _1 / _2 ... */
std::string uniquePath(const std::string &dir, const std::string &name, std::string &out_name)
{
    struct stat st;
    std::string path = dir + "/" + name;
    if (stat(path.c_str(), &st) != 0) {
        out_name = name;
        return path;
    }

    std::string stem = name;
    std::string ext;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext  = name.substr(dot);
    }

    for (int i = 1; i < 1000; ++i) {
        char suffix[16];
        snprintf(suffix, sizeof(suffix), "_%d", i);
        const std::string cand = stem + suffix + ext;
        const std::string p    = dir + "/" + cand;
        if (stat(p.c_str(), &st) != 0) {
            out_name = cand;
            return p;
        }
    }

    out_name = name;                        /* 极端情况：放弃去重 */
    return path;
}

std::string jsonEscape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            out += b;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

/* 列一个目录。带 base 参数 —— 原来写死 WebConsole::UPLOAD_DIR，现在 SD 也要用。
 * 子目录**也列出来**（is_dir=true），否则卡上建的文件夹在网页上等于不存在。 */
void listDir(const std::string &base, std::vector<FileEntry> &out)
{
    out.clear();
    DIR *dir = opendir(base.c_str());
    if (dir == nullptr) return;

    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;

        const std::string full = base + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            out.push_back({ent->d_name, 0, true});
        } else if (S_ISREG(st.st_mode)) {
            out.push_back({ent->d_name, static_cast<size_t>(st.st_size), false});
        }
    }
    closedir(dir);

    /* 目录排前面、同类按名字排。**排序放在服务端**：卡上文件多的时候，
     * 靠前端排序等于每次刷新都重排一遍，而且顺序会随 readdir 抖。 */
    std::sort(out.begin(), out.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

/* 取一个 query 参数（已 url 解码）。没这个参数时返回 false、out 置空。
 *
 * 和 parseTarget 同一个理由：**大缓冲放堆上**（见上面 StreamBuf 的说明）。
 * 顺带把 4 个 handler 里一模一样的 8 行收敛成一处 —— 它们原来各自在栈上摆
 * `query[512] + raw_name[256]`，那是把 httpd 那点栈余量摊薄的主要来源之一。 */
bool queryParam(httpd_req_t *req, const char *key, std::string &out)
{
    out.clear();

    std::string q(512, '\0');
    char v[256] = {0};      /* 单个参数名留栈上，256 字节可以接受 */

    if (httpd_req_get_url_query_str(req, q.data(), q.size()) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(q.c_str(), key, v, sizeof(v)) != ESP_OK) {
        return false;
    }

    urlDecode(v, out);
    return true;
}

/* ---- 请求的"目标"：root（data | sd）+ dir（相对子目录）----
 *
 * 为什么要这一层：早期那版把 6 条路由**全部写死**在 WebConsole::UPLOAD_DIR 上，
 * 想让它也能操作 SD 卡，就得把"根"参数化。
 * **默认值刻意保持不变**：不带任何参数的请求，行为与早期版本完全一致（老书签不会坏）。
 *
 * dir 是**用户可控输入**，必须当不可信数据处理。规矩：
 *   ① 按 '/' 拆段、逐段校验；任一段为空 / "." / ".." ⇒ 整条拒绝（路径穿越）；
 *   ② 只认 '/' 当分隔符（'\\' 直接拒）；不接受绝对路径（开头的空段自然会被 ① 拦掉）；
 *   ③ 挡掉 FAT 上本来就非法的字符（`* ? " < > | :`）和控制字符 ——
 *      让它在 API 层干净地报错，而不是让 fopen 抛一个看不懂的失败；
 *   ④ 总长封顶，别让超长 query 撑爆下面的 snprintf。
 */
struct Target {
    std::string base;        /* 绝对路径，如 /sdcard/videos */
    std::string root;        /* "data" / "sd" */
    std::string dir;         /* 规范化后的相对目录（不含首尾斜杠），可为空 */
    size_t      max_upload;  /* 按目标分别限流，见 hpp 里 MAX_UPLOAD_SD 的说明 */
};

bool parseTarget(httpd_req_t *req, Target &out, std::string &err)
{
    /* 这两块大缓冲刻意放**堆**上，不放栈 —— 本函数被**每个** handler 在最顶上调用，
     *    而 httpd 任务的栈只有 8 KB。2026-10-14 实测：
     *    把 query[512] + raw_dir[256] 摆在栈上，光这 768 字节就把栈顶穿，
     *    `Guru Meditation ... Stack protection fault`，溢出 640 字节，崩在 `_svfprintf_r`。
     *    见上面 StreamBuf 的说明。 */
    std::string query(512, '\0');
    std::string raw_dir(192, '\0');
    char        raw_root[16] = {0};     /* 这个小，留栈上无所谓 */

    if (httpd_req_get_url_query_str(req, query.data(), query.size()) == ESP_OK) {
        httpd_query_key_value(query.c_str(), "root", raw_root, sizeof(raw_root));
        httpd_query_key_value(query.c_str(), "dir",  raw_dir.data(), raw_dir.size());
    }

    out.root = (raw_root[0] == '\0') ? "data" : raw_root;
    if (out.root == "data") {
        out.base       = kDataRoot;
        out.max_upload = WebConsole::MAX_UPLOAD;
    } else if (out.root == "sd") {
        out.base       = kSdRoot;
        out.max_upload = WebConsole::MAX_UPLOAD_SD;
    } else {
        err = "bad root";
        return false;
    }

    std::string raw;
    urlDecode(raw_dir.c_str(), raw);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();                 /* 容忍尾随斜杠，别让 "avi/" 变成非法 */
    }

    std::string norm;
    for (size_t i = 0; i < raw.size();) {
        size_t j = raw.find('/', i);
        if (j == std::string::npos) j = raw.size();
        const std::string seg = raw.substr(i, j - i);

        if (seg.empty() || seg == "." || seg == "..") {
            err = "bad dir";
            return false;
        }
        for (unsigned char c : seg) {
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' ||
                    c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                err = "bad dir";
                return false;
            }
        }
        if (!norm.empty()) norm += '/';
        norm += seg;
        i = j + 1;
    }
    if (norm.size() > 128) {
        err = "dir too long";
        return false;
    }

    out.dir = norm;
    if (!norm.empty()) {
        out.base += "/" + norm;
    }

    /* 目录必须真的存在。**这一条把"卡没插"和"目录是空的"分开了** ——
     * 前者要明确报出来（否则用户会以为卡上的东西没了），后者是正常的空列表。 */
    struct stat st;
    if (stat(out.base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        err = (out.root == "sd") ? "sd_not_mounted" : "no such dir";
        return false;
    }
    return true;
}

/* 统一的 JSON 错误回复。
 * esp_http_server **没有** HTTPD_409_CONFLICT 这类枚举（只有 400/401/403/404/405/408/
 *    411/413/414/431/500），所以"要自定义状态码"的场合必须自己 set_status 再 sendstr。
 * 一律回 JSON（而不是 httpd_resp_send_err 的 HTML），前端才能稳定地解析出 error 码说人话。 */
esp_err_t sendJsonError(httpd_req_t *req, const char *status, const char *err)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, body);
}

/* 目标解析失败的统一回复。
 * `sd_not_mounted` 特意用 **409** 而不是 400：它是**预期内**的状态（没插卡，或者开机后才插），
 *    不是"你请求写错了"。前端据此说人话（"卡没挂上"），跟"参数非法"分开。 */
esp_err_t sendTargetError(httpd_req_t *req, const std::string &err)
{
    const bool sd = (err == "sd_not_mounted");
    return sendJsonError(req, sd ? "409 Conflict" : "400 Bad Request", err.c_str());
}

std::string humanSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ull * 1024ull) {
        snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ull) {
        snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

/* ---------------- 前端页面 ----------------
 * 全部编进固件：**不引任何 CDN**，没有外网也能用（早年那个小电视项目踩过 CDN 离线退化的坑）。
 * 中文只出现在这里 —— 浏览器字体什么都有，不像板上字体只有精选字形子集。 */
const char *kIndexHtml = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ESP32-P4 网页控制台</title>
<style>
  :root{color-scheme:dark}
  *{box-sizing:border-box}
  body{margin:0;padding:16px;background:#0f1216;color:#e6e9ee;line-height:1.5;
       font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Noto Sans SC","PingFang SC","Microsoft YaHei",sans-serif}
  h1{font-size:19px;margin:4px 0 16px;font-weight:700}
  h2{font-size:14px;margin:0 0 10px;color:#9aa4b2;font-weight:600;letter-spacing:.3px}
  .card{background:#171b21;border:1px solid #242a33;border-radius:12px;padding:14px;margin-bottom:12px}
  .row{display:flex;justify-content:space-between;gap:12px;padding:5px 0;font-size:14px}
  .row .k{color:#8b95a3}
  .ok{color:#4cd964}.bad{color:#ff6b6b}
  .ip{font-size:21px;font-weight:700;color:#5ac8fa;word-break:break-all}
  input[type=file]{width:100%;font-size:14px;color:#c8cfd8;margin-bottom:10px}
  button{background:#1f6feb;color:#fff;border:0;border-radius:9px;padding:12px 18px;
         font-size:15px;font-weight:600;font-family:inherit}
  button:disabled{background:#2b3440;color:#7d8794}
  button.ghost{background:#262c35;color:#c8cfd8;padding:5px 12px;font-size:13px;font-weight:500}
  .bar{height:8px;background:#242a33;border-radius:4px;overflow:hidden;margin-top:10px;display:none}
  .bar>i{display:block;height:100%;width:0;background:#4cd964;transition:width .15s ease}
  .msg{font-size:13px;margin-top:8px;min-height:18px;color:#8b95a3}
  ul{list-style:none;margin:0;padding:0}
  li{display:flex;align-items:center;gap:10px;padding:9px 0;font-size:14px;
     border-bottom:1px solid #21262e;word-break:break-all}
  li:last-child{border-bottom:0}
  li .name{flex:1;min-width:0;font-weight:500;color:#dfe4ea;text-decoration:none}
  li .sz{color:#7d8794;white-space:nowrap;font-variant-numeric:tabular-nums}
  button.del{padding:4px 10px;font-size:12px;background:#3a2226;color:#ff9a9a}
  .empty{color:#6b7480;font-size:14px;padding:6px 0}
  .hd{display:flex;justify-content:space-between;align-items:center}
  /* ---- 目标切换 / 目录导航 ---- */
  .tabs{display:flex;gap:8px;margin-bottom:10px}
  .tab{flex:1;background:#262c35;color:#c8cfd8;padding:10px 8px;font-size:14px;border-radius:9px}
  .tab.on{background:#1f6feb;color:#fff}
  .path{font-size:13px;color:#8b95a3;word-break:break-all;margin:0 0 10px;
        font-variant-numeric:tabular-nums}
  .acts{display:flex;gap:8px}
  .acts button{flex:1}
  li .name.dir{color:#7fb2ff}
  .note{font-size:12px;color:#6b7480;margin-top:8px}
</style>
</head>
<body>
<h1>ESP32-P4 网页控制台</h1>

<div class="card">
  <h2>设备状态</h2>
  <div class="row"><span class="k">网络</span><span id="s-net">…</span></div>
  <div class="row"><span class="k">地址</span><span class="ip" id="s-ip">…</span></div>
  <div class="row"><span class="k">数据分区</span><span id="s-fs">…</span></div>
  <div class="row"><span class="k">SD 卡</span><span id="s-sd">…</span></div>
</div>

<div class="card">
  <h2>位置</h2>
  <div class="tabs">
    <button class="tab on" id="t-data">数据分区</button>
    <button class="tab" id="t-sd">SD 卡</button>
  </div>
  <div class="path" id="path">…</div>
  <div class="acts">
    <button class="ghost" id="updir">返回上级</button>
    <button class="ghost" id="mk">新建文件夹</button>
  </div>
  <div class="note" id="sdn"></div>
</div>

<div class="card">
  <h2>上传到当前目录</h2>
  <input type="file" id="f" multiple>
  <button id="up">上传</button>
  <div class="bar" id="bar"><i id="bar-i"></i></div>
  <div class="msg" id="msg"></div>
</div>

<div class="card">
  <div class="hd"><h2 style="margin:0" id="listhd">文件</h2><button class="ghost" id="rf">刷新</button></div>
  <ul id="list" style="margin-top:10px"></ul>
</div>

<script>
var $ = function (id) { return document.getElementById(id); };

/* 当前目标：root = data | sd，dir = 相对子目录（空串 = 根）。 */
var st = { root: 'data', dir: '' };

function fmt(b) {
  if (b >= 1048576) return (b / 1048576).toFixed(2) + ' MB';
  if (b >= 1024) return (b / 1024).toFixed(1) + ' KB';
  return b + ' B';
}

/* 所有接口都带 root/dir。**不带参数时行为保持不变** —— 老书签不会坏。 */
function qs(extra) {
  var p = 'root=' + encodeURIComponent(st.root);
  if (st.dir) p += '&dir=' + encodeURIComponent(st.dir);
  return extra ? p + '&' + extra : p;
}

function rootLabel() { return st.root === 'sd' ? '/sdcard' : '/data/upload'; }

/* 错误码 → 人话。sd_not_mounted 和"参数非法"要分开说：
 * 前者是"卡没插好"，后者才是"这个请求有问题"，混在一起用户会以为卡上的东西丢了。 */
function errText(e) {
  if (e === 'sd_not_mounted') return 'SD 卡没挂上 —— 卡要在开机前插好，然后重启设备';
  if (e === 'dir not empty') return '这个文件夹不是空的，先删掉里面的东西';
  if (e === 'file too large') return '文件太大：数据分区上限 8 MB / SD 卡上限 1 MB（走 WiFi 的实测安全线）。大文件请走 U 盘模式';
  if (e === 'name taken') return '已经有同名的文件了';
  if (e === 'bad name') return '这个名字不能用（只能用 ASCII 字母/数字/._-）';
  if (e === 'no such file') return '这个东西已经不在了，刷新一下';
  return '失败：' + (e || '未知错误');
}

function refreshStatus() {
  fetch('/api/status', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (d) {
    $('s-net').innerHTML = d.wifi ? '<span class="ok">已连接</span>' : '<span class="bad">未连接</span>';
    $('s-ip').textContent = d.ip || '—';
    $('s-fs').textContent = fmt(d.free) + ' 可用 / ' + fmt(d.total);
    $('s-sd').innerHTML = d.sd ? (fmt(d.sd_free) + ' 可用 / ' + fmt(d.sd_total))
                               : '<span class="bad">未挂载</span>';
  }).catch(function () {
    $('s-net').innerHTML = '<span class="bad">设备无响应</span>';
  });
}

function refreshFiles() {
  fetch('/api/files?' + qs(), { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (d) {
    var ul = $('list');
    ul.innerHTML = '';
    if (!d.ok) {
      $('path').textContent = '—';
      $('listhd').textContent = '文件';
      $('updir').disabled = true;
      ul.innerHTML = '<div class="empty">' + errText(d.error) + '</div>';
      return;
    }
    $('path').textContent = rootLabel() + (d.dir ? '/' + d.dir : '');
    $('listhd').textContent = d.root === 'sd' ? 'SD 卡' : '数据分区';
    $('updir').disabled = !d.dir;
    if (!d.files || d.files.length === 0) {
      ul.innerHTML = '<div class="empty">这个目录是空的</div>';
      return;
    }
    d.files.forEach(function (f) {
      var li = document.createElement('li');
      var nm = document.createElement('a');
      nm.className = 'name';
      if (f.dir) {
        nm.className += ' dir';
        nm.href = 'javascript:void(0)';
        nm.textContent = f.name + ' /';
        nm.onclick = function () {
          st.dir = st.dir ? (st.dir + '/' + f.name) : f.name;
          refreshFiles();
        };
        var tag = document.createElement('span');
        tag.className = 'sz';
        tag.textContent = '文件夹';
        li.appendChild(nm);
        li.appendChild(tag);
      } else {
        nm.href = '/api/download?' + qs('name=' + encodeURIComponent(f.name));
        nm.textContent = f.name;
        nm.title = '点击下载';
        var s = document.createElement('span');
        s.className = 'sz';
        s.textContent = fmt(f.size);
        li.appendChild(nm);
        li.appendChild(s);
      }
      var del = document.createElement('button');
      del.className = 'del';
      del.textContent = '删除';
      del.onclick = function () {
        if (!confirm('删除 ' + f.name + (f.dir ? '（整个文件夹）' : '') + ' ？')) return;
        fetch('/api/delete?' + qs('name=' + encodeURIComponent(f.name)), { method: 'POST' })
          .then(function (r) { return r.json(); })
          .then(function (d) {
            if (d && d.ok === false) { alert(errText(d.error)); }
            refreshFiles();
            refreshStatus();
          })
          .catch(function () {});
      };
      li.appendChild(del);
      ul.appendChild(li);
    });
  }).catch(function () {});
}

/* 上传。resolve 一个 {ok, err}：失败时把服务端的错误码带出来，
 * 免得用户只看到"完成：成功 0/1"却不知道为什么。 */
function uploadOne(file) {
  return new Promise(function (resolve) {
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/upload?' + qs('name=' + encodeURIComponent(file.name)));
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = function (ev) {
      if (ev.lengthComputable) {
        $('bar').style.display = 'block';
        $('bar-i').style.width = (ev.loaded / ev.total * 100).toFixed(1) + '%';
      }
    };
    xhr.onload = function () {
      var err = null;
      if (xhr.status !== 200) {
        try { err = JSON.parse(xhr.responseText).error; } catch (e) { err = 'HTTP ' + xhr.status; }
      }
      resolve({ ok: xhr.status === 200, err: err });
    };
    xhr.onerror = function () { resolve({ ok: false, err: '网络中断' }); };
    xhr.send(file);
  });
}

$('up').onclick = function () {
  var files = Array.prototype.slice.call($('f').files || []);
  if (files.length === 0) { $('msg').textContent = '先选个文件'; return; }
  $('up').disabled = true;
  var okCount = 0;
  var lastErr = null;
  var i = 0;
  function next() {
    if (i >= files.length) {
      $('up').disabled = false;
      $('f').value = '';
      $('msg').textContent = '完成：成功 ' + okCount + ' / ' + files.length +
                             (lastErr ? '（' + errText(lastErr) + '）' : '');
      setTimeout(function () { $('bar').style.display = 'none'; $('bar-i').style.width = '0'; }, 800);
      refreshFiles();
      refreshStatus();
      return;
    }
    var f = files[i];
    $('msg').textContent = '正在上传 ' + (i + 1) + '/' + files.length + '：' + f.name;
    i++;
    uploadOne(f).then(function (r) { if (r.ok) okCount++; else lastErr = r.err; next(); });
  }
  next();
};

/* 每个目标一句"须知"。SD 那句不是保守估计，是**实测结论**：
 *   2026-10-14 实测 —— 往 SD 传 ≤1 MB 成功（941 KB/s），**≥2 MB 会把板子打崩并重启**
 *   （esp-hosted 流式收包要一块连续的内部 DMA 内存，本板最大连续块只有 30~90 KB）。
 *   ⚠️ 所以这里只承诺 ≤1 MB；大文件走 USB 口（桌面上的「USB Drive」app）。
 *   ⚠️ 别把这条"提示"改成更松的口径 —— 它是为了**避免用户把板子搞崩**，不是免责声明。 */
var NOTES = {
  data: '上传、删除、下载都作用于上面显示的当前目录。',
  sd: '上传、删除、下载都作用于上面显示的当前目录。SD 卡要在开机前插好（板子没有卡检测脚）。'
    + '<br><span class="bad">⚠️ 经 WiFi 上传只保证 ≤1 MB 可用</span>'
    + '（更大的文件会让设备重启）。大文件请用「USB Drive」APP 进入 U 盘模式，'
    + '再通过 USB-C（OTG）口传输。'
};

$('sdn').innerHTML = NOTES[st.root];

function switchRoot(r) {
  st.root = r;
  st.dir = '';
  $('t-data').className = 'tab' + (r === 'data' ? ' on' : '');
  $('t-sd').className   = 'tab' + (r === 'sd' ? ' on' : '');
  $('sdn').innerHTML = NOTES[r];
  refreshFiles();
  refreshStatus();
}

$('t-data').onclick = function () { switchRoot('data'); };
$('t-sd').onclick = function () { switchRoot('sd'); };

$('updir').onclick = function () {
  if (!st.dir) return;
  var i = st.dir.lastIndexOf('/');
  st.dir = (i < 0) ? '' : st.dir.slice(0, i);
  refreshFiles();
};

$('mk').onclick = function () {
  var n = prompt('新文件夹的名字（ASCII：字母/数字/._-）');
  if (!n) return;
  fetch('/api/mkdir?' + qs('name=' + encodeURIComponent(n)), { method: 'POST' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (d && d.ok === false) { alert(errText(d.error)); return; }
      refreshFiles();
    })
    .catch(function () {});
};

$('rf').onclick = function () { refreshFiles(); refreshStatus(); };

refreshStatus();
refreshFiles();
setInterval(refreshStatus, 5000);
</script>
</body>
</html>
)HTML";

/* ---------------- 内置路由 ---------------- */

esp_err_t hRoot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, kIndexHtml);
}

esp_err_t hStatus(httpd_req_t *req)
{
    const WebConsole::Status st = WebConsole::getInstance().getStatus();

    char body[512];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"running\":%s,\"wifi\":%s,\"ip\":\"%s\","
             "\"files\":%d,\"total\":%llu,\"free\":%llu,"
             "\"sd\":%s,\"sd_total\":%llu,\"sd_free\":%llu}",
             st.running ? "true" : "false",
             st.wifi_connected ? "true" : "false",
             st.ip,
             st.file_count,
             static_cast<unsigned long long>(st.fs_total),
             static_cast<unsigned long long>(st.fs_free),
             st.sd_present ? "true" : "false",
             static_cast<unsigned long long>(st.sd_total),
             static_cast<unsigned long long>(st.sd_free));

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

esp_err_t hFiles(httpd_req_t *req)
{
    Target t;
    std::string err;
    if (!parseTarget(req, t, err)) {
        return sendTargetError(req, err);
    }

    std::vector<FileEntry> files;
    listDir(t.base, files);

    std::string body = "{\"ok\":true,\"root\":\"" + jsonEscape(t.root) +
                       "\",\"dir\":\"" + jsonEscape(t.dir) + "\",\"files\":[";
    for (size_t i = 0; i < files.size(); ++i) {
        if (i > 0) body += ",";
        char one[384];
        snprintf(one, sizeof(one), "{\"name\":\"%s\",\"size\":%u,\"dir\":%s}",
                 jsonEscape(files[i].name).c_str(),
                 static_cast<unsigned>(files[i].size),
                 files[i].is_dir ? "true" : "false");
        body += one;
    }
    body += "]}";

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body.c_str());
}

esp_err_t hUpload(httpd_req_t *req)
{
    /* 打上"存储忙"标记：UI 看到它就不去查文件系统，免得和这里的写盘抢 flash。
     * 实测不做这件事的话上传会被拖慢约 4 倍。 */
    WebConsole::StorageGuard storage_guard(WebConsole::getInstance());

    /* 0) 先定目标（root + dir）。放在最前面：目标非法就直接拒，别白读一个几十 MB 的 body。 */
    Target t;
    std::string terr;
    if (!parseTarget(req, t, terr)) {
        return sendTargetError(req, terr);
    }

    /* 1) 文件名走 query（见文件头的说明：不用 multipart） */
    std::string decoded;
    queryParam(req, "name", decoded);

    bool renamed = false;
    const std::string safe = makeSafeName(decoded, renamed);

    std::string final_name;
    const std::string path = uniquePath(t.base, safe, final_name);

    /* 2) 大小闸门。Content-Length 是必须的 —— 浏览器发 File 体时一定会带。
     * 上限**按目标分开**：/data 是 16 MB 的分区、SD 有 32 GB，
     *    用同一个数会要么撑爆分区、要么卡上放不下视频（见 hpp 里 MAX_UPLOAD_SD 的说明）。 */
    if (req->content_len == 0) {
        ESP_UTILS_LOGW("上传被拒：没有 Content-Length");
        return httpd_resp_send_err(req, HTTPD_411_LENGTH_REQUIRED, "Content-Length required");
    }
    if (req->content_len > t.max_upload) {
        ESP_UTILS_LOGW("上传被拒：%u 字节超过 %s 的上限 %u",
                       static_cast<unsigned>(req->content_len), t.root.c_str(),
                       static_cast<unsigned>(t.max_upload));
        return httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "file too large");
    }

    /* 3) 流式写盘：4 KB 一块，绝不把整个文件读进内存。
     *    内部 RAM 一直紧张（WiFi/LWIP 缓冲没走 PSRAM），这是硬约束不是偏好。 */
    FILE *fp = fopen(path.c_str(), "wb");
    if (fp == nullptr) {
        ESP_UTILS_LOGE("fopen(%s) 失败", path.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot create file");
    }

    /* 4 KB 收包缓冲**放堆上**：它原来是栈上的 `char buf[4096]`，而 httpd 栈只有 8 KB ——
     * 这正是 2026-10-14 那次栈溢出的主力（见 StreamBuf 的说明）。 */
    StreamBuf stream;
    if (!stream.ok()) {
        ESP_UTILS_LOGE("收包缓冲分配失败");
        remove(path.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    size_t remain  = req->content_len;
    size_t written = 0;
    int    timeouts = 0;
    bool   ok = true;

    /* [探针·内存] 2026-10-14：往 SD 卡传 2 MB 会把板子打崩在
     * esp-hosted 的 `sdio_rx_get_buffer`（那里 `MEM_ALLOC(len)` 返回 NULL ⇒ assert）。
     * 而同样 2 MB 传到 /data 是好的 ⇒ **"写 SD"这条路会吃掉某种内存**，这里就是量它。
     * 每 512 KB 打一行：内部 RAM 余量 / **内部 RAM 最大连续块** / PSRAM 余量。
     * 为什么专门看"最大连续块"：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 默认 16384，
     * 意味着**小于 16 KB 的 malloc 一律先要内部 RAM** —— 崩溃那次要的正是 15360 字节。 */
    auto probe_mem = [](const char *when, size_t done_kb) {
        ESP_UTILS_LOGI("[探针·内存] %s 已写 %u KB：内部 free %u KB（**最大块 %u KB**）/ PSRAM free %u KB",
                       when,
                       static_cast<unsigned>(done_kb),
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                       static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                       static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    };
    probe_mem("开始", 0);

    while (remain > 0) {
        const size_t want = (remain < StreamBuf::kSize) ? remain : StreamBuf::kSize;
        const int got = httpd_req_recv(req, stream.p, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            /* 手机上切后台/信号弱会长时间没数据。给到 ~60 秒就放弃，别无限挂着。
             *
             * 2026-10-14 更正：这里原来是 `> 60`，而单次 recv 超时是 `recv_wait_timeout = 10` 秒
             *    ⇒ 实际是 **600 秒（10 分钟）** 才放弃，注释里"~60 秒"是错的。
             *    后果比"慢"严重得多：**esp_http_server 是单任务串行的** —— 一个卡住的请求会把
             *    整个控制台堵死 10 分钟（连 `/api/status` 都不应答，害我一度以为板子死了）。
             *    ⇒ 改成 6（= 6 × 10 秒 ≈ 60 秒），与注释原本的意思一致。 */
            if (++timeouts > 6) {
                ESP_UTILS_LOGW("上传超时：%s（已收 %u/%u 字节，连续 %d 次收包超时）",
                               final_name.c_str(), static_cast<unsigned>(written),
                               static_cast<unsigned>(req->content_len), timeouts);
                ok = false;
                break;
            }
            continue;
        }
        if (got <= 0) {
            ok = false;
            break;
        }
        timeouts = 0;
        if (fwrite(stream.p, 1, static_cast<size_t>(got), fp) != static_cast<size_t>(got)) {
            ESP_UTILS_LOGE("写入失败：%s", final_name.c_str());
            ok = false;
            break;
        }
        const size_t before = written;
        written += static_cast<size_t>(got);
        remain  -= static_cast<size_t>(got);

        if (before / (512 * 1024) != written / (512 * 1024)) {
            probe_mem("", written / 1024);
        }
    }
    fclose(fp);

    /* 栈余量**平时就报出来**（每个进程只报一次）——
     * 栈溢出在串口上看起来只是"设备自己重启了"，极难定位。
     * 放在这里（而不是成功分支里）：**失败的那次往往才最靠近栈顶**，
     *    只在上传成功时打点会漏掉最有价值的那次。 */
    static bool stack_reported = false;
    if (!stack_reported) {
        stack_reported = true;
        ESP_UTILS_LOGI("httpd 任务栈余量 %u 字节（只报一次；配置值见 _startServer 里的 cfg.stack_size）",
                       static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }

    if (!ok || written != req->content_len) {
        remove(path.c_str());               /* 半截文件不留 */
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    ESP_UTILS_LOGI("已上传 %s/%s (%u 字节) [%s]%s", t.base.c_str(), final_name.c_str(),
                   static_cast<unsigned>(written), t.root.c_str(),
                   renamed ? "  [原文件名含非 ASCII，已改为时间戳名]" : "");

    char body[320];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"name\":\"%s\",\"size\":%u,\"root\":\"%s\",\"dir\":\"%s\"}",
             jsonEscape(final_name).c_str(), static_cast<unsigned>(written),
             jsonEscape(t.root).c_str(), jsonEscape(t.dir).c_str());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* 删除一个已上传的文件。
 *
 * 这里**不能**套用 makeSafeName()：它对非 ASCII 主名会换成时间戳名，
 *    那是"给新文件起名"的逻辑；删除要删的正是传进来的那个名字，改名就成了删错对象。
 *    ⇒ 改成"不合法就拒"，只挡路径穿越和控制字符。 */
bool isSafeStoredName(const std::string &name)
{
    if (name.empty() || name.size() > 96) return false;
    if (name == "." || name == "..") return false;

    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '/' || c == '\\') return false;    /* 挡掉路径穿越 */
    }
    return true;
}

esp_err_t hDelete(httpd_req_t *req)
{
    WebConsole::StorageGuard storage_guard(WebConsole::getInstance());

    Target t;
    std::string terr;
    if (!parseTarget(req, t, terr)) {
        return sendTargetError(req, terr);
    }

    std::string decoded;
    queryParam(req, "name", decoded);

    if (!isSafeStoredName(decoded)) {
        ESP_UTILS_LOGW("删除被拒：文件名不合法 [%s]", decoded.c_str());
        return sendJsonError(req, "400 Bad Request", "bad name");
    }

    const std::string path = t.base + "/" + decoded;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        ESP_UTILS_LOGW("删除失败：没有这个条目 [%s]", decoded.c_str());
        return sendJsonError(req, "404 Not Found", "no such file");
    }

    /* 目录也能删（rmdir 只对空目录成功）。**故意不递归删** ——
     * 递归删目录是"一个手滑删掉整棵树"的经典入口，而这里没有回收站。
     * 非空目录会失败并报 409，让用户先把里面的东西删掉。 */
    const bool is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !S_ISREG(st.st_mode)) {
        return sendJsonError(req, "400 Bad Request", "not a file");
    }
    const int rc = is_dir ? rmdir(path.c_str()) : remove(path.c_str());
    if (rc != 0) {
        ESP_UTILS_LOGW("删除失败：%s（目录非空？）[%s]", path.c_str(), decoded.c_str());
        return sendJsonError(req, "409 Conflict", is_dir ? "dir not empty" : "delete failed");
    }

    ESP_UTILS_LOGI("已删除 %s/%s%s", t.base.c_str(), decoded.c_str(), is_dir ? "/ (目录)" : "");

    char body[320];
    snprintf(body, sizeof(body), "{\"ok\":true,\"deleted\":\"%s\",\"root\":\"%s\",\"dir\":\"%s\"}",
             jsonEscape(decoded).c_str(), jsonEscape(t.root).c_str(), jsonEscape(t.dir).c_str());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* 在目标目录下建一个子目录。
 *
 * 为什么需要它：视频那条路要把文件放进 `/sdcard/avi/`（VideoPlayer 写死的路径），
 * 而卡是**插在板子里**的、壳又不好拆 ⇒ 必须能**从网页上建目录**，否则只能全堆在卡根。 */
esp_err_t hMkdir(httpd_req_t *req)
{
    WebConsole::StorageGuard storage_guard(WebConsole::getInstance());

    Target t;
    std::string terr;
    if (!parseTarget(req, t, terr)) {
        return sendTargetError(req, terr);
    }

    std::string decoded;
    queryParam(req, "name", decoded);

    /* 用**和删除同一套**校验：不合法就拒。
     * 不能套 makeSafeName() —— 那是"给上传文件起名"的逻辑，会把非 ASCII 主名换成时间戳名，
     *    那样用户永远建不出自己想要的名字（建 x 得到 upload_1712…，看着像 bug）。 */
    if (!isSafeStoredName(decoded)) {
        ESP_UTILS_LOGW("建目录被拒：名字不合法 [%s]", decoded.c_str());
        return sendJsonError(req, "400 Bad Request", "bad name");
    }

    const std::string path = t.base + "/" + decoded;

    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            /* 已经在了 —— 当成成功（幂等）。前端刷新一下就能用，不必报错。 */
            char body[320];
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"existed\":true,\"name\":\"%s\",\"root\":\"%s\",\"dir\":\"%s\"}",
                     jsonEscape(decoded).c_str(), jsonEscape(t.root).c_str(), jsonEscape(t.dir).c_str());
            httpd_resp_set_type(req, "application/json; charset=utf-8");
            return httpd_resp_sendstr(req, body);
        }
        ESP_UTILS_LOGW("建目录失败：同名文件已存在 [%s]", path.c_str());
        return sendJsonError(req, "409 Conflict", "name taken");
    }

    if (mkdir(path.c_str(), 0777) != 0) {
        ESP_UTILS_LOGE("mkdir(%s) 失败", path.c_str());
        return sendJsonError(req, "500 Internal Server Error", "mkdir failed");
    }

    ESP_UTILS_LOGI("已建目录 %s", path.c_str());

    char body[320];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"existed\":false,\"name\":\"%s\",\"root\":\"%s\",\"dir\":\"%s\"}",
             jsonEscape(decoded).c_str(), jsonEscape(t.root).c_str(), jsonEscape(t.dir).c_str());
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* 下载一个已上传的文件。
 * 用 httpd_resp_send_chunk 分块发，**不把整个文件读进内存**（照片/歌都是几 MB）。 */
esp_err_t hDownload(httpd_req_t *req)
{
    WebConsole::StorageGuard storage_guard(WebConsole::getInstance());

    Target t;
    std::string terr;
    if (!parseTarget(req, t, terr)) {
        return sendTargetError(req, terr);
    }

    std::string decoded;
    queryParam(req, "name", decoded);

    if (!isSafeStoredName(decoded)) {
        ESP_UTILS_LOGW("下载被拒：文件名不合法 [%s]", decoded.c_str());
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }

    const std::string path = t.base + "/" + decoded;

    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    }

    FILE *fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        ESP_UTILS_LOGE("fopen(%s) 失败", path.c_str());
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open");
    }

    /* 名字里的双引号会撑破 header，换掉 */
    std::string disp_name = decoded;
    for (char &c : disp_name) {
        if (c == '"' || c == '\r' || c == '\n') {
            c = '_';
        }
    }
    char disp[224];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", disp_name.c_str());

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* 同理，发包缓冲也放堆上（见 StreamBuf 的说明）。 */
    StreamBuf stream;
    if (!stream.ok()) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    size_t sent = 0;
    bool   ok = true;

    while (sent < static_cast<size_t>(st.st_size)) {
        const size_t got = fread(stream.p, 1, StreamBuf::kSize, fp);
        if (got == 0) {
            ok = false;             /* 读短了：文件被截断或读失败 */
            break;
        }
        if (httpd_resp_send_chunk(req, stream.p, got) != ESP_OK) {
            ok = false;             /* 客户端断开 */
            break;
        }
        sent += got;
    }
    fclose(fp);

    if (!ok) {
        ESP_UTILS_LOGE("下载中断：%s (%u/%u)", decoded.c_str(),
                       static_cast<unsigned>(sent), static_cast<unsigned>(st.st_size));
        return ESP_FAIL;            /* 已经发出去半截了，不能再补终结块 */
    }

    ESP_UTILS_LOGI("已下载 %s (%u 字节)", decoded.c_str(), static_cast<unsigned>(sent));

    /* 零长度块 = 结束标志 */
    return httpd_resp_send_chunk(req, nullptr, 0);
}

/* ---------------- addRoute 的跳板 ---------------- */

struct RouteCtx {
    WebConsole::Handler fn;
};

esp_err_t routeTrampoline(httpd_req_t *req)
{
    auto *ctx = static_cast<RouteCtx *>(req->user_ctx);
    if (ctx == nullptr || !ctx->fn) return ESP_FAIL;
    return ctx->fn(req);
}

/* ---------------- 断线重连：兜住出厂 WlanPage 留下的洞 ---------------- */

int                s_disconnect_retry = 0;
esp_timer_handle_t s_retry_timer      = nullptr;

void retryLater(void *)
{
    if (WebConsole::getInstance().isRunning()) {
        esp_wifi_connect();
    }
}

} // namespace

/* ======================================================================
 * WebConsole
 * ====================================================================== */

WebConsole &WebConsole::getInstance()
{
    static WebConsole instance;
    return instance;
}

void WebConsole::_onGotIp(void *, esp_event_base_t, int32_t, void *data)
{
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    if (event != nullptr) {
        ESP_UTILS_LOGI("拿到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
    s_disconnect_retry = 0;
}

void WebConsole::_onStaDisconnected(void *, esp_event_base_t, int32_t, void *data)
{
    auto *event = static_cast<wifi_event_sta_disconnected_t *>(data);
    ESP_UTILS_LOGW("WiFi 断开 (reason=%d)",
                   event != nullptr ? event->reason : -1);

    if (!WebConsole::getInstance().isRunning()) {
        return;     /* 控制台没在跑就别插手，免得和设置页抢 */
    }

    ++s_disconnect_retry;
    if (s_disconnect_retry <= 5) {
        ESP_UTILS_LOGI("立即重连（第 %d 次）", s_disconnect_retry);
        esp_wifi_connect();
        return;
    }

    /* 连续失败：改成 15 秒一次。**不能在这里 sleep** —— 这是事件循环任务，
     * 睡下去会把整个 esp_event 堵死。 */
    if (s_retry_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &retryLater;
        args.name     = "wc_wifi_retry";
        if (esp_timer_create(&args, &s_retry_timer) != ESP_OK) {
            s_retry_timer = nullptr;
            return;
        }
    }
    if (!esp_timer_is_active(s_retry_timer)) {
        ESP_UTILS_LOGW("已连续失败 %d 次，15 秒后再试", s_disconnect_retry);
        esp_timer_start_once(s_retry_timer, 15 * 1000 * 1000);
    }
}

bool WebConsole::_mountData()
{
    if (_wl != WL_INVALID_HANDLE) {
        return true;                        /* 已挂载 */
    }

    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed   = true;    /* 首次上电会把 16 MB 分区格成 FAT，一次性开销 */
    cfg.max_files                = 8;
    cfg.allocation_unit_size     = CONFIG_WL_SECTOR_SIZE;
    cfg.disk_status_check_enable = false;
    cfg.use_one_fat              = false;

    const esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, PARTITION, &cfg, &_wl);
    if (err != ESP_OK) {
        ESP_UTILS_LOGE("挂载数据分区 %s 失败: %s", PARTITION, esp_err_to_name(err));
        _wl = WL_INVALID_HANDLE;
        return false;
    }

    struct stat st;
    if (stat(UPLOAD_DIR, &st) != 0) {
        if (mkdir(UPLOAD_DIR, 0777) != 0) {
            ESP_UTILS_LOGE("创建上传目录 %s 失败", UPLOAD_DIR);
            return false;
        }
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        ESP_UTILS_LOGI("数据分区已挂载 %s (%s)：总 %s，可用 %s",
                       MOUNT_POINT, PARTITION,
                       humanSize(total).c_str(), humanSize(free_bytes).c_str());
    }
    return true;
}

void WebConsole::_ensureWifi()
{
    /* 出厂 Settings::initWifi() 开机时就做过 netif/event/wifi_init 了，
     * 所以下面全部按「幂等兜底」写：做过就跳过，没做过才补。 */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_UTILS_LOGW("esp_netif_init: %s", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_UTILS_LOGW("esp_event_loop_create_default: %s", esp_err_to_name(err));
    }

    /* 用 ifkey 判断，别直接调 create —— 它每次都会新建一个 netif，重复调就是泄漏 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&wcfg);
        if (err != ESP_OK) {
            ESP_UTILS_LOGE("esp_wifi_init 失败: %s", esp_err_to_name(err));
            return;
        }
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_storage(WIFI_STORAGE_FLASH);   /* 和出厂设置页共用 NVS 里那份凭据 */
        ESP_UTILS_LOGI("WiFi 驱动由网页控制台初始化（本次开机还没人动过）");
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_UTILS_LOGW("esp_wifi_start: %s", esp_err_to_name(err));
    }

    /* 省电模式实测把 ping 往返拉到 64~1035 ms，
     * 上传会很难受。开发板是 USB 供电，不差这点电 —— 关掉。 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (!_wifi_hooks_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &WebConsole::_onStaDisconnected, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,        &WebConsole::_onGotIp,        nullptr);
        _wifi_hooks_registered = true;
    }

    /* 用 NVS 里已有的凭据发起连接（凭据是出厂设置页存的） */
    wifi_config_t wc = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != '\0') {
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_UTILS_LOGW("esp_wifi_connect: %s", esp_err_to_name(err));
        }
    } else {
        ESP_UTILS_LOGW("NVS 里没有 WiFi 凭据 —— 请先去「设置 → WLAN」连一次网");
    }
}

void WebConsole::_registerBuiltinRoutes()
{
    struct RouteDef {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    };

    const RouteDef routes[] = {
        {"/",           HTTP_GET,  hRoot},
        {"/api/status", HTTP_GET,  hStatus},
        {"/api/files",  HTTP_GET,  hFiles},
        {"/api/upload", HTTP_POST, hUpload},
        {"/api/delete", HTTP_POST, hDelete},
        {"/api/download", HTTP_GET, hDownload},
        {"/api/mkdir",  HTTP_POST, hMkdir},
    };

    for (const RouteDef &r : routes) {
        httpd_uri_t def = {};
        def.uri      = r.uri;
        def.method   = r.method;
        def.handler  = r.handler;
        def.user_ctx = nullptr;

        const esp_err_t err = httpd_register_uri_handler(_server, &def);
        if (err != ESP_OK) {
            ESP_UTILS_LOGE("注册路由 %s 失败: %s", r.uri, esp_err_to_name(err));
        }
    }
}

bool WebConsole::_startServer()
{
    if (_server != nullptr) {
        return true;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 默认 4096 肯定不够（上传回调要 4 KB 收发缓冲），所以这里一直加到了 8192。
     * 2026-10-14 实测：**8192 也已经是满的** —— 给每个 handler 加上 parseTarget()
     *    （栈上 768 字节）之后，连 /api/status 这种小 handler 都把栈顶穿了
     *    （`Guru Meditation ... Stack protection fault`，溢出 640 字节，崩在 `_svfprintf_r`）。
     * 处理办法分两层，**不是只靠调大**：
     *   ① **把大缓冲全部搬到堆上**（4 KB 的收发缓冲 + parseTarget/queryParam 的 query 缓冲）
     *      —— 这才是根因，见上面 StreamBuf 的说明；
     *   ② 再加一档余量到 12 KB。`_svfprintf_r` 自己就要一两百字节起步，
     *      而本文件的 handler 里 snprintf 用得很多，栈余量必须留够。
     * 真实余量**不靠猜**：hUpload 跑完会打印 `uxTaskGetStackHighWaterMark()`（每个进程一次）。 */
    cfg.stack_size        = 12288;
    cfg.max_uri_handlers  = 12;
    cfg.lru_purge_enable  = true;   /* 手机反复刷新时别把 socket 占满 */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    const esp_err_t err = httpd_start(&_server, &cfg);
    if (err != ESP_OK) {
        ESP_UTILS_LOGE("httpd_start 失败: %s", esp_err_to_name(err));
        _server = nullptr;
        return false;
    }

    _registerBuiltinRoutes();
    ESP_UTILS_LOGI("HTTP 服务器已启动（端口 80）");
    return true;
}

bool WebConsole::ensureStorageMounted()
{
    return _mountData();
}

bool WebConsole::start()
{
    if (_server != nullptr) {
        return true;                        /* 已经在跑 */
    }

    /* 分区挂不上就没法存文件，直接算失败 */
    if (!ensureStorageMounted()) {
        return false;
    }

    /* WiFi 失败不算致命：服务器照样起，屏幕上/网页里能看到"未连接"，用户可以去设置页配网 */
    _ensureWifi();

    return _startServer();
}

void WebConsole::stopServer()
{
    if (_server == nullptr) {
        return;
    }

    /* `httpd_stop()` 会等所有在途请求收尾再释放任务与 socket。
     * 如果在**上传进行中**点这个按钮，最长会等一个请求的超时 —— 这是对的：
     *    直接拆会留下半个文件（httpd 的 handler 还在往里写）。 */
    httpd_stop(_server);
    _server = nullptr;
    ESP_UTILS_LOGI("网页控制台已停止（/data 仍挂载；下次进 app 会重新 start()）");
}

void WebConsole::stop()
{
    if (_server != nullptr) {
        httpd_stop(_server);
        _server = nullptr;
    }

    if (_wl != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount_rw_wl(MOUNT_POINT, _wl);
        _wl = WL_INVALID_HANDLE;
    }
}

WebConsole::Status WebConsole::getStatus(bool with_storage)
{
    Status st;
    st.running = (_server != nullptr);

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            snprintf(st.ip, sizeof(st.ip), IPSTR, IP2STR(&ip.ip));
            st.wifi_connected = true;
        }
    }

    /* 以上全是纯 RAM 查询。下面两项要读文件系统 —— 调用方在上传期间应该传 false。 */
    if (!with_storage) {
        return st;
    }

    std::vector<FileEntry> files;
    listDir(UPLOAD_DIR, files);
    st.file_count = static_cast<int>(files.size());

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        st.fs_total = total;
        st.fs_free  = free_bytes;
    }

    /* SD 卡。挂不上时 sd_present 保持 false —— 界面上要说"卡没挂上"，
     * 不能显示成 `0 字节可用`（那看着像卡满了）。 */
    uint64_t sd_total = 0;
    uint64_t sd_free  = 0;
    if (esp_vfs_fat_info(kSdRoot, &sd_total, &sd_free) == ESP_OK) {
        st.sd_present = true;
        st.sd_total   = sd_total;
        st.sd_free    = sd_free;
    }

    return st;
}

bool WebConsole::addRoute(const char *uri, httpd_method_t method, Handler handler)
{
    if (_server == nullptr || uri == nullptr || !handler) {
        ESP_UTILS_LOGE("addRoute: 服务器未启动，或参数为空");
        return false;
    }

    _owned_uris.emplace_back(uri);
    auto *ctx = new RouteCtx{std::move(handler)};

    httpd_uri_t def = {};
    def.uri      = _owned_uris.back().c_str();
    def.method   = method;
    def.handler  = routeTrampoline;
    def.user_ctx = ctx;

    const esp_err_t err = httpd_register_uri_handler(_server, &def);
    if (err != ESP_OK) {
        ESP_UTILS_LOGE("addRoute %s 失败: %s", uri, esp_err_to_name(err));
        delete ctx;
        _owned_uris.pop_back();
        return false;
    }

    ESP_UTILS_LOGI("已挂路由: %s", uri);
    return true;
}

} // namespace esp_brookesia::services
