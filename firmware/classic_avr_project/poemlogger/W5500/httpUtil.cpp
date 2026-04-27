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
#include "../Utilities.h"
#include "../EepromManager.h"

uint8_t http_get_cgi_handler(uint8_t * uri_name, uint8_t * buf, uint32_t * file_len)
{
	uint8_t ret = HTTP_OK;
	uint16_t len = 0;

	if(predefined_get_cgi_processor(uri_name, buf, &len))
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
	else
	{
		// ここで他のCGIハンドラを呼び出すことも可能 (例: example.cgi)
		// 今回はpredefined_set_cgi_processorしか使わないため、retはそのまま
	}

	// predefined_set_cgi_processorから返された値(HTTP_OK or HTTP_RESET)をそのまま返す
	return ret;
}

//補正係数ページのHTMLフォームを生成する共通関数
static int generate_correction_form_html(char* p_buf, uint16_t remaining_size, EepromManager::SensorType type)
{
	const char* type_char = (type == EepromManager::A_type) ? "A" : ((type == EepromManager::B_type) ? "B" : "C");
	const char* post_action = (type == EepromManager::A_type) ? "/correct_a.cgi" : ((type == EepromManager::B_type) ? "/correct_b.cgi" : "/correct_c.cgi");

	const CorrectionFactors& cfc = EepromManager::cFactors[type];
	const PerSensorCoefficients* cefs = cfc.coeffs;
	
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

//補正係数フォームからPOSTされたデータを処理する共通関数
static void process_correction_form_post(uint8_t * uri, EepromManager::SensorType type)
{
	uint8_t* param_value;
	CorrectionFactors new_factors = EepromManager::cFactors[type];

	// 各パラメータをパースして構造体を更新
	if ((param_value = get_http_param_value((char *)uri, "a1"))) new_factors.coeffs[0].temp_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b1"))) new_factors.coeffs[0].temp_b = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c1"))) new_factors.coeffs[0].humi_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d1"))) new_factors.coeffs[0].humi_b = atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a2"))) new_factors.coeffs[1].temp_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b2"))) new_factors.coeffs[1].temp_b = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c2"))) new_factors.coeffs[1].humi_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d2"))) new_factors.coeffs[1].humi_b = atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a3"))) new_factors.coeffs[2].temp_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b3"))) new_factors.coeffs[2].temp_b = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c3"))) new_factors.coeffs[2].humi_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d3"))) new_factors.coeffs[2].humi_b = atof((char*)param_value);
	
	if ((param_value = get_http_param_value((char *)uri, "a4"))) new_factors.coeffs[3].temp_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b4"))) new_factors.coeffs[3].temp_b = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c4"))) new_factors.coeffs[3].humi_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d4"))) new_factors.coeffs[3].humi_b = atof((char*)param_value);

	if ((param_value = get_http_param_value((char *)uri, "a5"))) new_factors.coeffs[4].temp_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "b5"))) new_factors.coeffs[4].temp_b = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "c5"))) new_factors.coeffs[4].humi_a = atof((char*)param_value);
	if ((param_value = get_http_param_value((char *)uri, "d5"))) new_factors.coeffs[4].humi_b = atof((char*)param_value);
	
	// CRCを再計算
	new_factors.crc = Utilities::crc8((uint8_t*)&new_factors, sizeof(CorrectionFactors) - sizeof(new_factors.crc));
	
	// EEPROMを更新
	EepromManager::SetCorrectionFactors(type, new_factors);
}

