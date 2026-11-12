// Outpost - networking: addresses, UPnP port forwarding (SSDP + SOAP), Windows Firewall rules,
// NAT and reachability checks, and DuckDNS for a friendly address.
#pragma once
#include "server.h"

namespace netx {

struct PortRule { int port = 0, count = 1; std::wstring protocol; };   // protocol: tcp | udp | both
std::vector<PortRule> ForwardedPorts(const ServerDef& s, const Template& t);   // non-local ports with values

std::wstring LocalIp();
std::wstring Gateway();
std::wstring PublicIp(std::wstring* error = nullptr);        // via ipify (cached for a minute)
bool IsPrivateOrCgnat(const std::wstring& ip);

namespace upnp {
bool Available(std::wstring* routerName = nullptr);          // discovers the router (cached)
std::wstring ExternalIp();
bool Add(int port, const std::wstring& proto, const std::wstring& desc, std::wstring* error);   // proto: TCP | UDP
bool Remove(int port, const std::wstring& proto);
bool MapServer(const ServerDef& s, const Template& t, std::wstring* error);
void UnmapServer(const ServerDef& s, const Template& t);
}

namespace firewall {
bool HasRules(const ServerDef& s);
// Adds or removes the server's inbound rules. Asks for administrator permission (one prompt).
bool Apply(const ServerDef& s, bool add, std::wstring* error);
int RunElevatedHelper(const std::wstring& args);             // inside the elevated copy: --firewall add|remove <root>
}

struct PortCheck { int port = 0; std::wstring protocol; int upnp = -1, firewall = -1, reachable = -1; };   // 1 yes, 0 no, -1 unknown
struct Report {
    std::wstring localIp, gateway, publicIp, routerIp, routerName, nat, advice;
    bool upnp = false, cgnat = false;
    std::vector<PortCheck> ports;
    std::wstring error;
};
Report Diagnose(const ServerDef& s, const Template& t, bool running);

bool UpdateDuckDns(const std::wstring& domain, const std::wstring& token, std::wstring* error);
}
