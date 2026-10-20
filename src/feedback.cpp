// Feedback from the General page: a feature idea or a problem report, posted as JSON to the
// VideoBG-feedback server (a Vercel function that files it as an issue in a private GitHub repo; no
// secret is kept in the app). Nothing personal is asked for, and the log, when it's included, has
// user names and media file names taken out first.
#include "common.h"
#include <winhttp.h>
#include <wincodec.h>
#include <algorithm>

namespace {

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring o(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n);
    return o;
}

void Json(std::string& out, const char* key, const std::wstring& v) {
    out += out.size() > 1 ? ",\"" : "\"";
    out += key;
    out += "\":\"";
    for (unsigned char c : Utf8(v)) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

size_t FindNoCase(const std::wstring& s, const wchar_t* what, size_t from) {
    const size_t n = wcslen(what);
    for (size_t i = from; i + n <= s.size(); i++)
        if (!_wcsnicmp(s.c_str() + i, what, n)) return i;
    return std::wstring::npos;
}

std::string ReadTail(const std::wstring& path, size_t max) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    const LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - (LONGLONG)max);
    LARGE_INTEGER at{};
    at.QuadPart = start;
    SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
    std::string s((size_t)(size.QuadPart - start), '\0');
    DWORD got = 0;
    if (!s.empty()) ReadFile(f, s.data(), (DWORD)s.size(), &got, nullptr);
    CloseHandle(f);
    s.resize(got);
    return s;
}

std::string Base64(const std::string& in) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned v = (unsigned char)in[i] << 16 | (i + 1 < in.size() ? (unsigned char)in[i + 1] << 8 : 0) | (i + 2 < in.size() ? (unsigned char)in[i + 2] : 0);
        o += k[v >> 18 & 63];
        o += k[v >> 12 & 63];
        o += i + 1 < in.size() ? k[v >> 6 & 63] : '=';
        o += i + 2 < in.size() ? k[v & 63] : '=';
    }
    return o;
}

}  // namespace

bool Feedback_Picture(const std::wstring& file, HBITMAP bitmap, FeedbackPicture* out) {
    IWICImagingFactory* wic = nullptr;
    IWICBitmapSource* src = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmap* bmp = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fe = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr) && bitmap) {
        hr = wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &bmp);
        src = bmp;
    } else if (SUCCEEDED(hr)) {
        hr = wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
        if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
        src = frame;
    }
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = src->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0)) hr = E_FAIL;
    const double k = SUCCEEDED(hr) ? std::min(1.0, 1600.0 / std::max(w, h)) : 1;  // (screenshots stay readable)
    const UINT sw = std::max(1u, (UINT)(w * k + 0.5)), sh = std::max(1u, (UINT)(h * k + 0.5));
    if (SUCCEEDED(hr)) hr = wic->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(hr)) hr = scaler->Initialize(src, sw, sh, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(scaler, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fe, &props);
    if (SUCCEEDED(hr) && props) {
        PROPBAG2 opt{};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.82f;
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = fe->Initialize(props);
    if (SUCCEEDED(hr)) hr = fe->SetSize(sw, sh);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = fe->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr)) hr = fe->WriteSource(conv, nullptr);
    if (SUCCEEDED(hr)) hr = fe->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (SUCCEEDED(hr)) {
        HGLOBAL g = nullptr;
        hr = GetHGlobalFromStream(stream, &g);
        STATSTG st{};
        if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
        const void* p = SUCCEEDED(hr) ? GlobalLock(g) : nullptr;
        if (p) {
            out->jpeg.assign((const char*)p, (size_t)st.cbSize.QuadPart);
            GlobalUnlock(g);
        } else {
            hr = E_FAIL;
        }
    }
    for (IUnknown* u : {(IUnknown*)props, (IUnknown*)fe, (IUnknown*)enc, (IUnknown*)stream, (IUnknown*)conv, (IUnknown*)scaler, (IUnknown*)bmp,
                        (IUnknown*)frame, (IUnknown*)dec, (IUnknown*)wic})
        if (u) u->Release();
    if (FAILED(hr)) return false;
    const size_t slash = file.find_last_of(L"\\/");
    out->name = file.empty() ? L"Pasted picture" : slash == std::wstring::npos ? file : file.substr(slash + 1);
    return true;
}

