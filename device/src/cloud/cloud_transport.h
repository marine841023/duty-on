#pragma once

// Cloud traffic uses Windows' certificate store and TLS implementation. Local
// device/Hook HTTP servers continue to use cpp-httplib.
#include <winhttp.h>
#include <functional>
#include <memory>
#include <string>
#include <httplib.h>

namespace dutyon {
class CloudTransport {
    struct Handle {
        HINTERNET value = nullptr;
        explicit Handle(HINTERNET h = nullptr) : value(h) {}
        ~Handle() { if (value) WinHttpCloseHandle(value); }
        Handle(const Handle&) = delete;
    };
public:
    struct Response { int status = 0; std::string body; };
    using Result = std::shared_ptr<Response>;
    using Receiver = std::function<bool(const char*, size_t)>;
    CloudTransport(const std::string& origin, int = 0) {
        std::wstring url = wide(origin);
        URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
            parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
            !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength ||
            parts.dwExtraInfoLength || (parts.dwUrlPathLength &&
                std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) != L"/")) return;
        host_.assign(parts.lpszHostName, parts.dwHostNameLength);
        port_ = parts.nPort; secure_ = parts.nScheme == INTERNET_SCHEME_HTTPS;
    }
    bool valid() const { return !host_.empty(); }
    void set_connection_timeout(int sec) { connectMs_ = sec * 1000; }
    void set_read_timeout(int sec) { readMs_ = sec * 1000; }
    Result Get(const std::string& p, const httplib::Headers& h = {}) { return request(L"GET",p,h,{},"",{}); }
    Result Get(const std::string& p, Receiver cb) { return Get(p,{},cb); }
    Result Get(const std::string& p, const httplib::Headers& h, Receiver cb) { return request(L"GET",p,h,{},"",cb); }
    Result Post(const std::string& p,const std::string& b,const std::string& t) { return Post(p,{},b,t); }
    Result Post(const std::string& p,const httplib::Headers& h,const std::string& b,const std::string& t) { return request(L"POST",p,h,b,t,{}); }
    Result Put(const std::string& p,const httplib::Headers& h,const std::string& b,const std::string& t) { return request(L"PUT",p,h,b,t,{}); }
private:
    static std::wstring wide(const std::string& s) {
        int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
        std::wstring w(n,L'\0'); if(n) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),w.data(),n); return w;
    }
    Result request(const wchar_t* method,const std::string& path,const httplib::Headers& headers,
                   const std::string& body,const std::string& type,Receiver cb) {
        if (!valid() || body.size() > 50ull*1024*1024) return {};
        Handle session(WinHttpOpen(L"DutyOn",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
        if (!session.value) return {};
        WinHttpSetTimeouts(session.value,connectMs_,connectMs_,readMs_,readMs_);
        Handle connection(WinHttpConnect(session.value,host_.c_str(),port_,0));
        if (!connection.value) return {};
        auto target=wide(path);
        Handle req(WinHttpOpenRequest(connection.value,method,target.c_str(),nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,secure_?WINHTTP_FLAG_SECURE:0));
        if (!req.value) return {};
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD disabled=WINHTTP_DISABLE_COOKIES;
        if (!WinHttpSetOption(req.value,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect)) ||
            !WinHttpSetOption(req.value,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof(disabled))) return {};
        std::wstring hs;
        for (const auto& h:headers) hs+=wide(h.first+": "+h.second+"\r\n");
        if (!type.empty()) hs+=wide("Content-Type: "+type+"\r\n");
        if (!WinHttpSendRequest(req.value,hs.c_str(),DWORD(hs.size()),
              body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(body.data()),DWORD(body.size()),DWORD(body.size()),0) ||
            !WinHttpReceiveResponse(req.value,nullptr)) return {};
        DWORD status=0, size=sizeof(status);
        if (!WinHttpQueryHeaders(req.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)) return {};
        auto result=std::make_shared<Response>(); result->status=int(status);
        char buffer[65536]; size_t received=0;
        for (;;) {
            DWORD n=0; if(!WinHttpReadData(req.value,buffer,sizeof(buffer),&n)) return {};
            if(!n) break;
            received+=n;
            if(received>(cb?1024ull*1024*1024:8ull*1024*1024)) return {};
            if(cb) { if(!cb(buffer,n)) return {}; } else result->body.append(buffer,n);
        }
        return result;
    }
    std::wstring host_; INTERNET_PORT port_=0; bool secure_=false;
    int connectMs_=5000, readMs_=30000;
};
} // namespace dutyon
