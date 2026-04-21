#define LOG_TAG "ZZKDEBUG"
#include <aidl/android/hardware/biometrics/fingerprint/ISession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <unistd.h>

using aidl::android::hardware::biometrics::fingerprint::ISession;
using aidl::android::hardware::biometrics::fingerprint::ISessionCallback;

static const char* kDescSession = "android.hardware.biometrics.fingerprint.ISession";
static const char* kDefaultNode = "/sys/kernel/oplus_display/notify_fppress";
static const char* kFpTouchStateNode = "/sys/kernel/oplus_display/fp_touch_state";

__attribute__((constructor)) static void zzk_ctor() {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "FppressShim loaded (pid=%d)", getpid());
}

static AIBinder_Class_onTransact gSessionOnTransactOrig = nullptr;
static bool gPressed = false;

struct WriteCapture {
    AParcel* inParcel;
    int32_t vals[32];
    int count;
};
static thread_local WriteCapture gTL = {nullptr, {0}, 0};

static int gVendorDown = 22;
static int gVendorUp = 23;

static int gSessionDown = 14;
static int gSessionUp = 22;

static int gCbEnable = 1;      // 0 关闭；1 开启
static int gCbCode = 1001;     // 自定义回调 code
static int gCbDownVal = 1201;  // 1001 的第一个 int 值表示 down
static int gCbUpVal = 1202;    // 1001 的第一个 int 值表示 up
static int gCbUseDown = 1;     // 0 不用 1001 判定 down，1 使用

static void loadVendorCodesOnce() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.vendor.fod.down_code", buf) > 0) gVendorDown = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.up_code", buf) > 0) gVendorUp = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.session_down_code", buf) > 0)
        gSessionDown = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.session_up_code", buf) > 0)
        gSessionUp = atoi(buf);

    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.cb_enable", buf) > 0) gCbEnable = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.cb_code", buf) > 0) gCbCode = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.cb_down_val", buf) > 0) gCbDownVal = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.cb_up_val", buf) > 0) gCbUpVal = atoi(buf);
    memset(buf, 0, sizeof(buf));
    if (__system_property_get("persist.vendor.fod.cb_use_down", buf) > 0) gCbUseDown = atoi(buf);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "[ZZKDEBUG]codes: vendorDown=%d vendorUp=%d sessionDown=%d sessionUp=%d "
                        "cb{enable=%d code=%d downVal=%d upVal=%d useDown=%d}",
                        gVendorDown, gVendorUp, gSessionDown, gSessionUp, gCbEnable, gCbCode,
                        gCbDownVal, gCbUpVal, gCbUseDown);
}

static const char* getNodePath() {
    static char path[PROP_VALUE_MAX] = {0};
    if (path[0] == '\0') {
        char prop[PROP_VALUE_MAX] = {0};
        int len = __system_property_get("persist.vendor.fod.notify_node", prop);
        if (len > 0) {
            strncpy(path, prop, sizeof(path) - 1);
        } else {
            strncpy(path, kDefaultNode, sizeof(path) - 1);
        }
    }
    return path;
}

static bool isFpTouchStateZero() {
    int fd = open(kFpTouchStateNode, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buf[2] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) {
        return false;
    }
    return buf[0] == '0';
}

static void writeNode(const char* val) {
    const char* node = getNodePath();
    if (isFpTouchStateZero()) {
        val = "0";
    }
    int fd = open(node, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "open %s failed: %s", node, strerror(errno));
        return;
    }
    ssize_t want = (ssize_t)strlen(val);
    ssize_t r = write(fd, val, want);
    if (r != want) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "write %s failed (%zd/%zd): %s", node, r,
                            want, strerror(errno));
    } else {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "write %s -> %s (pressed=%d)", node, val,
                            gPressed ? 1 : 0);
    }
    close(fd);
}