// User names in paths (...\Users\<name>\...) and the names of videos, songs and fonts.
std::wstring Feedback_Redact(std::wstring s) {
    for (size_t at = 0; (at = FindNoCase(s, L"\\Users\\", at)) != std::wstring::npos;) {
        const size_t name = at + 7;
        size_t end = s.find_first_of(L"\\/\r\n\"", name);
        if (end == std::wstring::npos) end = s.size();
        s.replace(name, end - name, L"<you>");
        at = name + 5;
    }
    static const wchar_t* exts[] = {L"mp4", L"m4v", L"mkv", L"webm", L"mov", L"avi", L"wmv", L"mpg", L"mpeg", L"ts", L"m2ts", L"flv",
                                    L"3gp", L"gif", L"mp3", L"m4a", L"aac", L"wav", L"flac", L"ogg", L"opus", L"wma", L"m3u8", L"m3u",
                                    L"ttf", L"otf", L"zip", L"jpg", L"jpeg", L"png", L"bmp"};
    for (size_t dot = s.find(L'.'); dot != std::wstring::npos; dot = s.find(L'.', dot + 1)) {
        size_t len = 0;
        for (const wchar_t* e : exts) {
            const size_t n = wcslen(e);
            if (!_wcsnicmp(s.c_str() + dot + 1, e, n) && (dot + 1 + n == s.size() || !iswalnum(s[dot + 1 + n]))) len = std::max(len, n);
        }
        if (!len) continue;
        const size_t lineStart = s.rfind(L'\n', dot) == std::wstring::npos ? 0 : s.rfind(L'\n', dot) + 1;
        const size_t slash = s.find_last_of(L"\\/", dot);
        if (slash == std::wstring::npos || slash < lineStart || slash + 1 >= dot) continue;  // not a path
        if (s.compare(slash + 1, dot - slash - 1, L"<file>") == 0) continue;
        s.replace(slash + 1, dot - slash - 1, L"<file>");
        dot = slash + 1 + 6;
    }
    return s;
}

std::wstring Feedback_Windows() {
    RTL_OSVERSIONINFOW v{sizeof v};
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    if (auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) fn(&v);
    wchar_t display[32] = {};
    DWORD size = sizeof display, ubr = 0, ubrSize = sizeof ubr;
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", RRF_RT_REG_SZ, nullptr, display, &size);
    RegGetValueW(HKEY_LOCAL_MACHINE, key, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &ubrSize);
    wchar_t b[96];
    swprintf(b, 96, L"Windows %d%ls%ls (build %lu.%lu)", v.dwBuildNumber >= 22000 ? 11 : 10, display[0] ? L" " : L"", display,
             v.dwBuildNumber, ubr);
    return b;
}

std::wstring Feedback_Log() {
    const size_t max = 48 * 1024;  // the newest events: enough to see what led up to a problem
    std::string s = ReadTail(LogPath(false), max);
    if (s.size() < max) s = ReadTail(LogPath(true), max - s.size()) + s;
    if (const size_t nl = s.find('\n'); nl != std::string::npos && nl + 1 < s.size()) s.erase(0, nl + 1);  // whole lines only
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());  // lines end in \n, like the other fields
    return Feedback_Redact(Wide(s));
}

