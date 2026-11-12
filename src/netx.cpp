// Outpost - networking
#include "netx.h"
#include "remote.h"
#include "net.h"
#include "util.h"
#include "proc.h"
#include "json.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <mutex>
#include <regex>

namespace netx {

std::vector<PortRule> ForwardedPorts(const ServerDef& s, const Template& t) {
    std::vector<PortRule> out;
    for (auto& p : t.ports) {
        if (p.local) continue;
        int v = s.Port(p.id);
        if (v <= 0) v = p.def;
        out.push_back({v, p.count, p.protocol});
    }
    return out;
}

std::wstring LocalIp() {
    sock::Init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    std::wstring out;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        sockaddr_in me{};
        int len = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &len) == 0) {
            char buf[64];
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
            out = Utf8ToWide(buf);
        }
    }
    closesocket(s);
    return out;
}

std::wstring Gateway() {
    std::wstring mine = LocalIp();
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size); aa = (IP_ADAPTER_ADDRESSES*)buf.data();
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, aa, &size) != NO_ERROR) return L"";
    }
    for (auto* a = aa; a; a = a->Next) {
        bool match = false;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[64];
            inet_ntop(AF_INET, &((sockaddr_in*)u->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            if (Utf8ToWide(ip) == mine) match = true;
        }
        if (!match) continue;
        for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
            char ip[64];
            inet_ntop(AF_INET, &((sockaddr_in*)g->Address.lpSockaddr)->sin_addr, ip, sizeof(ip));
            return Utf8ToWide(ip);
        }
    }
    return L"";
}

std::wstring PublicIp(std::wstring* error) {
    static std::mutex m;
    static std::wstring cached;
    static ULONGLONG at = 0;
    {
        std::lock_guard<std::mutex> g(m);
        if (!cached.empty() && GetTickCount64() - at < 60000) return cached;
    }
    const wchar_t* urls[] = {L"https://api.ipify.org", L"https://checkip.amazonaws.com", L"https://icanhazip.com"};
    for (auto u : urls) {
        net::Response r;
        if (net::Get(u, r) && r.status == 200) {
            std::wstring ip = Trim(Utf8ToWide(r.body));
            if (!ip.empty() && ip.size() < 46 && ip.find_first_not_of(L"0123456789.:abcdefABCDEF") == std::wstring::npos) {
                std::lock_guard<std::mutex> g(m);
                cached = ip; at = GetTickCount64();
                return ip;
            }
        } else if (error) *error = r.error;
    }
    return L"";
}

bool IsPrivateOrCgnat(const std::wstring& ip) {
    int a = 0, b = 0;
    if (swscanf(ip.c_str(), L"%d.%d", &a, &b) != 2) return false;
    return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128) || a == 127 || (a == 169 && b == 254);
}

// ---------------------------------------------------------------- UPnP
namespace upnp {

static std::mutex s_m;
static std::string s_control, s_service, s_name;
static ULONGLONG s_discoveredAt = 0;
static bool s_found = false;

static std::string Tag(const std::string& xml, const std::string& tag, size_t from = 0, size_t* at = nullptr) {
    size_t a = xml.find("<" + tag + ">", from);
    if (a == std::string::npos) return "";
    size_t b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos) return "";
    if (at) *at = a;
    return xml.substr(a + tag.size() + 2, b - a - tag.size() - 2);
}

