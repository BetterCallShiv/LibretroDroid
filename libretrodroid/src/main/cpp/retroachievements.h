#ifndef LIBRETRODROID_RETROACHIEVEMENTS_H
#define LIBRETRODROID_RETROACHIEVEMENTS_H

#include <jni.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

extern "C" {
#include "rc_client.h"
#include "client/rc_client_internal.h"
#include "rc_consoles.h"
#include "rc_hash.h"
#include "libretro/rc_libretro.h"
}

namespace libretrodroid {

class RetroAchievementsManager {
public:
    static RetroAchievementsManager& getInstance() {
        static RetroAchievementsManager instance;
        return instance;
    }

    RetroAchievementsManager(const RetroAchievementsManager&) = delete;
    RetroAchievementsManager& operator=(const RetroAchievementsManager&) = delete;

    void init(JavaVM* vm, jobject listener, const std::string& username, const std::string& token, bool hardcore);
    void login(const std::string& username, const std::string& passwordOrToken, bool isPassword);
    void loadGame(uint32_t consoleId, const std::string& hash, const std::string& gamePath, const uint8_t* buffer = nullptr, size_t bufferSize = 0);
    void onGameLoadedInCore();
    void doFrame();
    void idle();
    void reset();
    void destroy();

    void sendServerResponse(int64_t requestId, int httpStatusCode, const std::string& responseBody);

    // Callbacks used by rc_client
    uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes);
    void handleServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callbackData);
    void handleEvent(const rc_client_event_t* event);

    void notifyLoginSuccess(const std::string& username, const std::string& token);
    void notifyLoginFailure(const std::string& error);
    void notifyGameLoaded(uint32_t gameId, const std::string& title,
                          uint32_t unlockedAchievements, uint32_t totalAchievements,
                          uint32_t unlockedPoints, uint32_t totalPoints,
                          const std::string& badgeUrl);
    void notifyGameLoadFailed(const std::string& error);

    bool isInitialized() const { return m_initialized; }

private:
    RetroAchievementsManager();
    ~RetroAchievementsManager();

    void initMemory(uint32_t consoleId);
    JNIEnv* getEnv();

    struct PendingRequest {
        rc_client_server_callback_t callback;
        void* callbackData;
    };

    JavaVM* m_jvm = nullptr;
    jobject m_listener = nullptr;
    rc_client_t* m_client = nullptr;
    rc_libretro_memory_regions_t m_memory_regions{};

    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    uint32_t m_console_id = 0;
    int64_t m_next_request_id = 0;
    std::unordered_map<int64_t, PendingRequest> m_pending_requests;
};

} // namespace libretrodroid

#endif // LIBRETRODROID_RETROACHIEVEMENTS_H