std::wstring Feedback_Preview(const FeedbackReport& r) {
    std::wstring t = r.bug ? L"Problem report" : L"Feature idea";
    t += L" from VideoBG " APP_VERSION L"\r\n\r\n";
    t += (r.bug ? L"What went wrong:\r\n" : L"Idea:\r\n") + r.text + L"\r\n";
    if (!r.steps.empty()) t += L"\r\nWhat you were doing:\r\n" + r.steps + L"\r\n";
    if (!r.often.empty()) t += L"\r\nHow often: " + r.often + L"\r\n";
    if (!r.email.empty()) t += L"\r\nReply to: " + r.email + L"\r\n";
    if (!r.system.empty()) t += L"\r\nYour PC and settings:\r\n" + r.system + L"\r\n";
    if (!r.pictures.empty()) {
        size_t bytes = 0;
        for (auto& p : r.pictures) bytes += p.jpeg.size();
        wchar_t b[128];
        swprintf(b, 128, L"\r\nPictures: %zu (as JPEGs, %.0f KB in all; their file names aren't sent)\r\n", r.pictures.size(), bytes / 1024.0);
        t += b;
    }
    if (!r.log.empty()) {
        std::wstring log;
        for (wchar_t c : r.log) {
            if (c == L'\n') log += L'\r';
            log += c;
        }
        t += L"\r\nLog (newest events; user and file names replaced):\r\n" + log;
    }
    return t;
}

bool Feedback_Send(const FeedbackReport& r, std::wstring* error) {
    std::string body = "{";
    Json(body, "kind", r.bug ? L"bug" : L"feature");
    Json(body, "version", APP_VERSION);
    Json(body, "text", r.text);
    if (r.bug) {
        Json(body, "steps", r.steps);
        Json(body, "often", r.often);
        Json(body, "email", r.email);
        Json(body, "system", r.system);
        Json(body, "log", r.log);
    }
    if (!r.pictures.empty()) {  // "pictures":["<base64 JPEG>", ...] (their file names aren't sent)
        body += ",\"pictures\":[";
        for (size_t i = 0; i < r.pictures.size(); i++) body += (i ? ",\"" : "\"") + Base64(r.pictures[i].jpeg) + "\"";
        body += "]";
    }
    body += "}";

    const std::wstring url = FeedbackUrl();
    wchar_t host[256] = {}, path[1024] = {};
    URL_COMPONENTS uc{sizeof uc};
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)std::size(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)std::size(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        *error = L"the feedback address is wrong";
        return false;
    }
    HINTERNET ses = WinHttpOpen(L"VideoBG/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"VideoBG/" APP_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = ses ? WinHttpConnect(ses, host, uc.nPort, 0) : nullptr;
    HINTERNET req = con ? WinHttpOpenRequest(con, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                        : nullptr;
    if (ses) WinHttpSetTimeouts(ses, 10000, 10000, 20000, 20000);
    bool sent = req && WinHttpSendRequest(req, L"Content-Type: application/json\r\n", (DWORD)-1L, body.data(), (DWORD)body.size(),
                                          (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(req, nullptr);
    const DWORD err = sent ? 0 : GetLastError();
    DWORD status = 0, size = sizeof status;
    std::string reply;
    if (sent) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        char buf[1024];
        DWORD got = 0;
        while (reply.size() < 4096 && WinHttpReadData(req, buf, sizeof buf, &got) && got) reply.append(buf, got);
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);

    if (!sent) {
        *error = err == ERROR_WINHTTP_NAME_NOT_RESOLVED || err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_TIMEOUT
                     ? L"no connection to the internet?"
                     : L"the connection failed (" + std::to_wstring(err) + L")";
        Log(L"feedback: not sent, error %lu", err);
        return false;
    }
    if (status == 200) {
        Log(L"feedback: %ls sent", r.bug ? L"problem report" : L"feature idea");
        return true;
    }
    // The server says why: {"ok":false,"error":"..."}
    const size_t at = reply.find("\"error\":\"");
    const size_t end = at == std::string::npos ? at : reply.find('"', at + 9);
    *error = end != std::string::npos ? Wide(reply.substr(at + 9, end - at - 9)) : L"the feedback server didn't take it (" + std::to_wstring(status) + L")";
    Log(L"feedback: not sent, status %lu", status);
    return false;
}
