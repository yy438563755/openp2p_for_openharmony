#include "napi/native_api.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hilog/log.h"

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x4F50
#define LOG_TAG "OpenP2PNAPI"

using RunAsModuleFn = int (*)(char *, char *, int, int);
using GetTokenFn = char *(*)(char *);
using NetworkConnectFn = int (*)(int);
using IsOnlineFn = int (*)(void);
using FreeCStringFn = void (*)(char *);
using StopModuleFn = void (*)(void);
using GetOhosSDWANConfigFn = int (*)(unsigned char *, int);
using OhosReadFn = void (*)(unsigned char *, int);
using OhosWriteFn = int (*)(unsigned char *, int, int);
using GetOhosNodeNameFn = char *(*)(void);
using GetOhosProbeStatusFn = char *(*)(void);
using GetLastErrorFn = char *(*)(void);

static void *g_handle = nullptr;
static RunAsModuleFn g_runAsModule = nullptr;
static GetTokenFn g_getToken = nullptr;
static NetworkConnectFn g_networkConnect = nullptr;
static IsOnlineFn g_isOnline = nullptr;
static FreeCStringFn g_freeCString = nullptr;
static StopModuleFn g_stopModule = nullptr;
static GetOhosSDWANConfigFn g_getSdwan = nullptr;
static OhosReadFn g_ohosRead = nullptr;
static OhosWriteFn g_ohosWrite = nullptr;
static GetOhosNodeNameFn g_getNodeName = nullptr;
static GetOhosProbeStatusFn g_getProbeStatus = nullptr;
static GetLastErrorFn g_getLastError = nullptr;
static std::string g_loadError;

struct RunAsModuleAsyncData {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::string baseDir;
    std::string token;
    int32_t bw = 0;
    int32_t logLevel = 1;
    int ok = 0;
};

struct GetSdwanConfigAsyncData {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    std::vector<unsigned char> buf;
    int len = 0;
};

static constexpr int TUN_PUMP_BUF_SIZE = 4096;
static constexpr int BRIDGE_MAX_FRAME = 4096;
static std::thread g_tunPumpThread;
static std::atomic<bool> g_tunPumpRunning{false};
static int g_tunPumpFd = -1;

// Cross-process TUN bridge: UI (host+Go) <-> AF_UNIX <-> VPN (device+TUN)
static std::thread g_bridgeThread;
static std::atomic<bool> g_bridgeRunning{false};
static int g_bridgeListenFd = -1;
static int g_bridgeConnFd = -1;
static int g_bridgeTunFd = -1;
static std::string g_bridgeSockPath;

static void TunPumpLoop()
{
    std::vector<unsigned char> readBuf(TUN_PUMP_BUF_SIZE);
    std::vector<unsigned char> writeBuf(TUN_PUMP_BUF_SIZE);
    while (g_tunPumpRunning.load()) {
        int tunFd = g_tunPumpFd;
        if (tunFd < 0) {
            break;
        }
        // Poll TUN for readability so Go->TUN writes are not starved by blocking read().
        struct pollfd pfd {};
        pfd.fd = tunFd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 20);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            OH_LOG_ERROR(LOG_APP, "tun poll error fd=%{public}d errno=%{public}d", tunFd, errno);
            break;
        }
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            OH_LOG_ERROR(LOG_APP, "tun poll hangup/err fd=%{public}d revents=%{public}d", tunFd, pfd.revents);
            break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (int i = 0; i < 32; i++) {
                ssize_t n = read(tunFd, readBuf.data(), readBuf.size());
                if (n > 0 && g_ohosRead != nullptr) {
                    g_ohosRead(readBuf.data(), static_cast<int>(n));
                } else if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    OH_LOG_ERROR(LOG_APP, "tun read error fd=%{public}d errno=%{public}d", tunFd, errno);
                    break;
                } else {
                    break;
                }
            }
        }
        // Drain Go -> system TUN (non-blocking ohosWrite timeout=0)
        if (g_ohosWrite != nullptr) {
            for (int i = 0; i < 32; i++) {
                int outLen = g_ohosWrite(writeBuf.data(), TUN_PUMP_BUF_SIZE, 0);
                if (outLen <= 0) {
                    break;
                }
                ssize_t wn = write(tunFd, writeBuf.data(), static_cast<size_t>(outLen));
                if (wn < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    OH_LOG_ERROR(LOG_APP, "tun write error fd=%{public}d errno=%{public}d", tunFd, errno);
                    break;
                }
            }
        }
    }
}