uint8_t predefined_get_cgi_processor(uint8_t * uri_name, uint8_t * buf, uint16_t * len)
{
	char* p_buf = (char*)buf;
	uint16_t remaining_size = 2048; // 送信バッファサイズ
	int written_len = 0;

	*p_buf = '\0'; // バッファをクリア

	// 共通ヘッダーとナビゲーションを書き込む
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

	// URIに応じて各ページのフォーム部分を追記する
	if (strcmp((const char *)uri_name, "net.cgi") == 0)
	{
		// ラベルテキストと引用符を削減
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
		EepromManager::net_settings.ip[0], EepromManager::net_settings.ip[1], EepromManager::net_settings.ip[2], EepromManager::net_settings.ip[3],
		EepromManager::net_settings.sn[0], EepromManager::net_settings.sn[1], EepromManager::net_settings.sn[2], EepromManager::net_settings.sn[3],
		EepromManager::net_settings.gw[0], EepromManager::net_settings.gw[1], EepromManager::net_settings.gw[2], EepromManager::net_settings.gw[3],
		EepromManager::net_settings.dns[0], EepromManager::net_settings.dns[1], EepromManager::net_settings.dns[2], EepromManager::net_settings.dns[3],
		EepromManager::net_settings.bip[0], EepromManager::net_settings.bip[1], EepromManager::net_settings.bip[2], EepromManager::net_settings.bip[3],
		EepromManager::net_settings.topic_suffix
		);
	}
	//A種センサの補正係数の設定
	else if (strcmp((const char *)uri_name, "correct_a.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, EepromManager::A_type);
	}
	//B種センサの補正係数の設定
	else if (strcmp((const char *)uri_name, "correct_b.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, EepromManager::B_type);
	}
	//C種センサの補正係数の設定
	else if (strcmp((const char *)uri_name, "correct_c.cgi") == 0)
	{
		written_len = generate_correction_form_html(p_buf, remaining_size, EepromManager::C_type);
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
		(double)EepromManager::vcCoefficients[0].vol0,
		(double)EepromManager::vcCoefficients[0].ccA,
		(double)EepromManager::vcCoefficients[0].ccB,
		(double)EepromManager::vcCoefficients[1].vol0,
		(double)EepromManager::vcCoefficients[1].ccA,
		(double)EepromManager::vcCoefficients[1].ccB,
		(double)EepromManager::vcCoefficients[2].vol0,
		(double)EepromManager::vcCoefficients[2].ccA,
		(double)EepromManager::vcCoefficients[2].ccB,
		(double)EepromManager::vcCoefficients[3].vol0,
		(double)EepromManager::vcCoefficients[3].ccA,
		(double)EepromManager::vcCoefficients[3].ccB
		);
	}
	else written_len = snprintf(p_buf, remaining_size, "<p>Not Found");

	if (written_len < 0 || (uint16_t)written_len >= remaining_size) { *len = 0; return 0; }
	p_buf += written_len;
	remaining_size -= written_len;

	// フッターを追記する
	written_len = snprintf(p_buf, remaining_size, "</body></html>");
	if (written_len < 0 || (uint16_t)written_len >= remaining_size) { *len = 0; return 0; }

	// 全体の長さを計算して返す
	*len = strlen((char*)buf);
	return 1;
}