static void dumpCapturedInts(const char* where, transaction_code_t code, AParcel** in) {
    if (gTL.inParcel && in && *in == gTL.inParcel && gTL.count > 0) {
        char buf[256];
        int n = 0;
        n += snprintf(buf + n, sizeof(buf) - n, "ints[%d]:", gTL.count);
        int limit = gTL.count < 16 ? gTL.count : 16;
        for (int i = 0; i < limit && n < (int)sizeof(buf); ++i) {
            n += snprintf(buf + n, sizeof(buf) - n, " %d", gTL.vals[i]);
        }
        if (gTL.count > limit) {
            snprintf(buf + n, sizeof(buf) - n, " ...");
        }
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ZZKDEBUG]%s code=%d captured %s", where,
                            (int)code, buf);
    } else {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[ZZKDEBUG]%s code=%d no captured ints (count=%d, match=%d)", where,
                            (int)code, gTL.count, (int)(gTL.inParcel && in && *in == gTL.inParcel));
    }
}

static const char* nameOfSessionCode(transaction_code_t code) {
    if (code == ISession::TRANSACTION_onPointerDown) return "ISession.onPointerDown";
    if (code == ISession::TRANSACTION_onPointerUp) return "ISession.onPointerUp";
    return "ISession.Unknown";
}

static const char* nameOfCallbackCode(transaction_code_t code) {
    if (code == ISessionCallback::TRANSACTION_onAcquired) return "ISessionCallback.onAcquired";
    if (code == ISessionCallback::TRANSACTION_onError) return "ISessionCallback.onError";
    return "ISessionCallback.Unknown";
}

static binder_status_t Session_onTransact_hook(AIBinder* binder, transaction_code_t code,
                                               const AParcel* in, AParcel* out) {
    const char* name = nameOfSessionCode(code);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ZZKDEBUG]ISession onTransact code=%d (%s)",
                        (int)code, name);

    if (gSessionDown >= 0 && code == (transaction_code_t)gSessionDown) {
        if (!gPressed) {
            gPressed = true;
            writeNode("1");
        }
    } else if (gSessionUp >= 0 && code == (transaction_code_t)gSessionUp) {
        if (gPressed) {
            gPressed = false;
            writeNode("0");
        }
    } else {
        if (code == ISession::TRANSACTION_onPointerDown) {
            if (!gPressed) {
                gPressed = true;
                writeNode("1");
            }
        } else if (code == ISession::TRANSACTION_onPointerUp) {
            if (gPressed) {
                gPressed = false;
                writeNode("0");
            }
        }
    }
    return gSessionOnTransactOrig ? gSessionOnTransactOrig(binder, code, in, out) : STATUS_OK;
}

extern "C" binder_status_t AIBinder_prepareTransaction(AIBinder* binder, AParcel** in) {
    using PrepareFn = binder_status_t (*)(AIBinder*, AParcel**);
    static auto orig = (PrepareFn)dlsym(RTLD_NEXT, "AIBinder_prepareTransaction");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    binder_status_t st = orig(binder, in);
    gTL.inParcel = (in ? *in : nullptr);
    gTL.count = 0;
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ZZKDEBUG]AIBinder_prepareTransaction in=%p",
                        (void*)gTL.inParcel);
    return st;
}

extern "C" binder_status_t AParcel_writeInt32(AParcel* parcel, int32_t value) {
    using WriteIntFn = binder_status_t (*)(AParcel*, int32_t);
    static auto orig = (WriteIntFn)dlsym(RTLD_NEXT, "AParcel_writeInt32");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    if (parcel && parcel == gTL.inParcel) {
        if (gTL.count < (int)(sizeof(gTL.vals) / sizeof(gTL.vals[0]))) {
            gTL.vals[gTL.count++] = value;
        }
    }
    return orig(parcel, value);
}