static void StopTunPumpInternal()
{
    g_tunPumpRunning.store(false);
    int fd = g_tunPumpFd;
    g_tunPumpFd = -1;
    if (fd >= 0) {
        close(fd);
    }
    if (g_tunPumpThread.joinable()) {
        g_tunPumpThread.join();
    }
}

static bool WriteAll(int fd, const void *buf, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd {};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                if (poll(&pfd, 1, 200) <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

static bool ReadAll(int fd, void *buf, size_t len)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd {};
                pfd.fd = fd;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 200) <= 0) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

static bool SendFrame(int fd, const uint8_t *data, uint32_t len)
{
    if (len == 0 || len > BRIDGE_MAX_FRAME) {
        return false;
    }
    uint32_t be = htonl(len);
    return WriteAll(fd, &be, 4) && WriteAll(fd, data, len);
}

static int RecvFrame(int fd, uint8_t *buf, size_t buflen)
{
    uint32_t be = 0;
    if (!ReadAll(fd, &be, 4)) {
        return -1;
    }
    uint32_t len = ntohl(be);
    if (len == 0 || len > BRIDGE_MAX_FRAME || len > buflen) {
        return -1;
    }
    if (!ReadAll(fd, buf, len)) {
        return -1;
    }
    return static_cast<int>(len);
}

static void SetNonBlock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void StopBridgeInternal()
{
    g_bridgeRunning.store(false);
    int listenFd = g_bridgeListenFd;
    int connFd = g_bridgeConnFd;
    int tunFd = g_bridgeTunFd;
    g_bridgeListenFd = -1;
    g_bridgeConnFd = -1;
    g_bridgeTunFd = -1;
    if (listenFd >= 0) {
        close(listenFd);
    }
    if (connFd >= 0) {
        close(connFd);
    }
    if (tunFd >= 0) {
        close(tunFd);
    }
    if (g_bridgeThread.joinable()) {
        g_bridgeThread.join();
    }
    if (!g_bridgeSockPath.empty()) {
        unlink(g_bridgeSockPath.c_str());
        g_bridgeSockPath.clear();
    }
}

/** UI process: accept VPN client, forward sock <-> Go OhosRead/OhosWrite. */
static void BridgeHostLoop()
{
    std::vector<uint8_t> frame(BRIDGE_MAX_FRAME);
    std::vector<unsigned char> goBuf(BRIDGE_MAX_FRAME);
    // Keep re-accepting: VPN may recreate TUN / reconnect after config changes.
    while (g_bridgeRunning.load()) {
        if (g_bridgeConnFd >= 0) {
            int old = g_bridgeConnFd;
            g_bridgeConnFd = -1;
            close(old);
        }
        OH_LOG_INFO(LOG_APP, "bridge host waiting accept");
        int sock = -1;
        while (g_bridgeRunning.load() && sock < 0) {
            int lfd = g_bridgeListenFd;
            if (lfd < 0) {
                OH_LOG_INFO(LOG_APP, "bridge host loop exit (no listen)");
                return;
            }
            struct pollfd pfd {};
            pfd.fd = lfd;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 200);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                int cfd = accept(lfd, nullptr, nullptr);
                if (cfd >= 0) {
                    SetNonBlock(cfd);
                    g_bridgeConnFd = cfd;
                    sock = cfd;
                    OH_LOG_INFO(LOG_APP, "bridge host accepted fd=%{public}d", cfd);
                }
            }
        }
        while (g_bridgeRunning.load() && sock >= 0) {
            struct pollfd pfd {};
            pfd.fd = sock;
            pfd.events = POLLIN;
            int pr = poll(&pfd, 1, 20);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                OH_LOG_ERROR(LOG_APP, "bridge host sock hangup revents=%{public}d", pfd.revents);
                break;
            }
            if (pr > 0 && (pfd.revents & POLLIN)) {
                int flags = fcntl(sock, F_GETFL, 0);
                if (flags >= 0) {
                    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
                }
                int n = RecvFrame(sock, frame.data(), frame.size());
                if (flags >= 0) {
                    fcntl(sock, F_SETFL, flags);
                }
                if (n <= 0) {
                    break;
                }
                if (g_ohosRead != nullptr) {
                    g_ohosRead(frame.data(), n);
                }
            }
            if (g_ohosWrite != nullptr) {
                bool sendFail = false;
                for (int i = 0; i < 32; i++) {
                    int outLen = g_ohosWrite(goBuf.data(), BRIDGE_MAX_FRAME, 0);
                    if (outLen <= 0) {
                        break;
                    }
                    if (!SendFrame(sock, goBuf.data(), static_cast<uint32_t>(outLen))) {
                        sendFail = true;
                        break;
                    }
                }
                if (sendFail) {
                    break;
                }
            }
            sock = g_bridgeConnFd;
        }
        OH_LOG_INFO(LOG_APP, "bridge host session ended, will re-accept");
    }
    OH_LOG_INFO(LOG_APP, "bridge host loop exit");
}