static bool Discover() {
    sock::Init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &dst.sin_addr);
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
                             "urn:schemas-upnp-org:service:WANIPConnection:1"};
    for (auto st : targets) {
        std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
        sendto(s, msg.data(), (int)msg.size(), 0, (sockaddr*)&dst, sizeof(dst));
    }
    std::vector<std::string> locations;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 3000) {
        fd_set r; FD_ZERO(&r); FD_SET(s, &r);
        timeval tv{0, 300000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        char buf[2048];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        std::string resp(buf, n), low = resp;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        size_t p = low.find("\nlocation:");
        if (p == std::string::npos) continue;
        size_t e = resp.find("\r\n", p + 1);
        std::string loc = resp.substr(p + 10, e - p - 10);
        while (!loc.empty() && loc[0] == ' ') loc.erase(0, 1);
        if (std::find(locations.begin(), locations.end(), loc) == locations.end()) locations.push_back(loc);
        if (locations.size() >= 3) break;
    }
    closesocket(s);
    for (auto& loc : locations) {
        http::Result d = http::Request("GET", loc, "", {}, 4000);
        if (d.status != 200) continue;
        const char* services[] = {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANIPConnection:1",
                                  "urn:schemas-upnp-org:service:WANPPPConnection:1"};
        for (auto svc : services) {
            size_t at = d.body.find(std::string(">") + svc + "<");
            if (at == std::string::npos) continue;
            std::string ctl = Tag(d.body, "controlURL", at);
            if (ctl.empty()) continue;
            std::string base = Tag(d.body, "URLBase");
            if (base.empty()) { size_t sl = loc.find('/', 7); base = loc.substr(0, sl); }
            if (!base.empty() && base.back() == '/') base.pop_back();
            std::lock_guard<std::mutex> g(s_m);
            s_control = ctl.compare(0, 7, "http://") == 0 ? ctl : base + (ctl[0] == '/' ? "" : "/") + ctl;
            s_service = svc;
            s_name = Tag(d.body, "friendlyName");
            Log(L"UPnP router found: %ls", Utf8ToWide(s_name).c_str());
            return true;
        }
    }
    return false;
}

bool Available(std::wstring* routerName) {
    {
        std::lock_guard<std::mutex> g(s_m);
        if (s_discoveredAt && GetTickCount64() - s_discoveredAt < (s_found ? 30 * 60000ull : 2 * 60000ull)) {
            if (routerName) *routerName = Utf8ToWide(s_name);
            return s_found;
        }
    }
    bool found = Discover();
    std::lock_guard<std::mutex> g(s_m);
    s_found = found;
    s_discoveredAt = GetTickCount64();
    if (routerName) *routerName = Utf8ToWide(s_name);
    return found;
}

static http::Result Soap(const std::string& action, const std::string& args) {
    std::string control, service;
    { std::lock_guard<std::mutex> g(s_m); control = s_control; service = s_service; }
    std::string body = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action + " xmlns:u=\"" + service + "\">" +
                       args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    return http::Request("POST", control, body, {"Content-Type: text/xml; charset=\"utf-8\"", "SOAPAction: \"" + service + "#" + action + "\""}, 5000);
}

std::wstring ExternalIp() {
    if (!Available()) return L"";
    http::Result r = Soap("GetExternalIPAddress", "");
    return r.status == 200 ? Utf8ToWide(Tag(r.body, "NewExternalIPAddress")) : L"";
}

bool Add(int port, const std::wstring& proto, const std::wstring& desc, std::wstring* error) {
    if (!Available()) { if (error) *error = L"No UPnP router found."; return false; }
    std::string ip = WideToUtf8(LocalIp()), d = WideToUtf8(desc);
    for (auto lease : {"0", "86400"}) {
        std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" +
                           WideToUtf8(proto) + "</NewProtocol><NewInternalPort>" + std::to_string(port) + "</NewInternalPort><NewInternalClient>" + ip +
                           "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + d +
                           "</NewPortMappingDescription><NewLeaseDuration>" + lease + "</NewLeaseDuration>";
        http::Result r = Soap("AddPortMapping", args);
        if (r.status == 200) return true;
        std::string code = Tag(r.body, "errorCode"), text = Tag(r.body, "errorDescription");
        if (error) *error = L"The router refused port " + std::to_wstring(port) + L" " + proto + (code.empty() ? L"" : L" (UPnP error " + Utf8ToWide(code) + L": " + Utf8ToWide(text) + L")");
        if (code == "718") return false;   // already mapped to another PC
    }
    return false;
}

bool Remove(int port, const std::wstring& proto) {
    if (!Available()) return false;
    std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>" + WideToUtf8(proto) + "</NewProtocol>";
    return Soap("DeletePortMapping", args).status == 200;
}

static std::vector<std::pair<int, std::wstring>> Expand(const ServerDef& s, const Template& t) {
    std::vector<std::pair<int, std::wstring>> out;
    for (auto& p : ForwardedPorts(s, t))
        for (int i = 0; i < p.count; ++i) {
            if (p.protocol == L"tcp" || p.protocol == L"both") out.push_back({p.port + i, L"TCP"});
            if (p.protocol == L"udp" || p.protocol == L"both") out.push_back({p.port + i, L"UDP"});
        }
    return out;
}