uint8_t predefined_set_cgi_processor(uint8_t * uri_name, uint8_t * uri, uint8_t * buf, uint16_t * len)
{
	uint8_t* param_value;
	bool settings_updated = false;
	char redirect_path[32] = {0};

	// "/net.cgi" へのPOSTリクエストを処理
	if (strcmp((const char *)uri_name, "net.cgi") == 0)
	{
		// 現在の設定を一時的な構造体にコピー
		NetworkSettings new_settings = EepromManager::net_settings;

		// 各パラメータをパースして構造体を更新
		if ((param_value = get_http_param_value((char *)uri, "ip"))) Utilities::parse_ip_string((char*)param_value, new_settings.ip);
		if ((param_value = get_http_param_value((char *)uri, "sn"))) Utilities::parse_ip_string((char*)param_value, new_settings.sn);
		if ((param_value = get_http_param_value((char *)uri, "gw"))) Utilities::parse_ip_string((char*)param_value, new_settings.gw);
		if ((param_value = get_http_param_value((char *)uri, "dns"))) Utilities::parse_ip_string((char*)param_value, new_settings.dns);
		if ((param_value = get_http_param_value((char *)uri, "bip"))) Utilities::parse_ip_string((char*)param_value, new_settings.bip);
		
		if ((param_value = get_http_param_value((char *)uri, "topic"))) {
			strncpy(new_settings.topic_suffix, (char*)param_value, sizeof(new_settings.topic_suffix) - 1);
			new_settings.topic_suffix[sizeof(new_settings.topic_suffix) - 1] = '\0'; // ヌル終端を保証
		}

		// CRCを再計算
		new_settings.crc = Utilities::crc8((uint8_t*)&new_settings, sizeof(NetworkSettings) - sizeof(new_settings.crc));
		
		// 新しい設定でEEPROMを更新
		EepromManager::SetNetworkSettings(new_settings);

		settings_updated = true;
		strcpy(redirect_path, "/net.cgi");
	}
	
	// "/correct_a.cgi" へのPOSTリクエストを処理
	else if (strcmp((const char *)uri_name, "correct_a.cgi") == 0)
	{
		process_correction_form_post(uri, EepromManager::A_type);
		settings_updated = true;
		strcpy(redirect_path, "/correct_a.cgi");
	}
	// "/correct_b.cgi" へのPOSTリクエストを処理
	else if (strcmp((const char *)uri_name, "correct_b.cgi") == 0)
	{
		process_correction_form_post(uri, EepromManager::B_type);
		settings_updated = true;
		strcpy(redirect_path, "/correct_b.cgi");
	}
	// "/correct_c.cgi" へのPOSTリクエストを処理
	else if (strcmp((const char *)uri_name, "correct_c.cgi") == 0)
	{
		process_correction_form_post(uri, EepromManager::C_type);
		settings_updated = true;
		strcpy(redirect_path, "/correct_c.cgi");
	}
	// "/vel.cgi" へのPOSTリクエストを処理
	else if (strcmp((const char *)uri_name, "vel.cgi") == 0)
	{
		VelocityCharacteristicCoefficients new_coeffs[4];
		
		// 短縮されたname属性(v0, ca, cb)から値を取得
		if ((param_value = get_http_param_value((char *)uri, "v01"))) new_coeffs[0].vol0 = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca1"))) new_coeffs[0].ccA = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb1"))) new_coeffs[0].ccB = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v02"))) new_coeffs[1].vol0 = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca2"))) new_coeffs[1].ccA = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb2"))) new_coeffs[1].ccB = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v03"))) new_coeffs[2].vol0 = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca3"))) new_coeffs[2].ccA = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb3"))) new_coeffs[2].ccB = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "v04"))) new_coeffs[3].vol0 = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "ca4"))) new_coeffs[3].ccA = atof((char*)param_value);
		if ((param_value = get_http_param_value((char *)uri, "cb4"))) new_coeffs[3].ccB = atof((char*)param_value);

		// CRCを再計算してEEPROMを更新
		for(int i=0;i<4;i++){			
			new_coeffs[i].crc = Utilities::crc8((uint8_t*)&new_coeffs[i], sizeof(VelocityCharacteristicCoefficients) - sizeof(new_coeffs[i].crc));
			EepromManager::SetVCCoefficients(i, new_coeffs[i]);
		}

		settings_updated = true;
		strcpy(redirect_path, "/vel.cgi");
	}

	// いずれかの設定が更新された場合
	if (settings_updated)
	{
		// JavaScriptを使用して1秒後にリダイレクトするHTMLページを生成
		*len = snprintf_P((char *)buf, 2048,
		PSTR("<!DOCTYPE html><html><head><title>Settings Saved</title>"
			"<style>body{font-family:sans-serif;text-align:center;padding-top:50px;}</style></head>"
			"<body>"
			"<p>Settings have been saved.<br>Redirecting back in 1 second...</p>"
			"<script>setTimeout(function(){window.location.href='%s'},1000);</script>"
			"</body></html>"),
		redirect_path // リダイレクト先を動的に設定
		);
		
		return 1;
	}

	return 0; // どのURIにも一致しなかった
}