/** VPN process: connect to UI host, forward TUN <-> sock (no Go). */
static void BridgeDeviceLoop(std::string sockPath)
{
    std::vector<uint8_t> frame(BRIDGE_MAX_FRAME);
    std::vector<unsigned char> tunBuf(BRIDGE_MAX_FRAME);

    if (sockPath.size() >= sizeof(sockaddr_un{}.sun_path)) {
        OH_LOG_ERROR(LOG_APP, "bridge device path too long");
        return;
    }

    int tryCount = 0;
    while (g_bridgeRunning.load()) {
        int tunFd = g_bridgeTunFd;
        if (tunFd < 0) {
            break;
        }
        SetNonBlock(tunFd);

        int sock = -1;
        while (g_bridgeRunning.load() && sock < 0) {
            sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (sock < 0) {
                usleep(200 * 1000);
                continue;
            }
            struct sockaddr_un addr {};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
            if (connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) {
                OH_LOG_INFO(LOG_APP, "bridge device connected try=%{public}d", tryCount);
                break;
            }
            close(sock);
            sock = -1;
            tryCount++;
            if (tryCount % 25 == 0) {
                OH_LOG_WARN(LOG_APP, "bridge device still connecting path=%{public}s try=%{public}d errno=%{public}d",
                    sockPath.c_str(), tryCount, errno);
            }
            usleep(200 * 1000);
        }
        if (sock < 0 || !g_bridgeRunning.load()) {
            break;
        }
        if (g_bridgeConnFd >= 0 && g_bridgeConnFd != sock) {
            close(g_bridgeConnFd);
        }
        g_bridgeConnFd = sock;
        SetNonBlock(sock);

        while (g_bridgeRunning.load() && tunFd >= 0 && sock >= 0) {
            struct pollfd pfds[2] {};
            pfds[0].fd = tunFd;
            pfds[0].events = POLLIN;
            pfds[1].fd = sock;
            pfds[1].events = POLLIN;
            int pr = poll(pfds, 2, 20);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                OH_LOG_ERROR(LOG_APP, "bridge device tun hangup");
                g_bridgeRunning.store(false);
                break;
            }
            if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                OH_LOG_ERROR(LOG_APP, "bridge device sock hangup");
                break;
            }
            if (pfds[0].revents & POLLIN) {
                bool sendFail = false;
                for (int i = 0; i < 32; i++) {
                    ssize_t n = read(tunFd, tunBuf.data(), tunBuf.size());
                    if (n > 0) {
                        if (!SendFrame(sock, tunBuf.data(), static_cast<uint32_t>(n))) {
                            sendFail = true;
                            break;
                        }
                    } else if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        sendFail = true;
                        break;
                    } else {
                        break;
                    }
                }
                if (sendFail) {
                    break;
                }
            }
            if (pfds[1].revents & POLLIN) {
                int flags = fcntl(sock, F_GETFL, 0);
                if (flags >= 0) {
                    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
                }
                int n = RecvFrame(sock, frame.data(), frame.size());
                if (flags >= 0) {
                    fcntl(sock, F_SETFL, flags);
                }
                if (n <= 0) {
                    break;
                }
                ssize_t wn = write(tunFd, frame.data(), static_cast<size_t>(n));
                if (wn < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    OH_LOG_ERROR(LOG_APP, "bridge device tun write errno=%{public}d", errno);
                    break;
                }
            }
            tunFd = g_bridgeTunFd;
            sock = g_bridgeConnFd;
        }
        if (g_bridgeConnFd >= 0) {
            close(g_bridgeConnFd);
            g_bridgeConnFd = -1;
        }
        OH_LOG_INFO(LOG_APP, "bridge device session ended, will reconnect");
        usleep(100 * 1000);
    }
    OH_LOG_INFO(LOG_APP, "bridge device loop exit");
}