bool MapServer(const ServerDef& s, const Template& t, std::wstring* error) {
    bool all = true;
    for (auto& p : Expand(s, t)) all &= Add(p.first, p.second, L"Outpost " + s.name, error);
    return all;
}
void UnmapServer(const ServerDef& s, const Template& t) {
    for (auto& p : Expand(s, t)) Remove(p.first, p.second);
}

} // namespace upnp

// ---------------------------------------------------------------- Windows Firewall
namespace firewall {

static std::wstring RuleName(const ServerDef& s, const wchar_t* proto) { return L"Outpost - " + s.id + L" - " + proto; }

static std::wstring PortList(const ServerDef& s, const Template& t, bool tcp) {
    std::wstring out;
    for (auto& p : ForwardedPorts(s, t)) {
        bool want = p.protocol == L"both" || p.protocol == (tcp ? L"tcp" : L"udp");
        if (!want) continue;
        if (!out.empty()) out += L",";
        out += p.count > 1 ? std::to_wstring(p.port) + L"-" + std::to_wstring(p.port + p.count - 1) : std::to_wstring(p.port);
    }
    return out;
}

bool HasRules(const ServerDef& s) {
    Template t;
    if (!servers::LoadTemplate(s, t)) return false;
    bool needTcp = !PortList(s, t, true).empty(), needUdp = !PortList(s, t, false).empty();
    auto exists = [&](const wchar_t* proto) {
        std::wstring out;
        int code = proc::RunCapture(L"C:\\Windows\\System32\\netsh.exe", L"advfirewall firewall show rule name=\"" + RuleName(s, proto) + L"\"", L"",
                                    [&](const std::string& l) { out += Utf8ToWide(l) + L"\n"; });
        return code == 0 && out.find(RuleName(s, proto)) != std::wstring::npos;
    };
    return (!needTcp || exists(L"TCP")) && (!needUdp || exists(L"UDP"));
}

int RunElevatedHelper(const std::wstring& args) {
    // args: add|remove <server root>
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(args.c_str(), &argc);
    if (!argv || argc < 2) return 2;
    bool add = std::wstring(argv[0]) == L"add";
    ServerDef s;
    bool ok = servers::Load(argv[1], s);
    LocalFree(argv);
    if (!ok) return 3;
    Template t;
    if (!servers::LoadTemplate(s, t)) return 4;
    const wchar_t* netsh = L"C:\\Windows\\System32\\netsh.exe";
    int failures = 0;
    for (auto proto : {L"TCP", L"UDP"}) {
        proc::RunCapture(netsh, L"advfirewall firewall delete rule name=\"" + RuleName(s, proto) + L"\"", L"", nullptr);
        if (!add) continue;
        std::wstring ports = PortList(s, t, std::wstring(proto) == L"TCP");
        if (ports.empty()) continue;
        std::wstring a = L"advfirewall firewall add rule name=\"" + RuleName(s, proto) + L"\" dir=in action=allow protocol=" + proto +
                         L" localport=" + ports + L" profile=any description=\"Outpost game server: " + s.name + L"\"";
        if (proc::RunCapture(netsh, a, L"", nullptr) != 0) ++failures;
    }
    return failures ? 1 : 0;
}

bool Apply(const ServerDef& s, bool add, std::wstring* error) {
    std::wstring params = L"--firewall " + std::wstring(add ? L"add " : L"remove ") + proc::Quote(s.root);
    std::wstring exe = ExePath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        if (error) *error = GetLastError() == ERROR_CANCELLED ? L"Windows Firewall wasn't changed (permission was declined)." : L"Couldn't ask Windows for permission.";
        return false;
    }
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0 && error) *error = L"Windows Firewall rules couldn't be " + std::wstring(add ? L"added" : L"removed") + L" (code " + std::to_wstring(code) + L").";
    return code == 0;
}

} // namespace firewall

