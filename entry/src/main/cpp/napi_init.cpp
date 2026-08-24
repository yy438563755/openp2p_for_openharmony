#include "napi/native_api.h"
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <string>
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
        {"getSdwanConfig", nullptr, NapiGetSdwanConfig, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ohosRead", nullptr, NapiOhosRead, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"ohosWrite", nullptr, NapiOhosWrite, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeStatus", nullptr, NapiNativeStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
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