static bool EnsureOpenP2PLoaded()
{
    if (g_handle != nullptr) {
        return true;
    }

    g_handle = dlopen("libopenp2p.so", RTLD_NOW | RTLD_GLOBAL);
    if (g_handle == nullptr) {
        const char *err = dlerror();
        g_loadError = err != nullptr ? err : "dlopen libopenp2p.so failed";
        OH_LOG_ERROR(LOG_APP, "dlopen libopenp2p.so failed: %{public}s", g_loadError.c_str());
        return false;
    }

    g_runAsModule = reinterpret_cast<RunAsModuleFn>(dlsym(g_handle, "RunAsModule"));
    g_getToken = reinterpret_cast<GetTokenFn>(dlsym(g_handle, "GetToken"));
    g_networkConnect = reinterpret_cast<NetworkConnectFn>(dlsym(g_handle, "NetworkConnect"));
    g_isOnline = reinterpret_cast<IsOnlineFn>(dlsym(g_handle, "IsOnline"));
    g_freeCString = reinterpret_cast<FreeCStringFn>(dlsym(g_handle, "FreeCString"));
    g_stopModule = reinterpret_cast<StopModuleFn>(dlsym(g_handle, "StopModule"));
    g_getSdwan = reinterpret_cast<GetOhosSDWANConfigFn>(dlsym(g_handle, "GetOhosSDWANConfig"));
    g_ohosRead = reinterpret_cast<OhosReadFn>(dlsym(g_handle, "OhosRead"));
    g_ohosWrite = reinterpret_cast<OhosWriteFn>(dlsym(g_handle, "OhosWrite"));
    g_getNodeName = reinterpret_cast<GetOhosNodeNameFn>(dlsym(g_handle, "GetOhosNodeName"));
    g_getProbeStatus = reinterpret_cast<GetOhosProbeStatusFn>(dlsym(g_handle, "GetOhosProbeStatus"));
    g_getLastError = reinterpret_cast<GetLastErrorFn>(dlsym(g_handle, "GetLastError"));

    if (g_runAsModule == nullptr || g_getToken == nullptr) {
        g_loadError = "dlsym missing core exports (RunAsModule/GetToken)";
        OH_LOG_ERROR(LOG_APP, "%{public}s", g_loadError.c_str());
        dlclose(g_handle);
        g_handle = nullptr;
        return false;
    }

    OH_LOG_INFO(LOG_APP, "libopenp2p.so loaded ok");
    return true;
}

static std::string NapiGetString(napi_env env, napi_value value)
{
    size_t len = 0;
    napi_get_value_string_utf8(env, value, nullptr, 0, &len);
    std::vector<char> buf(len + 1);
    napi_get_value_string_utf8(env, value, buf.data(), buf.size(), &len);
    return std::string(buf.data(), len);
}

static void ExecuteRunAsModule(napi_env env, void *data)
{
    (void)env;
    auto *asyncData = static_cast<RunAsModuleAsyncData *>(data);
    if (g_runAsModule == nullptr) {
        asyncData->ok = 0;
        return;
    }
    asyncData->ok = g_runAsModule(const_cast<char *>(asyncData->baseDir.c_str()),
        const_cast<char *>(asyncData->token.c_str()), asyncData->bw, asyncData->logLevel);
    OH_LOG_INFO(LOG_APP, "RunAsModule async finished ok=%{public}d", asyncData->ok);
}