// ---------------------------------------------------------------- reachability
static int CheckMinecraft(const std::wstring& ip, int port, bool bedrock) {
    net::Response r;
    std::wstring url = std::wstring(L"https://api.mcsrvstat.us/") + (bedrock ? L"bedrock/3/" : L"3/") + ip + L":" + std::to_wstring(port);
    if (!net::Get(url, r) || r.status != 200) return -1;
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v)) return -1;
    return v.GetBool(L"online", false) ? 1 : 0;
}

static int CheckTcp(const std::wstring& ip, int port) {
    // portchecker.io connects to the port from the internet (POST /api/v1/query)
    net::Response r;
    std::string body = "{\"host\":\"" + WideToUtf8(ip) + "\",\"ports\":[" + std::to_string(port) + "]}";
    if (!net::Send(L"POST", L"https://portchecker.io/api/v1/query", body, L"Content-Type: application/json\r\n", r) || r.status != 200) return -1;
    json::Value v;
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v)) return -1;
    if (const json::Value* c = v.Get(L"check"); c && c->type == json::Value::Array)
        for (auto& e : c->arr) if ((int)e.GetNum(L"port", 0) == port) return e.GetBool(L"status", false) ? 1 : 0;
    return -1;
}

Report Diagnose(const ServerDef& s, const Template& t, bool running) {
    Report r;
    r.localIp = LocalIp();
    r.gateway = Gateway();
    r.publicIp = PublicIp(&r.error);
    r.upnp = upnp::Available(&r.routerName);
    if (r.upnp) r.routerIp = upnp::ExternalIp();
    r.cgnat = !r.routerIp.empty() && IsPrivateOrCgnat(r.routerIp);
    if (r.publicIp.empty()) {
        r.nat = L"Offline";
        r.advice = L"Outpost couldn't reach the internet. Friends on your home network can still join using the local address.";
    } else if (r.cgnat || (!r.routerIp.empty() && r.routerIp != r.publicIp)) {
        r.nat = L"Double NAT";
        r.advice = L"Your router is behind another router or your internet provider's shared address (CGNAT), so port forwarding can't reach this PC. "
                   L"Ask your provider for a public IP, put the first router in bridge mode, or use a tunnel like playit.gg, Tailscale or ZeroTier.";
    } else if (r.upnp) {
        r.nat = L"Open (UPnP)";
        r.advice = L"Your router accepts automatic port forwarding. Outpost forwards the server's ports while it runs.";
    } else {
        r.nat = L"Manual forwarding";
        r.advice = L"Your router doesn't allow automatic forwarding (UPnP is off or unsupported). Forward the ports below by hand once.";
    }
    std::wstring err;
    bool fw = firewall::HasRules(s);
    for (auto& p : ForwardedPorts(s, t)) {
        std::vector<std::wstring> protos;
        if (p.protocol == L"tcp" || p.protocol == L"both") protos.push_back(L"TCP");
        if (p.protocol == L"udp" || p.protocol == L"both") protos.push_back(L"UDP");
        for (auto& pr : protos) {
            PortCheck c;
            c.port = p.port; c.protocol = pr;
            c.firewall = fw ? 1 : 0;
            if (running && !r.publicIp.empty() && !r.cgnat) {
                if (t.id == L"minecraft-java" && pr == L"TCP") c.reachable = CheckMinecraft(r.publicIp, p.port, false);
                else if (t.id == L"minecraft-bedrock" && pr == L"UDP" && p.port == s.Port()) c.reachable = CheckMinecraft(r.publicIp, p.port, true);
                else if (pr == L"TCP") c.reachable = CheckTcp(r.publicIp, p.port);
            }
            if (r.cgnat) c.reachable = 0;
            r.ports.push_back(c);
        }
    }
    return r;
}

bool UpdateDuckDns(const std::wstring& domain, const std::wstring& token, std::wstring* error) {
    std::wstring d = domain;
    size_t dot = d.find(L".duckdns.org");
    if (dot != std::wstring::npos) d = d.substr(0, dot);
    net::Response r;
    if (!net::Get(L"https://www.duckdns.org/update?domains=" + d + L"&token=" + token + L"&ip=", r) || r.status != 200) {
        if (error) *error = L"Couldn't reach DuckDNS.";
        return false;
    }
    bool ok = Trim(Utf8ToWide(r.body)) == L"OK";
    if (!ok && error) *error = L"DuckDNS refused the update (check the domain and token).";
    return ok;
}

} // namespace netx
