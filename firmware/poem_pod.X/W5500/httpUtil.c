/**
 * @file	httpUtil.c
 * @brief	HTTP Server Utilities	
 * @version 1.0
 * @date	2014/07/15
 * @par Revision
 *			2014/07/15 - 1.0 Release
 * @author	
 * \n\n @par Copyright (C) 1998 - 2014 WIZnet. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "httpUtil.h"

#include <avr/pgmspace.h>
#include "../eeprom_manager.h"
#include "../node_manager.h"
#include "../utility.h"

uint8_t http_get_cgi_handler(uint8_t * uri_name, uint8_t * uri, uint8_t * buf, uint32_t * file_len)
{
	uint8_t ret = HTTP_OK;
	uint16_t len = 0;

	if(predefined_get_cgi_processor(uri_name, uri, buf, &len))
	{
		;
	}
	else if(strcmp((const char *)uri_name, "example.cgi") == 0)
	{
		// To do
		;
	}
	else
	{
		// CGI file not found
		ret = HTTP_FAILED;
	}

	if(ret)	*file_len = len;
	return ret;
}

uint8_t http_post_cgi_handler(uint8_t * uri_name, st_http_request * p_http_request, uint8_t * buf, uint32_t * file_len)
{
	uint8_t ret;
	uint16_t len = 0;

	// predefined_set_cgi_processorの戻り値を直接ret変数に格納する
	ret = predefined_set_cgi_processor(uri_name, p_http_request->URI, buf, &len);

	if (ret > 0) // HTTP_OK(1) または HTTP_RESET(2) が返された場合
	{
		// predefined_set_cgi_processorで設定されたlenの値をfile_lenに反映させる
		*file_len = len;
	}
	
	return ret;
}

// HTTP CGI 応答用バッファサイズ。httpServer 側で確保しているバッファサイズと一致させる。
#define CGI_BUF_SIZE        2048

// /devices.cgi の 1 ページあたり表示台数。応答バッファ (2048 byte) に全 64 台は
// 載らないため分割する。ヘッダ ~710 + 行 ~170×5 + ナビ/フッタ ~250 ≈ 1.8 KB で収まる値。
#define DEV_PAGE_SIZE       5

// GET の URI (例: "/devices.cgi?p=2") からページ番号を取り出す。無指定・不正は 0。
static uint8_t parsePageParam(const uint8_t* uri)
{
    if (!uri) return 0;
    const char* q = strchr((const char*)uri, '?');
    if (!q) return 0;
    const char* p = strstr(q, "p=");
    if (!p) return 0;
    int v = atoi(p + 2);
    return (v < 0 || v > 255) ? 0 : (uint8_t)v;
}

// /net.cgi GET: 現在のネットワーク設定をフォーム HTML で返す。
// 戻り値: 1=処理した (buf 有効, *len 設定済み)、0=未処理 (上位で HTTP_FAILED にする)。
uint8_t predefined_get_cgi_processor(uint8_t * uri_name, uint8_t * uri, uint8_t * buf, uint16_t * len)
{
    if (strcmp((const char *)uri_name, "net.cgi") == 0) {
        const EM_NetworkSettings* s = &EM_Network_Settings;
        int written = snprintf((char*)buf, CGI_BUF_SIZE,
            "<!DOCTYPE html><html><head><title>PoEM-pod Network Settings</title>"
            "<style>"
            "body{font-family:sans-serif;width:430px;margin:10px auto;font-size:14px}"
            "label,p{margin:8px 0;display:block}"
            "input[type=text]{width:200px;padding:3px}"
            "input[type=submit]{padding:5px 15px;margin-top:8px}"
            "h3{margin:8px 0 4px}"
            "</style></head><body>"
            "<h3>Network</h3>"
            "<form method=post action=/net.cgi>"
            "<label>IP: <input type=text name=ip value=%u.%u.%u.%u></label>"
            "<label>SN: <input type=text name=sn value=%u.%u.%u.%u></label>"
            "<label>GW: <input type=text name=gw value=%u.%u.%u.%u></label>"
            "<label>DNS: <input type=text name=dns value=%u.%u.%u.%u></label>"
            "<h3>MQTT</h3>"
            "<label>Broker IP: <input type=text name=bip value=%u.%u.%u.%u></label>"
            "<label>Topic suffix: <input type=text name=topic value=%s maxlength=20></label>"
            "<p><input type=submit value=Save></p>"
            "</form>"
            "<p><a href=/>Back</a></p>"
            "</body></html>",
            s->ip[0],  s->ip[1],  s->ip[2],  s->ip[3],
            s->sn[0],  s->sn[1],  s->sn[2],  s->sn[3],
            s->gw[0],  s->gw[1],  s->gw[2],  s->gw[3],
            s->dns[0], s->dns[1], s->dns[2], s->dns[3],
            s->bip[0], s->bip[1], s->bip[2], s->bip[3],
            s->topic_suffix);

        if (written < 0 || written >= CGI_BUF_SIZE) { *len = 0; return 0; }
        *len = (uint16_t)written;
        return 1;
    }

    // /scan.cgi GET: I²C 全走査をその場で実行し、devices.cgi にリダイレクト。
    if (strcmp((const char *)uri_name, "scan.cgi") == 0) {
        NM_scan();
        int written = snprintf((char*)buf, CGI_BUF_SIZE,
            "<!DOCTYPE html><html><head>"
            "<meta http-equiv=refresh content=\"1;url=/devices.cgi\">"
            "<title>Scanning</title></head><body>"
            "<p>I2C scan complete (%u devices found). Redirecting...</p>"
            "</body></html>",
            (unsigned)NM_NodeCount);
        if (written < 0 || written >= CGI_BUF_SIZE) { *len = 0; return 0; }
        *len = (uint16_t)written;
        return 1;
    }

    // /devices.cgi GET: 検出済み子機一覧をテーブル + 名前編集フォームで返す。
    // 全台は応答バッファに載らないため DEV_PAGE_SIZE 台ずつのページングとする (?p=N、0 始まり)。
    if (strcmp((const char *)uri_name, "devices.cgi") == 0) {
        char* p   = (char*)buf;
        char* end = (char*)buf + CGI_BUF_SIZE;
        int n;

        uint8_t page_count = (NM_NodeCount + DEV_PAGE_SIZE - 1) / DEV_PAGE_SIZE;
        if (page_count == 0) page_count = 1;
        uint8_t page = parsePageParam(uri);
        if (page >= page_count) page = page_count - 1;
        uint8_t first = page * DEV_PAGE_SIZE;
        uint8_t last  = first + DEV_PAGE_SIZE;
        if (last > NM_NodeCount) last = NM_NodeCount;

        n = snprintf(p, end - p,
            "<!DOCTYPE html><html><head><title>PoEM-pod Devices</title>"
            "<style>"
            "body{font-family:sans-serif;width:520px;margin:10px auto;font-size:13px}"
            "table{border-collapse:collapse;width:100%%}"
            "th,td{padding:2px 6px;border-bottom:1px solid #ddd;text-align:left}"
            "th{background:#eee}"
            "input[type=text]{width:60px;padding:2px}"
            "input[type=number]{width:48px;padding:2px}"
            "input[type=submit],a.btn{padding:4px 12px;margin:8px 4px 0 0;display:inline-block}"
            "</style></head><body>"
            "<h3>Detected Devices (%u)</h3>"
            "<p style=\"font-size:11px;color:#666\">Addr is decimal (8-119 = 0x08-0x77). Changes take effect immediately.</p>"
            "<form method=post action=/devices.cgi>"
            "<table><tr><th>#<th>Addr<th>DeviceID<th>St1<th>N<th>Name</tr>",
            (unsigned)NM_NodeCount);
        if (n < 0 || n >= end - p) { *len = 0; return 0; }
        p += n;

        // 子機テーブル本体 (このページの分のみ)。バッファ残量を見て途中で切る (オーバーフロー防止)。
        // パラメータ名は a00~a63 / n00~n63 の 2 桁固定。1 桁だと POST 側の strstr 検索で
        // "n1" が "n10" に誤マッチするため、全キーを同じ長さに揃えて部分一致を排除する。
        for (uint8_t i = first; i < last; i++) {
            if ((end - p) < 280) break;  // 残り 280 byte 切ったらフォーム閉じへ
            const slave_node_t* nd = &NM_Nodes[i];
            n = snprintf(p, end - p,
                "<tr><td>%u"
                "<td><input type=number name=a%02u value=%u min=8 max=119>"
                "<td>%lu<td>0x%02X<td>%u"
                "<td><input type=text name=n%02u value=\"%s\" maxlength=15></tr>",
                (unsigned)i,
                (unsigned)i, (unsigned)nd->i2c_addr,
                (unsigned long)nd->device_id,
                (unsigned)nd->status1, (unsigned)nd->data_count,
                (unsigned)i, nd->name);
            if (n < 0 || n >= end - p) break;
            p += n;
        }

        // ページ移動ナビゲーション
        n = snprintf(p, end - p, "</table><p>");
        if (n < 0 || n >= end - p) { *len = 0; return 0; }
        p += n;
        if (page > 0) {
            n = snprintf(p, end - p,
                "<a class=btn href=\"/devices.cgi?p=%u\">&lt; Prev</a> ", (unsigned)(page - 1));
            if (n < 0 || n >= end - p) { *len = 0; return 0; }
            p += n;
        }
        n = snprintf(p, end - p, "Page %u/%u", (unsigned)(page + 1), (unsigned)page_count);
        if (n < 0 || n >= end - p) { *len = 0; return 0; }
        p += n;
        if (page + 1 < page_count) {
            n = snprintf(p, end - p,
                " <a class=btn href=\"/devices.cgi?p=%u\">Next &gt;</a>", (unsigned)(page + 1));
            if (n < 0 || n >= end - p) { *len = 0; return 0; }
            p += n;
        }

        n = snprintf(p, end - p,
            "</p>"
            "<p><input type=submit value=\"Save Names\">"
            "<a class=btn href=/scan.cgi>Rescan I2C</a>"
            "<a class=btn href=/>Back</a></p>"
            "</form></body></html>");
        if (n < 0 || n >= end - p) { *len = 0; return 0; }
        p += n;

        *len = (uint16_t)(p - (char*)buf);
        return 1;
    }

    return 0;
}

// 新アドレスが他の (自分以外の) ノードのアドレスと衝突しないかを判定。
// アドレス変更を「同時に複数」受け付けたときに片方が他方を上書きする事故を防ぐ。
static bool addrConflictsWithOthers(uint8_t self_idx, uint8_t new_addr)
{
    for (uint8_t j = 0; j < NM_NodeCount; j++) {
        if (j == self_idx) continue;
        if (NM_Nodes[j].i2c_addr == new_addr) return true;
    }
    return false;
}

// /devices.cgi POST: フォームから来た addr/name を子機へ I²C で書き戻す。
// 子機側で EEPROM に永続化される。形式: ?a0=...&n0=...&a1=...&n1=...
// 変更がないものは I²C を発行せずスキップ。アドレス変更を先に処理して、続けて name 変更。
static uint8_t process_devices_post(uint8_t * uri, uint8_t * buf, uint16_t * len)
{
    char param_name[6];        // "a63\0" / "n63\0" 程度
    char new_name[NODE_NAME_LEN];

    // ===== アドレス変更 (先に処理) =============================================
    // 同じ POST 内でアドレスと名前の両方が来ても、アドレス変更後に新アドレスで name を書ける。
    for (uint8_t i = 0; i < NM_NodeCount; i++) {
        // 2 桁固定 (a00~a63)。フォーム側と揃える。1 桁だと strstr の部分一致事故が起きる。
        snprintf(param_name, sizeof(param_name), "a%02u", (unsigned)i);
        uint8_t* val = get_http_param_value((char*)uri, param_name);
        if (!val) continue;

        // 範囲外 / 同値 / 衝突は黙ってスキップ
        int parsed = atoi((char*)val);
        if (parsed < I2C_ADDR_SCAN_FIRST || parsed > I2C_ADDR_SCAN_LAST) continue;
        uint8_t new_addr = (uint8_t)parsed;
        if (new_addr == NM_Nodes[i].i2c_addr) continue;
        if (addrConflictsWithOthers(i, new_addr)) continue;

        // 失敗しても他ノード処理は継続。成功時は NM_Nodes[i].i2c_addr が更新される。
        NM_writeAddress(&NM_Nodes[i], new_addr);
    }

    // ===== 名前変更 ============================================================
    for (uint8_t i = 0; i < NM_NodeCount; i++) {
        snprintf(param_name, sizeof(param_name), "n%02u", (unsigned)i);
        uint8_t* val = get_http_param_value((char*)uri, param_name);
        if (!val) continue;

        // 入力を NODE_NAME_LEN-1 文字に切り詰めて NUL 終端
        memset(new_name, 0, NODE_NAME_LEN);
        strncpy(new_name, (char*)val, NODE_NAME_LEN - 1);

        // 既存名と同じならスキップ (I²C と EEPROM の摩耗回避)
        if (memcmp(NM_Nodes[i].name, new_name, NODE_NAME_LEN) == 0) continue;

        // 子機 EEPROM へ永続化 (失敗しても他ノード処理は継続)
        NM_writeName(&NM_Nodes[i], new_name);
    }

    int written = snprintf((char*)buf, CGI_BUF_SIZE,
        "<!DOCTYPE html><html><head><title>Saved</title>"
        "<meta http-equiv=refresh content=\"1;url=/devices.cgi\">"
        "</head><body><p>Settings saved. Redirecting...</p></body></html>");
    if (written < 0 || written >= CGI_BUF_SIZE) { *len = 0; return HTTP_FAILED; }
    *len = (uint16_t)written;
    return HTTP_OK;
}

// /net.cgi POST: フォームから送られた IP/SN/GW/DNS/BrokerIP/Topic を反映して EEPROM 更新。
// 反映後はリダイレクト用の小さな HTML を返す。
uint8_t predefined_set_cgi_processor(uint8_t * uri_name, uint8_t * uri, uint8_t * buf, uint16_t * len)
{
    if (strcmp((const char *)uri_name, "devices.cgi") == 0) {
        return process_devices_post(uri, buf, len);
    }

    if (strcmp((const char *)uri_name, "net.cgi") != 0) {
        return HTTP_FAILED;
    }

    EM_NetworkSettings new_settings = EM_Network_Settings;
    uint8_t* param_value;

    if ((param_value = get_http_param_value((char *)uri, "ip"))) {
        parse_ipv4((char*)param_value, new_settings.ip);
    }
    if ((param_value = get_http_param_value((char *)uri, "sn"))) {
        parse_ipv4((char*)param_value, new_settings.sn);
    }
    if ((param_value = get_http_param_value((char *)uri, "gw"))) {
        parse_ipv4((char*)param_value, new_settings.gw);
    }
    if ((param_value = get_http_param_value((char *)uri, "dns"))) {
        parse_ipv4((char*)param_value, new_settings.dns);
    }
    if ((param_value = get_http_param_value((char *)uri, "bip"))) {
        parse_ipv4((char*)param_value, new_settings.bip);
    }
    if ((param_value = get_http_param_value((char *)uri, "topic"))) {
        strncpy(new_settings.topic_suffix, (char*)param_value,
                sizeof(new_settings.topic_suffix) - 1);
        new_settings.topic_suffix[sizeof(new_settings.topic_suffix) - 1] = '\0';
    }

    // 反映 (CRC は EM_updateEEPROM が再計算するためここでは触らない)
    EM_Network_Settings = new_settings;
    EM_updateEEPROM();

    int written = snprintf((char*)buf, CGI_BUF_SIZE,
        "<!DOCTYPE html><html><head><title>Saved</title>"
        "<style>body{font-family:sans-serif;text-align:center;padding-top:50px}</style>"
        "</head><body>"
        "<p>Settings saved. Redirecting...</p>"
        "<script>setTimeout(function(){location.href='/net.cgi'},1000);</script>"
        "</body></html>");

    if (written < 0 || written >= CGI_BUF_SIZE) {
        *len = 0;
        return HTTP_FAILED;
    }
    *len = (uint16_t)written;
    return HTTP_OK;
}

/*
// 補正係数ページのHTMLフォームを生成する共通関数
// SensorType は EepromManager.h で typedef enum 定義されている前提
static int generate_correction_form_html(char* p_buf, uint16_t remaining_size, SensorType type)
{
	const char* type_char = (type == SENSOR_TYPE_A) ? "A" : ((type == SENSOR_TYPE_B) ? "B" : "C");
	const char* post_action = (type == SENSOR_TYPE_A) ? "/correct_a.cgi" : ((type == SENSOR_TYPE_B) ? "/correct_b.cgi" : "/correct_c.cgi");

	// EepromManager_cFactors などのグローバル配列にアクセスする想定
	const CorrectionFactors* cfc = &EepromManager_cFactors[type];
	const PerSensorCoefficients* cefs = cfc->coeffs;
	
	return snprintf_P(p_buf, remaining_size,
	PSTR("<h3>Correction - Type %s</h3><form method=post action=%s><table>"
	"<tr><td>DBT1<td><input name=a1 value=%.3f>*x+<input name=b1 value=%.3f>"
	"<tr><td>HMD1<td><input name=c1 value=%.3f>*x+<input name=d1 value=%.3f>"
	"<tr><td>DBT2<td><input name=a2 value=%.3f>*x+<input name=b2 value=%.3f>"
	"<tr><td>HMD2<td><input name=c2 value=%.3f>*x+<input name=d2 value=%.3f>"
	"<tr><td>DBT3<td><input name=a3 value=%.3f>*x+<input name=b3 value=%.3f>"
	"<tr><td>HMD3<td><input name=c3 value=%.3f>*x+<input name=d3 value=%.3f>"
	"<tr><td>DBT4<td><input name=a4 value=%.3f>*x+<input name=b4 value=%.3f>"
	"<tr><td>HMD4<td><input name=c4 value=%.3f>*x+<input name=d4 value=%.3f>"
	"<tr><td>DBT5<td><input name=a5 value=%.3f>*x+<input name=b5 value=%.3f>"
	"<tr><td>HMD5<td><input name=c5 value=%.3f>*x+<input name=d5 value=%.3f>"
	"</table><p><input type=submit value=Save></form>"),
	type_char, post_action,
	(double)cefs[0].temp_a, (double)cefs[0].temp_b, (double)cefs[0].humi_a, (double)cefs[0].humi_b,
	(double)cefs[1].temp_a, (double)cefs[1].temp_b, (double)cefs[1].humi_a, (double)cefs[1].humi_b,
	(double)cefs[2].temp_a, (double)cefs[2].temp_b, (double)cefs[2].humi_a, (double)cefs[2].humi_b,
	(double)cefs[3].temp_a, (double)cefs[3].temp_b, (double)cefs[3].humi_a, (double)cefs[3].humi_b,
	(double)cefs[4].temp_a, (double)cefs[4].temp_b, (double)cefs[4].humi_a, (double)cefs[4].humi_b
	);
}

// 補正係数フォームからPOSTされたデータを処理する共通関数
static void process_correction_form_post(uint8_t * uri, SensorType type)
{
	uint8_t* param_value;
	CorrectionFactors new_factors = EepromManager_cFactors[type];

	if ((param_value = get_http_param_value((char *)uri, "a1"))) new_factors.coeffs[0].temp_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b1"))) new_factors.coeffs[0].temp_b = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c1"))) new_factors.coeffs[0].humi_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d1"))) new_factors.coeffs[0].humi_b = (float)atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a2"))) new_factors.coeffs[1].temp_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b2"))) new_factors.coeffs[1].temp_b = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c2"))) new_factors.coeffs[1].humi_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d2"))) new_factors.coeffs[1].humi_b = (float)atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a3"))) new_factors.coeffs[2].temp_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b3"))) new_factors.coeffs[2].temp_b = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c3"))) new_factors.coeffs[2].humi_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d3"))) new_factors.coeffs[2].humi_b = (float)atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a4"))) new_factors.coeffs[3].temp_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b4"))) new_factors.coeffs[3].temp_b = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c4"))) new_factors.coeffs[3].humi_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d4"))) new_factors.coeffs[3].humi_b = (float)atof((char*)param_value);

	if ((param_value = get_http_param_value((char *)uri, "a5"))) new_factors.coeffs[4].temp_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b5"))) new_factors.coeffs[4].temp_b = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c5"))) new_factors.coeffs[4].humi_a = (float)atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d5"))) new_factors.coeffs[4].humi_b = (float)atof((char*)param_value);
	
	// Utilities_crc8 などのC関数を呼び出す
	new_factors.crc = Utilities_crc8((uint8_t*)&new_factors, sizeof(CorrectionFactors) - sizeof(new_factors.crc));
	
	// EEPROMを更新するC関数を呼び出す
	EepromManager_SetCorrectionFactors(type, new_factors);
}

uint8_t predefined_get_cgi_processor(uint8_t * uri_name, uint8_t * buf, uint16_t * len)
{
	char* p_buf = (char*)buf;
	uint16_t remaining_size = 2048; 
	int written_len = 0;

	*p_buf = '\0'; 

	written_len = snprintf_P(p_buf, remaining_size,
	PSTR("<!DOCTYPE html><html><head><title>PoEM-Logger Configuration Menu</title><style>"
		"body{font-family:sans-serif;width:430px;margin:10px auto;font-size:14px}"
		"td{padding:1px 4px}label,p{margin:8px 0}h3{margin:8px 0 4px}"
		"input[type=text]{width:125px;padding:3px}"
		"input[type=submit]{padding:5px 15px;margin-top:8px}"
		".n{margin-bottom:10px;border-bottom:1px solid#ccc;padding-bottom:5px}"
		".n a{margin-right:8px;text-decoration:none;color:#07f}"
		"</style></head><body><h3>PoEM-Logger Configuration Menu</h3><div class=n>"
		"<a href=/net.cgi>Network</a> / "
		"<a href=/correct_a.cgi>Correction Factors A</a> / "
		"<a href=/correct_b.cgi>Correction Factors B</a> / "
		"<a href=/correct_c.cgi>Correction Factors C</a> / "
		"<a href=/vel.cgi>Velocity Coefficients</a>"
		"</div>"));
	
	if (written_len < 0 || (uint16_t)written_len >= remaining_size) { *len = 0; return 0; }
	p_buf += written_len;
	remaining_size -= written_len;

	if (strcmp((const char *)uri_name, "net.cgi") == 0)
	{
		written_len = snprintf_P(p_buf, remaining_size,
		PSTR("<h3>Network</h3><form method=post action=/net.cgi>"
			"<label>IP: <input type=text name=ip value=%d.%d.%d.%d></label><br>"
			"<label>SN: <input type=text name=sn value=%d.%d.%d.%d></label><br>"
			"<label>GW: <input type=text name=gw value=%d.%d.%d.%d></label><br>"
			"<label>DNS: <input type=text name=dns value=%d.%d.%d.%d></label>"
			"<h3>MQTT</h3>"
			"<label>Broker IP: <input type=text name=bip value=%d.%d.%d.%d></label><br>"
			"<label>Topic: <input type=text name=topic value=%s maxlength=10></label>"
			"<p><input type=submit value=Save></form>"
			),
		EepromManager_net_settings.ip[0], EepromManager_net_settings.ip[1], EepromManager_net_settings.ip[2], EepromManager_net_settings.ip[3],
		EepromManager_net_settings.sn[0], EepromManager_net_settings.sn[1], EepromManager_net_settings.sn[2], EepromManager_net_settings.sn[3],
		EepromManager_net_settings.gw[0], EepromManager_net_settings.gw[1], EepromManager_net_settings.gw[2], EepromManager_net_settings.gw[3],
		EepromManager_net_settings.dns[0], EepromManager_net_settings.dns[1], EepromManager_net_settings.dns[2], EepromManager_net_settings.dns[3],
		EepromManager_net_settings.bip[0], EepromManager_net_settings.bip[1], EepromManager_net_settings.bip[2], EepromManager_net_settings.bip[3],
		EepromManager_net_settings.topic_suffix
		);
	}
	else if (strcmp((const char *)uri_name, "correct_a.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, SENSOR_TYPE_A);
	}
	else if (strcmp((const char *)uri_name, "correct_b.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, SENSOR_TYPE_B);
	}
	else if (strcmp((const char *)uri_name, "correct_c.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, SENSOR_TYPE_C);
	}
	else if (strcmp((const char *)uri_name, "vel.cgi") == 0)
	{
		written_len = snprintf_P(p_buf, remaining_size,
		PSTR("<h3>Velocity</h3><form method=post action=/vel.cgi>"
			"<label>Vel 1:</label><br>"
			"<label>0V: <input type=text name=v01 value=%.3f> Coef A: <input type=text name=ca1 value=%.3f> Coef B: <input type=text name=cb1 value=%.3f></label><br>"
			"<label>Vel 2:</label><br>"
			"<label>0V: <input type=text name=v02 value=%.3f> Coef A: <input type=text name=ca2 value=%.3f> Coef B: <input type=text name=cb2 value=%.3f></label><br>"
			"<label>Vel 3:</label><br>"
			"<label>0V: <input type=text name=v03 value=%.3f> Coef A: <input type=text name=ca3 value=%.3f> Coef B: <input type=text name=cb3 value=%.3f></label><br>"
			"<label>Vel 4:</label><br>"
			"<label>0V: <input type=text name=v04 value=%.3f> Coef A: <input type=text name=ca4 value=%.3f> Coef B: <input type=text name=cb4 value=%.3f></label><br>"
			"<p><input type=submit value=Save></form>"),
		(double)EepromManager_vcCoefficients[0].vol0,
		(double)EepromManager_vcCoefficients[0].ccA,
		(double)EepromManager_vcCoefficients[0].ccB,
		(double)EepromManager_vcCoefficients[1].vol0,
		(double)EepromManager_vcCoefficients[1].ccA,
		(double)EepromManager_vcCoefficients[1].ccB,
		(double)EepromManager_vcCoefficients[2].vol0,
		(double)EepromManager_vcCoefficients[2].ccA,
		(double)EepromManager_vcCoefficients[2].ccB,
		(double)EepromManager_vcCoefficients[3].vol0,
		(double)EepromManager_vcCoefficients[3].ccA,
		(double)EepromManager_vcCoefficients[3].ccB
		);
	}
	else written_len = snprintf(p_buf, remaining_size, "<p>Not Found");

	if (written_len < 0 || (uint16_t)written_len >= remaining_size) { *len = 0; return 0; }
	p_buf += written_len;
	remaining_size -= written_len;

	written_len = snprintf(p_buf, remaining_size, "</body></html>");
	if (written_len < 0 || (uint16_t)written_len >= remaining_size) { *len = 0; return 0; }

	*len = (uint16_t)strlen((char*)buf);
	return 1;
}

uint8_t predefined_set_cgi_processor(uint8_t * uri_name, uint8_t * uri, uint8_t * buf, uint16_t * len)
{
	uint8_t* param_value;
	uint8_t settings_updated = 0;
	char redirect_path[32] = {0};

	if (strcmp((const char *)uri_name, "net.cgi") == 0)
	{
		EM_NetworkSettings new_settings = EM_Network_Settings;

		if ((param_value = get_http_param_value((char *)uri, "ip"))) Utilities_parse_ip_string((char*)param_value, new_settings.ip);
		if ((param_value = get_http_param_value((char *)uri, "sn"))) Utilities_parse_ip_string((char*)param_value, new_settings.sn);
		if ((param_value = get_http_param_value((char *)uri, "gw"))) Utilities_parse_ip_string((char*)param_value, new_settings.gw);
		if ((param_value = get_http_param_value((char *)uri, "dns"))) Utilities_parse_ip_string((char*)param_value, new_settings.dns);
		if ((param_value = get_http_param_value((char *)uri, "bip"))) Utilities_parse_ip_string((char*)param_value, new_settings.bip);
		
		if ((param_value = get_http_param_value((char *)uri, "topic"))) {
			strncpy(new_settings.topic_suffix, (char*)param_value, sizeof(new_settings.topic_suffix) - 1);
			new_settings.topic_suffix[sizeof(new_settings.topic_suffix) - 1] = '\0';
		}

		new_settings.crc = Utilities_crc8((uint8_t*)&new_settings, sizeof(NetworkSettings) - sizeof(new_settings.crc));
		EepromManager_SetNetworkSettings(new_settings);

		settings_updated = 1;
		strcpy(redirect_path, "/net.cgi");
	}
	else if (strcmp((const char *)uri_name, "correct_a.cgi") == 0)
	{
		process_correction_form_post(uri, SENSOR_TYPE_A);
		settings_updated = 1;
		strcpy(redirect_path, "/correct_a.cgi");
	}
	else if (strcmp((const char *)uri_name, "correct_b.cgi") == 0)
	{
		process_correction_form_post(uri, SENSOR_TYPE_B);
		settings_updated = 1;
		strcpy(redirect_path, "/correct_b.cgi");
	}
	else if (strcmp((const char *)uri_name, "correct_c.cgi") == 0)
	{
		process_correction_form_post(uri, SENSOR_TYPE_C);
		settings_updated = 1;
		strcpy(redirect_path, "/correct_c.cgi");
	}
	else if (strcmp((const char *)uri_name, "vel.cgi") == 0)
	{
		VelocityCharacteristicCoefficients new_coeffs[4];
		
		if ((param_value = get_http_param_value((char *)uri, "v01"))) new_coeffs[0].vol0 = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca1"))) new_coeffs[0].ccA = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb1"))) new_coeffs[0].ccB = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v02"))) new_coeffs[1].vol0 = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca2"))) new_coeffs[1].ccA = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb2"))) new_coeffs[1].ccB = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v03"))) new_coeffs[2].vol0 = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca3"))) new_coeffs[2].ccA = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb3"))) new_coeffs[2].ccB = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v04"))) new_coeffs[3].vol0 = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca4"))) new_coeffs[3].ccA = (float)atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb4"))) new_coeffs[3].ccB = (float)atof((char*)param_value);

		for(int i=0; i<4; i++){			
			new_coeffs[i].crc = Utilities_crc8((uint8_t*)&new_coeffs[i], sizeof(VelocityCharacteristicCoefficients) - sizeof(new_coeffs[i].crc));
			EepromManager_SetVCCoefficients(i, new_coeffs[i]);
		}

		settings_updated = 1;
		strcpy(redirect_path, "/vel.cgi");
	}

	if (settings_updated)
	{
		*len = (uint16_t)snprintf_P((char *)buf, 2048,
		PSTR("<!DOCTYPE html><html><head><title>Settings Saved</title>"
			"<style>body{font-family:sans-serif;text-align:center;padding-top:50px;}</style></head>"
			"<body>"
			"<p>Settings have been saved.<br>Redirecting back in 1 second...</p>"
			"<script>setTimeout(function(){window.location.href='%s'},1000);</script>"
			"</body></html>"),
		redirect_path
		);
		
		return 1;
	}

	return 0; 
}

*/