static void CompleteRunAsModule(napi_env env, napi_status status, void *data)
{
    auto *asyncData = static_cast<RunAsModuleAsyncData *>(data);
    napi_value result = nullptr;
    if (status != napi_ok) {
        napi_get_boolean(env, false, &result);
    } else {
        napi_get_boolean(env, asyncData->ok != 0, &result);
    }
    napi_resolve_deferred(env, asyncData->deferred, result);
    napi_delete_async_work(env, asyncData->work);
    delete asyncData;
}

// Returns Promise<boolean>. Heavy Go login runs off the UI thread.
static napi_value NapiRunAsModule(napi_env env, napi_callback_info info)
{
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    if (!EnsureOpenP2PLoaded() || g_runAsModule == nullptr) {
        napi_value falseVal = nullptr;
        napi_get_boolean(env, false, &falseVal);
        napi_resolve_deferred(env, deferred, falseVal);
        return promise;
    }

    size_t argc = 4;
    napi_value args[4] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    auto *asyncData = new RunAsModuleAsyncData();
    asyncData->deferred = deferred;
    asyncData->baseDir = NapiGetString(env, args[0]);
    asyncData->token = NapiGetString(env, args[1]);
    if (argc > 2) {
        napi_get_value_int32(env, args[2], &asyncData->bw);
    }
    if (argc > 3) {
        napi_get_value_int32(env, args[3], &asyncData->logLevel);
    }

    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "OpenP2PRunAsModule", NAPI_AUTO_LENGTH, &resourceName);
    napi_status st = napi_create_async_work(env, nullptr, resourceName, ExecuteRunAsModule, CompleteRunAsModule,
        asyncData, &asyncData->work);
    if (st != napi_ok) {
        napi_value falseVal = nullptr;
        napi_get_boolean(env, false, &falseVal);
        napi_resolve_deferred(env, deferred, falseVal);
        delete asyncData;
        return promise;
    }
    napi_queue_async_work(env, asyncData->work);
    return promise;
}

