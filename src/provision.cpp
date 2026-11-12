// Outpost - getting a game's server files
#include "provision.h"
#include "net.h"
#include "util.h"
#include "fsutil.h"
#include "archive.h"
#include "proc.h"
#include "json.h"
#include <bcrypt.h>
#include <algorithm>
#include <regex>
#include <shellapi.h>

namespace provision {

static bool GetJson(const std::wstring& url, json::Value& v, std::wstring* error) {
    net::Response r;
    if (!net::Get(url, r)) { if (error) *error = L"Couldn't reach " + url + L" (" + r.error + L")"; return false; }
    if (r.status != 200) { if (error) *error = url + L" answered HTTP " + std::to_wstring(r.status); return false; }
    if (!json::Parser(Utf8ToWide(r.body)).Parse(v)) { if (error) *error = url + L" sent something that isn't JSON."; return false; }
    return true;
}

std::wstring HashFile(const std::wstring& path, const wchar_t* alg) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring out;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, 0) != 0) return out;
    DWORD len = 0, cb = 0;
    BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof(len), &cb, 0);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f != INVALID_HANDLE_VALUE && BCryptCreateHash(a, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<uint8_t> buf(1 << 20);
        DWORD n;
        while (ReadFile(f, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) BCryptHashData(h, buf.data(), n, 0);
        std::vector<uint8_t> d(len);
        if (BCryptFinishHash(h, d.data(), len, 0) == 0) {
            wchar_t hx[3];
            for (uint8_t b : d) { swprintf(hx, 3, L"%02x", b); out += hx; }
        }
        BCryptDestroyHash(h);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    BCryptCloseAlgorithmProvider(a, 0);
    return out;
}

// Download with progress, then check a hash when we have one.
static bool Fetch(const std::wstring& url, const std::wstring& path, const std::wstring& what, const std::wstring& expect,
                  const wchar_t* alg, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    if (!expect.empty() && FileExists(path) && HashFile(path, alg) == Lower(expect)) return true;
    EnsureDir(PathParent(path));
    int last = -2;
    bool ok = net::Download(url, path, [&](uint64_t done, uint64_t total) {
        int pct = total ? (int)(done * 100 / total) : -1;
        if (pct != last && progress) { last = pct; progress(pct, L"Downloading " + what + (total ? L" (" + FormatBytes(total) + L")" : L"")); }
        return !(cancel && *cancel);
    }, error);
    if (!ok) { if (cancel && *cancel) *error = L"Cancelled."; return false; }
    if (!expect.empty() && HashFile(path, alg) != Lower(expect)) {
        DeleteFileW(path.c_str());
        *error = what + L" didn't match its checksum, so it wasn't used. Try again.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Java
static std::wstring JavaDir(int major) { return DataDir() + L"\\runtime\\java-" + std::to_wstring(major); }

bool EnsureJava(int major, const Progress& progress, std::atomic<bool>* cancel, std::wstring* javaExe, std::wstring* error) {
    std::wstring dir = JavaDir(major), exe = dir + L"\\bin\\java.exe";
    if (FileExists(exe)) { *javaExe = exe; return true; }
    if (progress) progress(-1, L"Finding Java " + std::to_wstring(major));
    json::Value v;
    std::wstring url = L"https://api.adoptium.net/v3/assets/latest/" + std::to_wstring(major) + L"/hotspot?os=windows&architecture=x64&image_type=jre&vendor=eclipse";
    if (!GetJson(url, v, error)) return false;
    if (v.type != json::Value::Array || v.arr.empty()) {
        // some versions only ship a JDK
        url = L"https://api.adoptium.net/v3/assets/latest/" + std::to_wstring(major) + L"/hotspot?os=windows&architecture=x64&image_type=jdk&vendor=eclipse";
        if (!GetJson(url, v, error) || v.type != json::Value::Array || v.arr.empty()) { *error = L"No Java " + std::to_wstring(major) + L" download was found."; return false; }
    }
    const json::Value* pkg = nullptr;
    if (const json::Value* b = v.arr[0].Get(L"binary")) pkg = b->Get(L"package");
    if (!pkg) { *error = L"Adoptium sent an unexpected reply."; return false; }
    std::wstring zip = DownloadDir() + L"\\" + pkg->GetStr(L"name");
    if (!Fetch(pkg->GetStr(L"link"), zip, L"Java " + std::to_wstring(major), pkg->GetStr(L"checksum"), BCRYPT_SHA256_ALGORITHM, progress, cancel, error)) return false;
    if (progress) progress(-1, L"Unpacking Java " + std::to_wstring(major));
    auto names = archive::List(zip);
    std::wstring top = names.empty() ? L"" : names[0].substr(0, names[0].find(L'/'));
    std::wstring tmp = dir + L".part";
    fs::DeleteTree(tmp);
    if (!archive::Extract(zip, tmp, top, nullptr, error)) return false;
    fs::DeleteTree(dir);
    if (!MoveFileExW(tmp.c_str(), dir.c_str(), 0)) { *error = L"Couldn't put Java in place."; return false; }
    DeleteFileW(zip.c_str());
    if (!FileExists(exe)) { *error = L"The Java download didn't contain java.exe."; return false; }
    *javaExe = exe;
    Log(L"Java %d installed", major);
    return true;
}

// ---------------------------------------------------------------- Minecraft
static const wchar_t* kMojangManifest = L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json";

static bool MojangVersion(const std::wstring& version, json::Value& out, std::wstring* error) {
    json::Value m;
    if (!GetJson(kMojangManifest, m, error)) return false;
    std::wstring want = version;
    if (want == L"latest") if (const json::Value* l = m.Get(L"latest")) want = l->GetStr(L"release");
    if (const json::Value* vs = m.Get(L"versions"); vs && vs->type == json::Value::Array)
        for (auto& e : vs->arr)
            if (e.GetStr(L"id") == want) return GetJson(e.GetStr(L"url"), out, error);
    *error = L"Minecraft " + want + L" wasn't found.";
    return false;
}

static int JavaFor(const std::wstring& mcVersion) {
    json::Value v;
    std::wstring err;
    if (MojangVersion(mcVersion, v, &err))
        if (const json::Value* j = v.Get(L"javaVersion")) return (int)j->GetNum(L"majorVersion", 21);
    // fallback when Mojang's metadata isn't reachable
    if (CompareVersions(mcVersion, L"1.20.5") >= 0) return 21;
    if (CompareVersions(mcVersion, L"1.18") >= 0) return 17;
    if (CompareVersions(mcVersion, L"1.17") >= 0) return 16;
    return 8;
}

// "21.1.77" -> "1.21.1", "20.4.237" -> "1.20.4", "21.0.5" -> "1.21", "26.1.0.3" -> "26.1"
static std::wstring NeoForgeMc(const std::wstring& nf) {
    int a = 0, b = 0;
    if (swscanf(nf.c_str(), L"%d.%d", &a, &b) < 2) return L"";
    if (a >= 25) return std::to_wstring(a) + L"." + std::to_wstring(b);
    return b == 0 ? L"1." + std::to_wstring(a) : L"1." + std::to_wstring(a) + L"." + std::to_wstring(b);
}

static void SortDesc(std::vector<std::wstring>& v) {
    std::stable_sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) { return CompareVersions(a, b) > 0; });
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

static bool IsRelease(const std::wstring& v) {
    for (wchar_t c : v) if (!iswdigit(c) && c != L'.') return false;
    return !v.empty();
}

Versions ListVersions(const Template& t, const std::wstring& variant) {
    Versions out;
    if (t.sourceType == L"minecraft") {
        json::Value v;
        if (variant == L"paper") {
            if (GetJson(L"https://fill.papermc.io/v3/projects/paper", v, &out.error))
                if (const json::Value* groups = v.Get(L"versions"); groups && groups->type == json::Value::Object)
                    for (auto& g : groups->obj) for (auto& x : g.second.arr) if (IsRelease(x.s)) out.list.push_back(x.s);
        } else if (variant == L"fabric") {
            if (GetJson(L"https://meta.fabricmc.net/v2/versions/game", v, &out.error) && v.type == json::Value::Array)
                for (auto& x : v.arr) if (x.GetBool(L"stable", false)) out.list.push_back(x.GetStr(L"version"));
        } else if (variant == L"forge") {
            if (GetJson(L"https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json", v, &out.error))
                if (const json::Value* p = v.Get(L"promos"))
                    for (auto& kv : p->obj) { size_t d = kv.first.find(L'-'); if (d != std::wstring::npos) out.list.push_back(kv.first.substr(0, d)); }
        } else if (variant == L"neoforge") {
            if (GetJson(L"https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge", v, &out.error))
                if (const json::Value* vs = v.Get(L"versions"))
                    for (auto& x : vs->arr) if (x.s.find(L"beta") == std::wstring::npos) { std::wstring mc = NeoForgeMc(x.s); if (!mc.empty()) out.list.push_back(mc); }
        } else {
            if (GetJson(kMojangManifest, v, &out.error))
                if (const json::Value* vs = v.Get(L"versions"))
                    for (auto& x : vs->arr) if (x.GetStr(L"type") == L"release") out.list.push_back(x.GetStr(L"id"));
        }
        SortDesc(out.list);
        if (!out.list.empty()) { out.latest = out.list[0]; out.error.clear(); }
    } else if (t.sourceType == L"terraria") {
        json::Value v;
        if (GetJson(L"https://terraria.org/api/get/dedicated-servers-names", v, &out.error) && v.type == json::Value::Array)
            for (auto& x : v.arr) {
                std::wstring n = x.s;   // terraria-server-1449.zip
                size_t a = n.rfind(L'-'), b = n.rfind(L'.');
                if (a == std::wstring::npos || b == std::wstring::npos || b <= a) continue;
                std::wstring num = n.substr(a + 1, b - a - 1), pretty;
                for (size_t i = 0; i < num.size(); ++i) { if (i) pretty += L'.'; pretty += num[i]; }
                out.list.push_back(pretty);
            }
        SortDesc(out.list);
        if (!out.list.empty()) { out.latest = out.list[0]; out.error.clear(); }
    }
    return out;
}

// After Forge/NeoForge's installer: the JVM args file from run.bat (modern) or the server jar (old)
static std::wstring ForgeLaunchArgs(const std::wstring& dir) {
    std::wstring bat;
    if (ReadTextFile(dir + L"\\run.bat", bat)) {
        std::wsmatch m;
        std::wregex re(L"@(libraries[/\\\\][^\\s]+win_args\\.txt)");
        if (std::regex_search(bat, m, re)) return L"@user_jvm_args.txt @" + m[1].str();
    }
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.jar").c_str(), &fd);
    std::wstring best;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = Lower(fd.cFileName);
            if (n.find(L"installer") != std::wstring::npos) continue;
            if (n.find(L"forge") != std::wstring::npos || n.find(L"neoforge") != std::wstring::npos) best = fd.cFileName;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return best.empty() ? L"" : L"-jar " + proc::Quote(best);
}

static bool InstallMinecraft(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    std::wstring dir = s.FilesDir();
    EnsureDir(dir);
    std::wstring variant = s.variant.empty() ? L"vanilla" : s.variant;
    std::wstring mc = s.version;
    if (mc.empty() || mc == L"latest") {
        if (progress) progress(-1, L"Checking the newest version");
        Versions v = ListVersions(t, variant);
        if (v.latest.empty()) { *error = v.error.empty() ? L"Couldn't find the newest version." : v.error; return false; }
        mc = v.latest;
    }
    int javaMajor = JavaFor(mc);
    std::wstring java;
    if (!EnsureJava(javaMajor, progress, cancel, &java, error)) return false;
    s.vars[L"java"] = java;
    std::wstring installed = mc;

    if (variant == L"vanilla") {
        json::Value v;
        if (!MojangVersion(mc, v, error)) return false;
        const json::Value* d = v.Get(L"downloads");
        const json::Value* srv = d ? d->Get(L"server") : nullptr;
        if (!srv) { *error = L"Minecraft " + mc + L" has no server download."; return false; }
        if (!Fetch(srv->GetStr(L"url"), dir + L"\\server.jar", L"Minecraft " + mc, srv->GetStr(L"sha1"), BCRYPT_SHA1_ALGORITHM, progress, cancel, error)) return false;
        s.vars[L"launchArgs"] = L"-jar server.jar";
    } else if (variant == L"paper") {
        json::Value b;
        if (!GetJson(L"https://fill.papermc.io/v3/projects/paper/versions/" + mc + L"/builds/latest", b, error)) return false;
        const json::Value* d = b.Get(L"downloads");
        const json::Value* srv = d ? d->Get(L"server:default") : nullptr;
        if (!srv) { *error = L"Paper has no build for " + mc + L" yet."; return false; }
        std::wstring sha;
        if (const json::Value* c = srv->Get(L"checksums")) sha = c->GetStr(L"sha256");
        if (!Fetch(srv->GetStr(L"url"), dir + L"\\paper.jar", L"Paper " + mc, sha, BCRYPT_SHA256_ALGORITHM, progress, cancel, error)) return false;
        s.vars[L"launchArgs"] = L"-jar paper.jar";
        wchar_t build[32]; swprintf(build, 32, L"%g", b.GetNum(L"id", 0));
        installed = mc + L" (build " + build + L")";
    } else if (variant == L"fabric") {
        json::Value loaders, installers;
        if (!GetJson(L"https://meta.fabricmc.net/v2/versions/loader", loaders, error) || !GetJson(L"https://meta.fabricmc.net/v2/versions/installer", installers, error)) return false;
        std::wstring loader, inst;
        for (auto& x : loaders.arr) if (x.GetBool(L"stable", false)) { loader = x.GetStr(L"version"); break; }
        for (auto& x : installers.arr) if (x.GetBool(L"stable", false)) { inst = x.GetStr(L"version"); break; }
        if (loader.empty() || inst.empty()) { *error = L"Couldn't find Fabric's loader."; return false; }
        std::wstring url = L"https://meta.fabricmc.net/v2/versions/loader/" + mc + L"/" + loader + L"/" + inst + L"/server/jar";
        if (!Fetch(url, dir + L"\\fabric-server-launch.jar", L"Fabric " + loader, L"", nullptr, progress, cancel, error)) return false;
        s.vars[L"launchArgs"] = L"-jar fabric-server-launch.jar";
        installed = mc + L" (Fabric " + loader + L")";
    } else if (variant == L"forge" || variant == L"neoforge") {
        std::wstring url, ver;
        json::Value v;
        if (variant == L"forge") {
            if (!GetJson(L"https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json", v, error)) return false;
            const json::Value* p = v.Get(L"promos");
            ver = p ? p->GetStr(mc + L"-recommended", p->GetStr(mc + L"-latest")) : L"";
            if (ver.empty()) { *error = L"Forge has no build for Minecraft " + mc + L"."; return false; }
            std::wstring full = mc + L"-" + ver;
            url = L"https://maven.minecraftforge.net/net/minecraftforge/forge/" + full + L"/forge-" + full + L"-installer.jar";
        } else {
            if (!GetJson(L"https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge", v, error)) return false;
            if (const json::Value* vs = v.Get(L"versions"))
                for (auto& x : vs->arr) if (NeoForgeMc(x.s) == mc && x.s.find(L"beta") == std::wstring::npos && (ver.empty() || CompareVersions(x.s, ver) > 0)) ver = x.s;
            if (ver.empty()) { *error = L"NeoForge has no build for Minecraft " + mc + L"."; return false; }
            url = L"https://maven.neoforged.net/releases/net/neoforged/neoforge/" + ver + L"/neoforge-" + ver + L"-installer.jar";
        }
        std::wstring inst = DownloadDir() + L"\\" + variant + L"-" + ver + L"-installer.jar";
        std::wstring sha1;
        net::Response r;
        if (net::Get(url + L".sha1", r) && r.status == 200) sha1 = Trim(Utf8ToWide(r.body)).substr(0, 40);
        if (!Fetch(url, inst, (variant == L"forge" ? L"Forge " : L"NeoForge ") + ver, sha1, BCRYPT_SHA1_ALGORITHM, progress, cancel, error)) return false;
        if (progress) progress(-1, L"Running the " + std::wstring(variant == L"forge" ? L"Forge" : L"NeoForge") + L" installer");
        int code = proc::RunCapture(java, L"-jar " + proc::Quote(inst) + L" --installServer", dir,
                                    [&](const std::string& line) { if (progress) progress(-1, Utf8ToWide(line).substr(0, 120)); }, cancel);
        if (code != 0) { *error = L"The installer failed (exit code " + std::to_wstring(code) + L"). See outpost.log."; return false; }
        std::wstring args = ForgeLaunchArgs(dir);
        if (args.empty()) { *error = L"The installer finished, but Outpost couldn't find how to start the server."; return false; }
        s.vars[L"launchArgs"] = args;
        installed = mc + L" (" + (variant == L"forge" ? L"Forge " : L"NeoForge ") + ver + L")";
    }
    s.vars[L"mcVersion"] = mc;
    s.installedVersion = installed;
    return true;
}

// ---------------------------------------------------------------- Bedrock
static const wchar_t* kBedrockKeep[] = {L"server.properties", L"allowlist.json", L"permissions.json", L"whitelist.json", L"worlds"};

static bool InstallBedrock(ServerDef& s, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    if (progress) progress(-1, L"Checking the newest Bedrock server");
    std::wstring url;
    json::Value v;
    std::wstring err;
    if (GetJson(L"https://net-secondary.web.minecraft-services.net/api/v1.0/download/links", v, &err))
        if (const json::Value* res = v.Get(L"result"))
            if (const json::Value* links = res->Get(L"links"); links && links->type == json::Value::Array)
                for (auto& l : links->arr) if (l.GetStr(L"downloadType") == L"serverBedrockWindows") url = l.GetStr(L"downloadUrl");
    if (url.empty()) {
        net::Response r;
        if (net::Get(L"https://www.minecraft.net/en-us/download/server/bedrock", r) && r.status == 200) {
            std::smatch m;
            std::regex re("https://[^\"']+/bin-win/bedrock-server-[0-9.]+\\.zip");
            if (std::regex_search(r.body, m, re)) url = Utf8ToWide(m[0].str());
        }
    }
    if (url.empty()) { *error = L"Couldn't find the Bedrock server download. " + err; return false; }
    std::wstring ver = PathName(url);
    size_t a = ver.find(L"server-"), b = ver.rfind(L".zip");
    if (a != std::wstring::npos && b != std::wstring::npos) ver = ver.substr(a + 7, b - a - 7);
    std::wstring zip = DownloadDir() + L"\\bedrock-" + ver + L".zip";
    if (!Fetch(url, zip, L"Bedrock server " + ver, L"", nullptr, progress, cancel, error)) return false;
    if (progress) progress(-1, L"Unpacking");
    std::wstring dir = s.FilesDir(), tmp = s.root + L"\\update.part";
    fs::DeleteTree(tmp);
    if (!archive::Extract(zip, tmp, L"", nullptr, error)) return false;
    // keep the world and your settings when updating
    bool update = FileExists(dir + L"\\bedrock_server.exe");
    if (update) for (auto k : kBedrockKeep) fs::DeleteTree(tmp + L"\\" + k);
    if (!fs::CopyTree(tmp, dir)) { *error = L"Couldn't copy the Bedrock server files (is it still running?)."; return false; }
    fs::DeleteTree(tmp);
    DeleteFileW(zip.c_str());
    s.installedVersion = ver;
    return true;
}

// ---------------------------------------------------------------- Terraria
static bool InstallTerraria(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    std::wstring ver = s.version;
    if (ver.empty() || ver == L"latest") {
        Versions v = ListVersions(t, L"");
        if (v.latest.empty()) { *error = v.error.empty() ? L"Couldn't find Terraria's server download." : v.error; return false; }
        ver = v.latest;
    }
    std::wstring num;
    for (wchar_t c : ver) if (iswdigit(c)) num += c;
    std::wstring zip = DownloadDir() + L"\\terraria-server-" + num + L".zip";
    if (!Fetch(L"https://terraria.org/api/download/pc-dedicated-server/terraria-server-" + num + L".zip", zip, L"Terraria " + ver, L"", nullptr, progress, cancel, error)) return false;
    std::wstring prefix;
    for (auto& n : archive::List(zip)) {
        size_t p = n.find(L"/Windows/TerrariaServer.exe");
        if (p != std::wstring::npos) prefix = n.substr(0, p + 8);
    }
    if (prefix.empty()) { *error = L"The Terraria download didn't contain the Windows server."; return false; }
    if (progress) progress(-1, L"Unpacking");
    if (!archive::Extract(zip, s.FilesDir(), prefix, nullptr, error)) return false;
    DeleteFileW(zip.c_str());
    s.installedVersion = ver;
    return true;
}

// ---------------------------------------------------------------- SteamCMD
static std::wstring SteamCmdExe() { return DataDir() + L"\\steamcmd\\steamcmd.exe"; }

bool EnsureSteamCmd(const Progress& progress, std::wstring* exe, std::wstring* error) {
    *exe = SteamCmdExe();
    if (FileExists(*exe)) return true;
    std::wstring zip = DownloadDir() + L"\\steamcmd.zip";
    if (!Fetch(L"https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip", zip, L"SteamCMD", L"", nullptr, progress, nullptr, error)) return false;
    if (!archive::Extract(zip, PathParent(*exe), L"", nullptr, error)) return false;
    DeleteFileW(zip.c_str());
    return FileExists(*exe);
}

static std::wstring SteamBuildId(const std::wstring& dir, int appId) {
    std::wstring acf;
    if (!ReadTextFile(dir + L"\\steamapps\\appmanifest_" + std::to_wstring(appId) + L".acf", acf)) return L"";
    std::wsmatch m;
    std::wregex re(L"\"buildid\"\\s+\"(\\d+)\"");
    return std::regex_search(acf, m, re) ? m[1].str() : L"";
}

static bool InstallSteam(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    const json::Value* src = t.Obj(L"source");
    int appId = (int)src->GetNum(L"appId", 0);
    std::wstring branch = src->GetStr(L"branch");
    std::wstring steamcmd;
    if (!EnsureSteamCmd(progress, &steamcmd, error)) return false;
    std::wstring dir = s.FilesDir();
    EnsureDir(dir);
    std::wstring args = L"+force_install_dir " + proc::Quote(dir) + L" +login anonymous +app_update " + std::to_wstring(appId);
    if (!branch.empty() && branch != L"public") args += L" -beta " + branch;
    args += L" validate +quit";
    std::wstring lastErr;
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (progress) progress(-1, attempt == 1 ? L"Starting SteamCMD" : L"SteamCMD is updating itself, trying again");
        int code = proc::RunCapture(steamcmd, args, PathParent(steamcmd), [&](const std::string& raw) {
            std::wstring line = Trim(Utf8ToWide(raw));
            Log(L"steamcmd: %ls", line.c_str());
            size_t p = line.find(L"progress: ");
            if (p != std::wstring::npos && progress) {
                double pct = _wtof(line.c_str() + p + 10);
                std::wstring stage = line.find(L"verifying") != std::wstring::npos ? L"Verifying" :
                                     line.find(L"preallocating") != std::wstring::npos ? L"Preparing" : L"Downloading";
                // "(1234 / 5678)" gives the size
                size_t a = line.find(L'('), sl = line.find(L" / "), b = line.find(L')');
                std::wstring size;
                if (a != std::wstring::npos && sl != std::wstring::npos && b != std::wstring::npos && sl > a && b > sl)
                    size = L" of " + FormatBytes(_wcstoui64(line.substr(sl + 3, b - sl - 3).c_str(), nullptr, 10));
                progress((int)pct, stage + L" " + t.name + size);
            } else if (line.find(L"Error!") != std::wstring::npos || line.find(L"ERROR") != std::wstring::npos || line.find(L"FAILED") != std::wstring::npos) {
                lastErr = line;
            } else if (progress && (line.find(L"Logging in") != std::wstring::npos || line.find(L"Waiting for") != std::wstring::npos)) {
                progress(-1, line.substr(0, 100));
            }
        }, cancel);
        if (cancel && *cancel) { *error = L"Cancelled."; return false; }
        std::wstring build = SteamBuildId(dir, appId);
        if (code == 0 && !build.empty() && lastErr.empty()) { s.installedVersion = L"build " + build; return true; }
        if (lastErr.find(L"0x202") != std::wstring::npos) { *error = L"Not enough disk space for " + t.name + L"."; return false; }
        if (lastErr.find(L"No subscription") != std::wstring::npos) { *error = t.name + L" can't be downloaded anonymously."; return false; }
        if (code != 7 && code != 6 && !lastErr.empty() && attempt >= 3) break;
        lastErr.clear();
        Sleep(1500);
    }
    *error = L"SteamCMD couldn't download " + t.name + L"." + (lastErr.empty() ? L"" : L" (" + lastErr + L")");
    return false;
}

// ---------------------------------------------------------------- generic url
static bool InstallUrl(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    const json::Value* src = t.Obj(L"source");
    std::wstring url = Expand(src->GetStr(L"url"), s, t);
    std::wstring name = PathName(url);
    size_t q = name.find(L'?');
    if (q != std::wstring::npos) name = name.substr(0, q);
    std::wstring file = DownloadDir() + L"\\" + name;
    if (!Fetch(url, file, t.name, src->GetStr(L"sha256"), BCRYPT_SHA256_ALGORITHM, progress, cancel, error)) return false;
    if (IEquals(file.substr(file.size() > 4 ? file.size() - 4 : 0), L".zip")) {
        if (!archive::Extract(file, s.FilesDir(), src->GetStr(L"zipFolder"), nullptr, error)) return false;
        DeleteFileW(file.c_str());
    } else {
        std::wstring dest = s.FilesDir() + L"\\" + src->GetStr(L"saveAs", name);
        EnsureDir(s.FilesDir());
        if (!MoveFileExW(file.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) { *error = L"Couldn't save " + dest; return false; }
    }
    s.installedVersion = src->GetStr(L"version", L"downloaded");
    return true;
}

// Things a game's server needs on the PC (Terraria: Microsoft XNA Framework 4.0). Installed once, with Windows' permission prompt.
static bool Prerequisites(const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    const json::Value* pre = t.Obj(L"prerequisites");
    if (!pre || pre->type != json::Value::Array) return true;
    for (auto& p : pre->arr) {
        std::wstring check = ExpandEnv(p.GetStr(L"check")), name = p.GetStr(L"name");
        if (!check.empty() && GetFileAttributesW(check.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        std::wstring file = DownloadDir() + L"\\" + p.GetStr(L"file", L"prerequisite.exe");
        if (!Fetch(p.GetStr(L"url"), file, name, p.GetStr(L"sha256"), BCRYPT_SHA256_ALGORITHM, progress, cancel, error)) return false;
        if (progress) progress(-1, L"Installing " + name + L" (Windows may ask for permission)");
        std::wstring run = p.GetStr(L"run"), args = p.GetStr(L"args");
        for (size_t q; (q = args.find(L"{file}")) != std::wstring::npos;) args.replace(q, 6, file);
        if (run.empty()) run = file;
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = p.GetBool(L"elevated", false) ? L"runas" : L"open";
        sei.lpFile = run.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) { *error = name + L" is needed and wasn't installed (permission declined?)."; return false; }
        WaitForSingleObject(sei.hProcess, 10 * 60 * 1000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        Log(L"prerequisite %ls: exit code %lu", name.c_str(), code);
        if (!check.empty() && GetFileAttributesW(check.c_str()) == INVALID_FILE_ATTRIBUTES) { *error = name + L" didn't install (code " + std::to_wstring(code) + L")."; return false; }
    }
    return true;
}

bool Install(ServerDef& s, const Template& t, const Progress& progress, std::atomic<bool>* cancel, std::wstring* error) {
    Log(L"install %ls (%ls %ls %ls)", s.id.c_str(), t.id.c_str(), s.variant.c_str(), s.version.c_str());
    EnsureDir(DownloadDir());
    if (!Prerequisites(t, progress, cancel, error)) return false;
    bool ok = false;
    if (t.sourceType == L"minecraft") ok = InstallMinecraft(s, t, progress, cancel, error);
    else if (t.sourceType == L"bedrock") ok = InstallBedrock(s, progress, cancel, error);
    else if (t.sourceType == L"terraria") ok = InstallTerraria(s, t, progress, cancel, error);
    else if (t.sourceType == L"steamcmd") ok = InstallSteam(s, t, progress, cancel, error);
    else if (t.sourceType == L"url") ok = InstallUrl(s, t, progress, cancel, error);
    else if (t.sourceType == L"manual") { ok = true; s.installedVersion = L"your files"; }
    else *error = L"This template uses a download type Outpost doesn't know (\"" + t.sourceType + L"\").";
    if (ok) Log(L"install %ls: done, %ls", s.id.c_str(), s.installedVersion.c_str());
    else Log(L"install %ls failed: %ls", s.id.c_str(), error->c_str());
    return ok;
}

std::wstring CheckUpdate(const ServerDef& s, const Template& t, std::wstring* error) {
    if (t.sourceType == L"steamcmd") {
        int appId = (int)t.Obj(L"source")->GetNum(L"appId", 0);
        std::wstring branch = t.Obj(L"source")->GetStr(L"branch", L"public");
        json::Value v;
        if (!GetJson(L"https://api.steamcmd.net/v1/info/" + std::to_wstring(appId), v, error)) return L"";
        const json::Value* d = v.Get(L"data");
        d = d ? d->Get(std::to_wstring(appId)) : nullptr;
        d = d ? d->Get(L"depots") : nullptr;
        d = d ? d->Get(L"branches") : nullptr;
        d = d ? d->Get(branch) : nullptr;
        std::wstring latest = d ? d->GetStr(L"buildid") : L"";
        std::wstring mine = SteamBuildId(s.FilesDir(), appId);
        return (!latest.empty() && !mine.empty() && latest != mine) ? L"build " + latest : L"";
    }
    if (t.sourceType == L"minecraft") {
        if (s.version != L"latest" && s.variant != L"paper") return L"";
        std::wstring mc = s.version == L"latest" ? ListVersions(t, s.variant.empty() ? L"vanilla" : s.variant).latest : s.version;
        if (mc.empty()) return L"";
        std::wstring cur = s.vars.count(L"mcVersion") ? s.vars.at(L"mcVersion") : L"";
        if (mc != cur) return mc;
        if (s.variant == L"paper") {
            json::Value b;
            if (GetJson(L"https://fill.papermc.io/v3/projects/paper/versions/" + mc + L"/builds/latest", b, error)) {
                wchar_t build[32]; swprintf(build, 32, L"%g", b.GetNum(L"id", 0));
                std::wstring latest = mc + L" (build " + build + L")";
                if (latest != s.installedVersion) return latest;
            }
        }
        return L"";
    }
    if (t.sourceType == L"terraria") {
        if (s.version != L"latest") return L"";
        Versions v = ListVersions(t, L"");
        return (!v.latest.empty() && v.latest != s.installedVersion) ? v.latest : L"";
    }
    if (t.sourceType == L"bedrock") {
        json::Value v;
        if (GetJson(L"https://net-secondary.web.minecraft-services.net/api/v1.0/download/links", v, error))
            if (const json::Value* res = v.Get(L"result"))
                if (const json::Value* links = res->Get(L"links"))
                    for (auto& l : links->arr)
                        if (l.GetStr(L"downloadType") == L"serverBedrockWindows") {
                            std::wstring u = l.GetStr(L"downloadUrl");
                            size_t a = u.find(L"server-"), b = u.rfind(L".zip");
                            std::wstring ver = (a != std::wstring::npos && b != std::wstring::npos) ? u.substr(a + 7, b - a - 7) : L"";
                            return (!ver.empty() && ver != s.installedVersion) ? ver : L"";
                        }
    }
    return L"";
}

std::wstring DetectGame(const std::wstring& folder, std::wstring* variant) {
    variant->clear();
    if (FileExists(folder + L"\\server.properties") && !FileExists(folder + L"\\bedrock_server.exe")) {
        *variant = L"vanilla";
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((folder + L"\\*.jar").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = Lower(fd.cFileName);
                if (n.find(L"paper") != std::wstring::npos || n.find(L"purpur") != std::wstring::npos) *variant = L"paper";
                else if (n.find(L"fabric") != std::wstring::npos) *variant = L"fabric";
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        std::wstring bat;
        if (ReadTextFile(folder + L"\\run.bat", bat)) {
            if (bat.find(L"neoforged") != std::wstring::npos) *variant = L"neoforge";
            else if (bat.find(L"minecraftforge") != std::wstring::npos) *variant = L"forge";
        }
        return L"minecraft-java";
    }
    // anything else: the template whose launch exe is there
    for (auto& t : templates::All()) {
        const json::Value* l = t.Obj(L"launch");
        std::wstring exe = l ? l->GetStr(L"exe") : L"";
        std::wstring bat = l ? l->GetStr(L"batch") : L"";
        if ((!exe.empty() && exe[0] != L'{' && FileExists(folder + L"\\" + exe)) || (!bat.empty() && FileExists(folder + L"\\" + bat))) return t.id;
    }
    return L"";
}

} // namespace provision
