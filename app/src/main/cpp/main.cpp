#include "zygisk.hpp"
#include "dobby.h"
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <android/log.h>
#include <unistd.h>
#include <vector>
#include <fstream>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "SNFix/Zygisk", __VA_ARGS__)

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static void (*o_hook)(const prop_info *, T_Callback, void *);

static volatile T_Callback o_callback;

static void
handle_system_property(void *cookie, const char *name, const char *value, uint32_t serial) {
    if (std::string_view(name).compare("ro.product.first_api_level") == 0) {
        LOGD("Set first_api_level to 25 (Android 7.1), original value: %s", value);
        value = "25";
    }
    o_callback(cookie, name, value, serial);
}

static void my_hook(const prop_info *pi, T_Callback callback, void *cookie) {
    o_callback = callback;
    o_hook(pi, handle_system_property, cookie);
}

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        bool isGms = std::string_view(rawProcess).starts_with("com.google.android.gms");
        bool isGmsUnstable =
                std::string_view(rawProcess).compare("com.google.android.gms.unstable") == 0;
        env->ReleaseStringUTFChars(args->nice_name, rawProcess);

        if (!isGms) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        if (isGmsUnstable) {
            int fd = api->connectCompanion();
            int size;
            if (recv(fd, &size, sizeof(size), 0) < 1) {
                close(fd);
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                LOGD("Error recv size");
                return;
            }
            dexFile.resize(size);
            if (recv(fd, dexFile.data(), dexFile.size(), 0) < 1) {
                close(fd);
                dexFile.clear();
                dexFile.shrink_to_fit();
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                LOGD("Error recv .dex data");
                return;
            }
            close(fd);
        } else {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        if (dexFile.empty()) return;

        LOGD("Dex file size: %d", static_cast<int>(dexFile.size()));

        void *handle = DobbySymbolResolver(nullptr, "__system_property_read_callback");

        if (handle == nullptr) {
            LOGD("Couldn't get __system_property_read_callback handle, REPORT TO @chiteroman at XDA");
            LOGD("Module will continue injecting .dex file, but it can't ensure to pass DEVICE veredict");
        } else {
            LOGD("Got __system_property_read_callback handle at %p", handle);
            DobbyHook(handle, (dobby_dummy_func_t) my_hook, (dobby_dummy_func_t *) &o_hook);
        }

        LOGD("getSystemClassLoader");
        auto clClass = env->FindClass("java/lang/ClassLoader");
        auto getSystemClassLoader = env->GetStaticMethodID(clClass, "getSystemClassLoader",
                                                           "()Ljava/lang/ClassLoader;");
        auto systemClassLoader = env->CallStaticObjectMethod(clClass, getSystemClassLoader);

        LOGD("InMemoryDexClassLoader");
        auto dexClClass = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        auto buffer = env->NewDirectByteBuffer(dexFile.data(), static_cast<jlong>(dexFile.size()));
        auto dexClInit = env->GetMethodID(dexClClass, "<init>",
                                          "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        auto dexCl = env->NewObject(dexClClass, dexClInit, buffer, systemClassLoader);

        LOGD("loadClass");
        auto loadClass = env->GetMethodID(clClass, "loadClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;");
        auto entryClassName = env->NewStringUTF("es.chiteroman.playintegrityfix.EntryPoint");
        auto entryClassObj = env->CallObjectMethod(dexCl, loadClass, entryClassName);

        LOGD("init");
        auto entryClass = (jclass) entryClassObj;
        auto entryInit = env->GetStaticMethodID(entryClass, "init", "()V");
        env->CallStaticVoidMethod(entryClass, entryInit);

        LOGD("clean");
        dexFile.clear();
        dexFile.shrink_to_fit();
        env->DeleteLocalRef(entryClassName);
        env->DeleteLocalRef(buffer);
        env->DeleteLocalRef(dexCl);
        env->DeleteLocalRef(entryClassObj);
        env->DeleteLocalRef(dexClClass);
        env->DeleteLocalRef(clClass);
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::vector<char> dexFile;
};

static void companion(int fd) {
    std::ifstream ifs("/data/adb/modules/playintegrityfix/classes.dex",
                      std::ios::binary | std::ios::ate);

    int size = static_cast<int>(ifs.tellg());
    send(fd, &size, sizeof(size), 0);

    ifs.seekg(std::ios::beg);

    std::vector<char> dexFile(size);
    ifs.read(dexFile.data(), size);
    ifs.close();

    send(fd, dexFile.data(), dexFile.size(), 0);

    dexFile.clear();
    dexFile.shrink_to_fit();
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)