static napi_value NapiGetToken(napi_env env, napi_callback_info info)
{
    napi_value result;
    if (!EnsureOpenP2PLoaded() || g_getToken == nullptr) {
        napi_create_string_utf8(env, "0", NAPI_AUTO_LENGTH, &result);
        return result;
    }

    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string baseDir = NapiGetString(env, args[0]);
    char *token = g_getToken(const_cast<char *>(baseDir.c_str()));
    if (token == nullptr) {
        napi_create_string_utf8(env, "0", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    napi_create_string_utf8(env, token, NAPI_AUTO_LENGTH, &result);
    if (g_freeCString != nullptr) {
        g_freeCString(token);
    }
    return result;
}

static napi_value NapiConnect(napi_env env, napi_callback_info info)
{
    napi_value result;
    if (!EnsureOpenP2PLoaded() || g_networkConnect == nullptr) {
        napi_get_boolean(env, false, &result);
        return result;
    }

    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t timeoutMs = 1000;
    if (argc > 0) {
        napi_get_value_int32(env, args[0], &timeoutMs);
    }
    int ok = g_networkConnect(timeoutMs);
    napi_get_boolean(env, ok != 0, &result);
    return result;
}

static napi_value NapiIsOnline(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result;
    bool online = false;
    if (EnsureOpenP2PLoaded() && g_isOnline != nullptr) {
        online = g_isOnline() != 0;
    }
    napi_get_boolean(env, online, &result);
    return result;
}

static napi_value NapiStop(napi_env env, napi_callback_info info)
{
    (void)info;
    StopBridgeInternal();
    StopTunPumpInternal();
    if (EnsureOpenP2PLoaded() && g_stopModule != nullptr) {
        g_stopModule();
    }
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value NapiGetNodeName(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result;
    if (!EnsureOpenP2PLoaded() || g_getNodeName == nullptr) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    char *name = g_getNodeName();
    if (name == nullptr) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &result);
    if (g_freeCString != nullptr) {
        g_freeCString(name);
    }
    return result;
}

static napi_value NapiGetProbeStatus(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result;
    if (!EnsureOpenP2PLoaded() || g_getProbeStatus == nullptr) {
        napi_create_string_utf8(env, "探测接口未就绪", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    char *status = g_getProbeStatus();
    if (status == nullptr) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    napi_create_string_utf8(env, status, NAPI_AUTO_LENGTH, &result);
    if (g_freeCString != nullptr) {
        g_freeCString(status);
    }
    return result;
}

static napi_value NapiGetSdwanConfig(napi_env env, napi_callback_info info)
{
    (void)info;
    std::vector<unsigned char> buf(64 * 1024);
    int n = 0;
    if (EnsureOpenP2PLoaded() && g_getSdwan != nullptr) {
        n = g_getSdwan(buf.data(), static_cast<int>(buf.size()));
    }
    napi_value result;
    void *data = nullptr;
    napi_create_arraybuffer(env, n > 0 ? static_cast<size_t>(n) : 0, &data, &result);
    if (n > 0 && data != nullptr) {
        memcpy(data, buf.data(), static_cast<size_t>(n));
    }
    return result;
}

static void ExecuteGetSdwanConfig(napi_env env, void *data)
{
    (void)env;
    auto *asyncData = static_cast<GetSdwanConfigAsyncData *>(data);
    asyncData->buf.resize(64 * 1024);
    asyncData->len = 0;
    if (EnsureOpenP2PLoaded() && g_getSdwan != nullptr) {
        asyncData->len = g_getSdwan(asyncData->buf.data(), static_cast<int>(asyncData->buf.size()));
    }
}

static void CompleteGetSdwanConfig(napi_env env, napi_status status, void *data)
{
    auto *asyncData = static_cast<GetSdwanConfigAsyncData *>(data);
    napi_value result = nullptr;
    void *out = nullptr;
    size_t outLen = 0;
    if (status == napi_ok && asyncData->len > 0) {
        outLen = static_cast<size_t>(asyncData->len);
    }
    napi_create_arraybuffer(env, outLen, &out, &result);
    if (outLen > 0 && out != nullptr) {
        memcpy(out, asyncData->buf.data(), outLen);
    }
    napi_resolve_deferred(env, asyncData->deferred, result);
    napi_delete_async_work(env, asyncData->work);
    delete asyncData;
}

static napi_value NapiGetSdwanConfigAsync(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value promise = nullptr;
    napi_deferred deferred = nullptr;
    napi_create_promise(env, &deferred, &promise);

    if (!EnsureOpenP2PLoaded() || g_getSdwan == nullptr) {
        napi_value emptyBuf = nullptr;
        void *data = nullptr;
        napi_create_arraybuffer(env, 0, &data, &emptyBuf);
        napi_resolve_deferred(env, deferred, emptyBuf);
        return promise;
    }

    auto *asyncData = new GetSdwanConfigAsyncData();
    asyncData->deferred = deferred;
    napi_value resourceName = nullptr;
    napi_create_string_utf8(env, "OpenP2PGetSdwanConfig", NAPI_AUTO_LENGTH, &resourceName);
    napi_status st = napi_create_async_work(env, nullptr, resourceName, ExecuteGetSdwanConfig,
        CompleteGetSdwanConfig, asyncData, &asyncData->work);
    if (st != napi_ok) {
        napi_value emptyBuf = nullptr;
        void *data = nullptr;
        napi_create_arraybuffer(env, 0, &data, &emptyBuf);
        napi_resolve_deferred(env, deferred, emptyBuf);
        delete asyncData;
        return promise;
    }
    napi_queue_async_work(env, asyncData->work);
    return promise;
}

static napi_value NapiStartTunPump(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t tunFd = -1;
    if (argc > 0) {
        napi_get_value_int32(env, args[0], &tunFd);
    }
    StopTunPumpInternal();
    if (tunFd < 0) {
        OH_LOG_ERROR(LOG_APP, "startTunPump invalid fd=%{public}d", tunFd);
        napi_value result = nullptr;
        napi_get_boolean(env, false, &result);
        return result;
    }
    int dupFd = dup(tunFd);
    if (dupFd < 0) {
        OH_LOG_ERROR(LOG_APP, "startTunPump dup failed errno=%{public}d", errno);
        napi_value result = nullptr;
        napi_get_boolean(env, false, &result);
        return result;
    }
    int flags = fcntl(dupFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(dupFd, F_SETFL, flags | O_NONBLOCK);
    }
    g_tunPumpFd = dupFd;
    g_tunPumpRunning.store(true);
    g_tunPumpThread = std::thread(TunPumpLoop);
    OH_LOG_INFO(LOG_APP, "startTunPump fd=%{public}d dup=%{public}d nonblock", tunFd, dupFd);
    napi_value result = nullptr;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value NapiStopTunPump(napi_env env, napi_callback_info info)
{
    (void)info;
    StopTunPumpInternal();
    OH_LOG_INFO(LOG_APP, "stopTunPump");
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

/** UI: listen on unix socket and bridge to Go channels. */
static napi_value NapiStartTunBridgeHost(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char pathBuf[512] = {0};
    size_t pathLen = 0;
    if (argc > 0) {
        napi_get_value_string_utf8(env, args[0], pathBuf, sizeof(pathBuf) - 1, &pathLen);
    }
    napi_value result = nullptr;
    if (pathLen == 0) {
        napi_get_boolean(env, false, &result);
        return result;
    }
    if (!EnsureOpenP2PLoaded() || g_ohosRead == nullptr || g_ohosWrite == nullptr) {
        OH_LOG_ERROR(LOG_APP, "bridge host needs Go exports");
        napi_get_boolean(env, false, &result);
        return result;
    }
    StopBridgeInternal();
    StopTunPumpInternal();

    unlink(pathBuf);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        napi_get_boolean(env, false, &result);
        return result;
    }
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    if (pathLen >= sizeof(addr.sun_path)) {
        close(lfd);
        napi_get_boolean(env, false, &result);
        return result;
    }
    std::strncpy(addr.sun_path, pathBuf, sizeof(addr.sun_path) - 1);
    if (bind(lfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || listen(lfd, 1) != 0) {
        OH_LOG_ERROR(LOG_APP, "bridge host bind/listen errno=%{public}d", errno);
        close(lfd);
        napi_get_boolean(env, false, &result);
        return result;
    }
    g_bridgeSockPath = pathBuf;
    g_bridgeListenFd = lfd;
    g_bridgeRunning.store(true);
    g_bridgeThread = std::thread(BridgeHostLoop);
    OH_LOG_INFO(LOG_APP, "bridge host listening %{public}s", pathBuf);
    napi_get_boolean(env, true, &result);
    return result;
}

/** VPN: connect to UI host and pump TUN packets across the socket. */
static napi_value NapiStartTunBridgeDevice(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    char pathBuf[512] = {0};
    size_t pathLen = 0;
    int32_t tunFd = -1;
    if (argc > 0) {
        napi_get_value_string_utf8(env, args[0], pathBuf, sizeof(pathBuf) - 1, &pathLen);
    }
    if (argc > 1) {
        napi_get_value_int32(env, args[1], &tunFd);
    }
    napi_value result = nullptr;
    if (pathLen == 0 || tunFd < 0) {
        napi_get_boolean(env, false, &result);
        return result;
    }
    StopBridgeInternal();
    StopTunPumpInternal();

    int dupFd = dup(tunFd);
    if (dupFd < 0) {
        OH_LOG_ERROR(LOG_APP, "bridge device dup failed errno=%{public}d", errno);
        napi_get_boolean(env, false, &result);
        return result;
    }
    g_bridgeTunFd = dupFd;
    g_bridgeSockPath.clear(); // host owns socket file; device must not unlink
    g_bridgeRunning.store(true);
    std::string pathCopy(pathBuf);
    g_bridgeThread = std::thread([pathCopy]() { BridgeDeviceLoop(pathCopy); });
    OH_LOG_INFO(LOG_APP, "bridge device start tunFd=%{public}d path=%{public}s", tunFd, pathBuf);
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value NapiStopTunBridge(napi_env env, napi_callback_info info)
{
    (void)info;
    StopBridgeInternal();
    OH_LOG_INFO(LOG_APP, "stopTunBridge");
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value NapiOhosRead(napi_env env, napi_callback_info info)
{
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    void *data = nullptr;
    size_t len = 0;
    napi_typedarray_type type;
    napi_value arraybuffer;
    size_t byteOffset = 0;
    bool isTypedArray = false;
    napi_is_typedarray(env, args[0], &isTypedArray);
    if (isTypedArray) {
        napi_get_typedarray_info(env, args[0], &type, &len, &data, &arraybuffer, &byteOffset);
    } else {
        napi_get_arraybuffer_info(env, args[0], &data, &len);
    }
    if (EnsureOpenP2PLoaded() && g_ohosRead != nullptr && data != nullptr && len > 0) {
        g_ohosRead(static_cast<unsigned char *>(data), static_cast<int>(len));
    }
    napi_value result;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value NapiOhosWrite(napi_env env, napi_callback_info info)
{
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    int32_t timeoutMs = 3000;
    int32_t maxLen = 4096;
    if (argc > 0) {
        napi_get_value_int32(env, args[0], &timeoutMs);
    }
    if (argc > 1) {
        napi_get_value_int32(env, args[1], &maxLen);
    }
    if (maxLen <= 0) {
        maxLen = 4096;
    }

    std::vector<unsigned char> buf(static_cast<size_t>(maxLen));
    int n = 0;
    if (EnsureOpenP2PLoaded() && g_ohosWrite != nullptr) {
        n = g_ohosWrite(buf.data(), maxLen, timeoutMs);
    }

    napi_value result;
    void *data = nullptr;
    size_t outLen = n > 0 ? static_cast<size_t>(n) : 0;
    napi_create_arraybuffer(env, outLen, &data, &result);
    if (n > 0 && data != nullptr) {
        memcpy(data, buf.data(), outLen);
    }
    return result;
}

static napi_value NapiNativeStatus(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result;
    if (EnsureOpenP2PLoaded()) {
        napi_create_string_utf8(env, "ok", NAPI_AUTO_LENGTH, &result);
    } else {
        napi_create_string_utf8(env, g_loadError.c_str(), NAPI_AUTO_LENGTH, &result);
    }
    return result;
}

static napi_value NapiLastError(napi_env env, napi_callback_info info)
{
    (void)info;
    napi_value result;
    if (!EnsureOpenP2PLoaded() || g_getLastError == nullptr) {
        napi_create_string_utf8(env, g_loadError.empty() ? "native not ready" : g_loadError.c_str(), NAPI_AUTO_LENGTH,
            &result);
        return result;
    }
    char *err = g_getLastError();
    if (err == nullptr) {
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &result);
        return result;
    }
    napi_create_string_utf8(env, err, NAPI_AUTO_LENGTH, &result);
    if (g_freeCString != nullptr) {
        g_freeCString(err);
    }
    return result;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports)
{
    EnsureOpenP2PLoaded();

    napi_property_descriptor desc[] = {
        {"runAsModule", nullptr, NapiRunAsModule, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getToken", nullptr, NapiGetToken, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"connect", nullptr, NapiConnect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isOnline", nullptr, NapiIsOnline, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stop", nullptr, NapiStop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getNodeName", nullptr, NapiGetNodeName, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getProbeStatus", nullptr, NapiGetProbeStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSdwanConfig", nullptr, NapiGetSdwanConfig, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getSdwanConfigAsync", nullptr, NapiGetSdwanConfigAsync, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ohosRead", nullptr, NapiOhosRead, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ohosWrite", nullptr, NapiOhosWrite, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startTunPump", nullptr, NapiStartTunPump, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopTunPump", nullptr, NapiStopTunPump, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startTunBridgeHost", nullptr, NapiStartTunBridgeHost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"startTunBridgeDevice", nullptr, NapiStartTunBridgeDevice, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"stopTunBridge", nullptr, NapiStopTunBridge, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeStatus", nullptr, NapiNativeStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"lastError", nullptr, NapiLastError, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void *)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void)
{
    napi_module_register(&demoModule);
}
