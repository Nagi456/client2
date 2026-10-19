#pragma once
#include <winhttp.h>
#include "Updates.h"
namespace MyClientLauncher {
// HTTPS and normal Windows certificate verification; only our release assets
// are accepted as initial URLs. Redirects must remain HTTPS.
inline void DownloadHttps(const std::string& url, const fs::path& destination, uint64_t limit)
{
    if (!ReleaseUrl(url)) throw std::runtime_error("許可されていない更新配布先です。");
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        throw std::runtime_error("更新URLが不正です。");
    struct HttpHandle { HINTERNET value; ~HttpHandle() { if (value) WinHttpCloseHandle(value); } };
    HttpHandle session{WinHttpOpen(L"MyClient/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 10000, 10000, 15000, 15000))
        throw std::runtime_error("更新通信を開始できません。");
    HttpHandle connection{WinHttpConnect(session.value, std::wstring(parts.lpszHostName, parts.dwHostNameLength).c_str(), parts.nPort, 0)};
    if (!connection.value) throw std::runtime_error("更新配布先に接続できません。");
    HttpHandle request{WinHttpOpenRequest(connection.value, L"GET", std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) ||
        !WinHttpSendRequest(request.value, L"Cache-Control: no-cache\r\n", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("更新一覧またはDLLを取得できません。接続を確認してください。");
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("更新配布先から取得できません。HTTP " + std::to_string(status) + "（未公開の場合もあります）。");
    std::ofstream out(destination, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("ダウンロード保存先を開けません。");
    uint64_t total = 0;
    const auto start = GetTickCount64();
    char buffer[65536];
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer, sizeof buffer, &read)) throw std::runtime_error("ダウンロードが中断されました。");
        if (!read) break;
        total += read;
        if (total > limit || GetTickCount64() - start > 300000) throw std::runtime_error("ダウンロードのサイズまたは時間が上限を超えました。");
        out.write(buffer, read);
        if (!out) throw std::runtime_error("更新ファイルの書き込みに失敗しました。");
    }
    out.close();
    if (!out || total == 0) throw std::runtime_error("更新ファイルの保存に失敗しました。");
}
}