extern "C" binder_status_t AIBinder_transact(AIBinder* binder, transaction_code_t code,
                                             AParcel** in, AParcel** out, binder_flags_t flags) {
    using TransactFn = binder_status_t (*)(AIBinder*, transaction_code_t, AParcel**, AParcel**,
                                           binder_flags_t);
    static auto orig = (TransactFn)dlsym(RTLD_NEXT, "AIBinder_transact");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    loadVendorCodesOnce();

    const char* cbName = nameOfCallbackCode(code);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "[ZZKDEBUG]AIBinder_transact code=%d (%s) flags=0x%x", (int)code, cbName,
                        (unsigned int)flags);

    dumpCapturedInts("transact", code, in);

    if (code == ISessionCallback::TRANSACTION_onAcquired) {
        bool matched = false;

        if (gTL.inParcel && in && *in == gTL.inParcel && gTL.count > 0) {
            for (int i = 0; i < gTL.count; ++i) {
                if (gTL.vals[i] == gVendorDown) {
                    matched = true;
                    if (!gPressed) {
                        gPressed = true;
                        writeNode("1");
                    }
                    __android_log_print(
                            ANDROID_LOG_INFO, LOG_TAG,
                            "[ZZKDEBUG]onAcquired: matched vendorDown at idx=%d (val=%d)", i,
                            gTL.vals[i]);
                    break;
                } else if (gTL.vals[i] == gVendorUp) {
                    matched = true;
                    if (gPressed) {
                        gPressed = false;
                        writeNode("0");
                    }
                    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                        "[ZZKDEBUG]onAcquired: matched vendorUp at idx=%d (val=%d)",
                                        i, gTL.vals[i]);
                    break;
                }
            }

            if (!matched && gTL.count >= 2) {
                int32_t acquired = gTL.vals[0];
                int32_t vendor = gTL.vals[1];
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                    "[ZZKDEBUG]onAcquired first-two: acquired=%d vendor=%d",
                                    acquired, vendor);
                if (vendor == gVendorDown) {
                    matched = true;
                    if (!gPressed) {
                        gPressed = true;
                        writeNode("1");
                    }
                } else if (vendor == gVendorUp) {
                    matched = true;
                    if (gPressed) {
                        gPressed = false;
                        writeNode("0");
                    }
                }
            }
        }

        if (!matched) {
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "[ZZKDEBUG]onAcquired: no vendor match (down=%d up=%d)",
                                gVendorDown, gVendorUp);
        }
    }

    if (gCbEnable && code == (transaction_code_t)gCbCode) {
        int event = (gTL.count > 0) ? gTL.vals[0] : -1;
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[ZZKDEBUG]cb%u event=%d (downVal=%d upVal=%d, pressed=%d)",
                            (unsigned)gCbCode, event, gCbDownVal, gCbUpVal, gPressed ? 1 : 0);

        if (gCbUseDown && event == gCbDownVal && !gPressed) {
            gPressed = true;
            writeNode("1");
        }
        if (event == gCbUpVal && gPressed) {
            gPressed = false;
            writeNode("0");
        }
    }

    binder_status_t ret = orig(binder, code, in, out, flags);

    gTL.inParcel = nullptr;
    gTL.count = 0;

    return ret;
}

extern "C" AIBinder_Class* AIBinder_Class_define(const char* descriptor,
                                                 AIBinder_Class_onCreate onCreate,
                                                 AIBinder_Class_onDestroy onDestroy,
                                                 AIBinder_Class_onTransact onTransact) {
    using DefineFn = AIBinder_Class* (*)(const char*, AIBinder_Class_onCreate,
                                         AIBinder_Class_onDestroy, AIBinder_Class_onTransact);
    static auto orig_define = (DefineFn)dlsym(RTLD_NEXT, "AIBinder_Class_define");
    if (!orig_define) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "[ZZKDEBUG]dlsym AIBinder_Class_define failed");
        return nullptr;
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ZZKDEBUG]AIBinder_Class_define: %s",
                        descriptor ? descriptor : "(null)");

    if (descriptor && strcmp(descriptor, kDescSession) == 0) {
        gSessionOnTransactOrig = onTransact;
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "[ZZKDEBUG][Hook] ISession onTransact");
        return orig_define(descriptor, onCreate, onDestroy, &Session_onTransact_hook);
    }

    return orig_define(descriptor, onCreate, onDestroy, onTransact);
}
