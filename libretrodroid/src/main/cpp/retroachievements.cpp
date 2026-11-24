#include "retroachievements.h"
#include "log.h"
#include "libretrodroid.h"
#include "environment.h"

#include <cstring>

namespace libretrodroid {

static uint32_t rc_client_read_memory_bridge(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client) {
    auto* self = (RetroAchievementsManager*) rc_client_get_userdata(client);
    if (!self) return 0;
    return self->readMemory(address, buffer, num_bytes);
}

static void rc_client_server_call_bridge(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client) {
    auto* self = (RetroAchievementsManager*) rc_client_get_userdata(client);
    if (!self) return;
    self->handleServerCall(request, callback, callback_data);
}

static void rc_client_event_handler_bridge(const rc_client_event_t* event, rc_client_t* client) {
    auto* self = (RetroAchievementsManager*) rc_client_get_userdata(client);
    if (!self) return;
    self->handleEvent(event);
}

static void rc_client_login_callback(int result, const char* error_message, rc_client_t* client, void* userdata) {
    auto* self = (RetroAchievementsManager*) userdata;
    if (!self) return;
    if (result == RC_OK) {
        const auto* user = rc_client_get_user_info(client);
        std::string uname = (user && user->display_name) ? user->display_name : "";
        std::string token = (user && user->token) ? user->token : "";
        LOGI("RetroAchievements login successful: %s", uname.c_str());
        self->notifyLoginSuccess(uname, token);
    } else {
        LOGE("RetroAchievements login error (%d): %s", result, error_message ? error_message : "Unknown");
        self->notifyLoginFailure(error_message ? error_message : "Login failed");
    }
}

static void rc_client_load_game_callback(int result, const char* error_message, rc_client_t* client, void* userdata) {
    auto* self = (RetroAchievementsManager*) userdata;
    if (!self) return;
    if (result == RC_OK) {
        const auto* game = rc_client_get_game_info(client);
        rc_client_user_game_summary_t summary{};
        rc_client_get_user_game_summary(client, &summary);
        uint32_t gid = game ? game->id : 0;
        std::string title = (game && game->title) ? game->title : "";

        char imgUrl[256] = {0};
        rc_client_game_get_image_url(game, imgUrl, sizeof(imgUrl));
        std::string badgeUrl = imgUrl;
        if (badgeUrl.empty() && game && game->badge_name && game->badge_name[0] != '\0') {
            badgeUrl = std::string("https://media.retroachievements.org/Images/") + game->badge_name + ".png";
        }

        LOGI("RetroAchievements game loaded: %s (ID %u, %u promoted, %u unlocked)",
             title.c_str(), gid, summary.num_promoted_achievements, summary.num_unlocked_achievements);
        self->notifyGameLoaded(gid, title, summary.num_unlocked_achievements, summary.num_promoted_achievements,
                               summary.points_unlocked, summary.points_available, badgeUrl);
    } else {
        LOGE("RetroAchievements load game error (%d): %s", result, error_message ? error_message : "Unknown");
        self->notifyGameLoadFailed(error_message ? error_message : "Load game failed");
    }
}

RetroAchievementsManager::RetroAchievementsManager() {
    memset(&m_memory_regions, 0, sizeof(m_memory_regions));
}

RetroAchievementsManager::~RetroAchievementsManager() {
    destroy();
}

JNIEnv* RetroAchievementsManager::getEnv() {
    if (!m_jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint res = m_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (m_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
    }
    return env;
}

void RetroAchievementsManager::init(JavaVM* vm, jobject listener, const std::string& username, const std::string& token, bool hardcore) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    destroy();

    m_jvm = vm;
    JNIEnv* env = getEnv();
    if (env && listener) {
        m_listener = env->NewGlobalRef(listener);
    }

    m_client = rc_client_create(&rc_client_read_memory_bridge, &rc_client_server_call_bridge);
    if (!m_client) {
        LOGE("Failed to create rc_client_t");
        return;
    }

    rc_client_set_userdata(m_client, this);
    rc_client_set_event_handler(m_client, &rc_client_event_handler_bridge);
    rc_client_set_hardcore_enabled(m_client, hardcore ? 1 : 0);

    m_initialized = true;

    if (!username.empty() && !token.empty()) {
        m_client->user.username = rc_buffer_strcpy(&m_client->state.buffer, username.c_str());
        m_client->user.display_name = m_client->user.username;
        m_client->user.token = rc_buffer_strcpy(&m_client->state.buffer, token.c_str());
        rc_mutex_lock(&m_client->state.mutex);
        m_client->state.user = RC_CLIENT_USER_STATE_LOGGED_IN;
        rc_mutex_unlock(&m_client->state.mutex);
        LOGI("RetroAchievements session pre-authenticated directly for %s (skipped network login call!)", username.c_str());
    }
}

void RetroAchievementsManager::login(const std::string& username, const std::string& passwordOrToken, bool isPassword) {
    if (!m_client) {
        LOGE("Cannot login: m_client is null");
        return;
    }
    if (username.empty() || passwordOrToken.empty()) {
        LOGW("Cannot login: username or credential empty");
        return;
    }

    if (isPassword) {
        LOGI("Beginning RetroAchievements login with password for %s", username.c_str());
        rc_client_begin_login_with_password(m_client, username.c_str(), passwordOrToken.c_str(), &rc_client_login_callback, this);
    } else {
        LOGI("Beginning RetroAchievements login with token for %s", username.c_str());
        rc_client_begin_login_with_token(m_client, username.c_str(), passwordOrToken.c_str(), &rc_client_login_callback, this);
    }
}

void RetroAchievementsManager::initMemory(uint32_t consoleId) {
    rc_libretro_memory_destroy(&m_memory_regions);

    auto get_core_memory = [](uint32_t id, rc_libretro_core_memory_info_t* info) {
        auto& instance = LibretroDroid::getInstance();
        info->data = (uint8_t*) instance.getCoreMemoryData(id);
        info->size = instance.getCoreMemorySize(id);
    };

    const struct retro_memory_map* mmap = Environment::getInstance().getMemoryMap();
    int res = rc_libretro_memory_init(&m_memory_regions, mmap, get_core_memory, consoleId);
    LOGI("RetroAchievements rc_libretro_memory_init: res=%d, regions=%u, total_size=%zu",
         res, m_memory_regions.count, m_memory_regions.total_size);
}

std::string RetroAchievementsManager::generateHash(uint32_t consoleId, const std::string& gamePath, const uint8_t* buffer, size_t bufferSize) {
    char generatedHash[33] = {0};
    int hashSuccess = 0;
    if (buffer && bufferSize > 0) {
        hashSuccess = rc_hash_generate_from_buffer(generatedHash, consoleId, buffer, bufferSize);
    } else if (!gamePath.empty()) {
        hashSuccess = rc_hash_generate_from_file(generatedHash, consoleId, gamePath.c_str());
    }

    if (hashSuccess && generatedHash[0] != '\0') {
        LOGI("Generated RA hash: %s for consoleId: %u", generatedHash, consoleId);
        return std::string(generatedHash);
    }
    return "";
}

void RetroAchievementsManager::loadGame(uint32_t consoleId, const std::string& hash, const std::string& gamePath, const uint8_t* buffer, size_t bufferSize) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (!m_client) {
        LOGW("loadGame called but m_client is null");
        return;
    }

    m_console_id = consoleId;

    std::string finalHash = hash;
    if (finalHash.empty()) {
        char generatedHash[33] = {0};
        int hashSuccess = 0;
        if (buffer && bufferSize > 0) {
            hashSuccess = rc_hash_generate_from_buffer(generatedHash, consoleId, buffer, bufferSize);
        } else if (!gamePath.empty()) {
            hashSuccess = rc_hash_generate_from_file(generatedHash, consoleId, gamePath.c_str());
        }

        if (hashSuccess && generatedHash[0] != '\0') {
            finalHash = generatedHash;
            LOGI("Generated RA hash: %s for consoleId: %u", finalHash.c_str(), consoleId);
        }
    }

    if (!finalHash.empty()) {
        LOGI("Starting rc_client_begin_load_game with hash: %s", finalHash.c_str());
        rc_client_begin_load_game(m_client, finalHash.c_str(), &rc_client_load_game_callback, this);
    } else {
        LOGW("Cannot identify game hash for RetroAchievements (consoleId=%u, path=%s)", consoleId, gamePath.c_str());
    }
}

void RetroAchievementsManager::onGameLoadedInCore() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_console_id != 0) {
        initMemory(m_console_id);
    }
}

void RetroAchievementsManager::doFrame() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_client && rc_client_is_game_loaded(m_client)) {
        if (m_memory_regions.count == 0 && m_console_id != 0) {
            initMemory(m_console_id);
        }
        rc_client_do_frame(m_client);
    }
}

void RetroAchievementsManager::idle() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_client) {
        rc_client_idle(m_client);
    }
}

void RetroAchievementsManager::reset() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_client) {
        rc_client_reset(m_client);
    }
}

void RetroAchievementsManager::destroy() {
    if (m_client) {
        rc_client_destroy(m_client);
        m_client = nullptr;
    }

    rc_libretro_memory_destroy(&m_memory_regions);

    JNIEnv* env = getEnv();
    if (env && m_listener) {
        env->DeleteGlobalRef(m_listener);
        m_listener = nullptr;
    }

    m_pending_requests.clear();
    m_initialized = false;
    m_console_id = 0;
}

uint32_t RetroAchievementsManager::readMemory(uint32_t address, uint8_t* buffer, uint32_t numBytes) {
    if (m_memory_regions.count == 0) return 0;
    return rc_libretro_memory_read(&m_memory_regions, address, buffer, numBytes);
}

void RetroAchievementsManager::handleServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callbackData) {
    int64_t req_id = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        req_id = ++m_next_request_id;
        m_pending_requests[req_id] = { callback, callbackData };
    }

    JNIEnv* env = getEnv();
    if (!env || !m_listener) {
        LOGE("Cannot dispatch server call: env=%p, listener=%p", env, m_listener);
        return;
    }

    jstring jUrl = env->NewStringUTF(request->url ? request->url : "");
    jstring jPostData = request->post_data ? env->NewStringUTF(request->post_data) : nullptr;
    jstring jContentType = request->content_type ? env->NewStringUTF(request->content_type) : nullptr;

    jclass listenerClass = env->GetObjectClass(m_listener);
    jmethodID method = env->GetMethodID(listenerClass, "onServerRequest", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V");
    if (method) {
        env->CallVoidMethod(m_listener, method, jUrl, jPostData, jContentType, (jlong) req_id);
    } else {
        LOGE("Failed to find onServerRequest method on listener");
    }

    env->DeleteLocalRef(jUrl);
    if (jPostData) env->DeleteLocalRef(jPostData);
    if (jContentType) env->DeleteLocalRef(jContentType);
}

void RetroAchievementsManager::sendServerResponse(int64_t requestId, int httpStatusCode, const std::string& responseBody) {
    PendingRequest req{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_pending_requests.find(requestId);
        if (it == m_pending_requests.end()) {
            LOGW("sendServerResponse: request ID %lld not found", (long long) requestId);
            return;
        }
        req = it->second;
        m_pending_requests.erase(it);
    }

    rc_api_server_response_t server_response;
    memset(&server_response, 0, sizeof(server_response));
    server_response.body = responseBody.data();
    server_response.body_length = responseBody.size();
    server_response.http_status_code = httpStatusCode;

    if (req.callback) {
        req.callback(&server_response, req.callbackData);
    }
}

void RetroAchievementsManager::handleEvent(const rc_client_event_t* event) {
    if (!event) return;

    if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED && event->achievement) {
        const auto* ach = event->achievement;
        LOGI("Achievement Triggered! ID=%u, Title=%s, Points=%u, Badge=%s",
             ach->id, ach->title ? ach->title : "", ach->points, ach->badge_name);

        JNIEnv* env = getEnv();
        if (!env || !m_listener) return;

        jstring jTitle = env->NewStringUTF(ach->title ? ach->title : "");
        jstring jDesc = env->NewStringUTF(ach->description ? ach->description : "");
        jstring jBadgeName = env->NewStringUTF(ach->badge_name);
        jstring jBadgeUrl = env->NewStringUTF(ach->badge_url ? ach->badge_url : "");

        jclass listenerClass = env->GetObjectClass(m_listener);
        jmethodID method = env->GetMethodID(listenerClass, "onAchievementUnlocked",
                                            "(ILjava/lang/String;Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;)V");
        if (method) {
            env->CallVoidMethod(m_listener, method,
                                (jint) ach->id,
                                jTitle, jDesc,
                                (jint) ach->points,
                                jBadgeName, jBadgeUrl);
        }

        env->DeleteLocalRef(jTitle);
        env->DeleteLocalRef(jDesc);
        env->DeleteLocalRef(jBadgeName);
        env->DeleteLocalRef(jBadgeUrl);
    } else if (event->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE && event->achievement) {
        const auto* ach = event->achievement;
        JNIEnv* env = getEnv();
        if (!env || !m_listener) return;

        jstring jTitle = env->NewStringUTF(ach->title ? ach->title : "");
        jstring jProgress = env->NewStringUTF(ach->measured_progress);

        jclass listenerClass = env->GetObjectClass(m_listener);
        jmethodID method = env->GetMethodID(listenerClass, "onAchievementProgress",
                                            "(ILjava/lang/String;Ljava/lang/String;F)V");
        if (method) {
            env->CallVoidMethod(m_listener, method,
                                (jint) ach->id,
                                jTitle, jProgress,
                                (jfloat) ach->measured_percent);
        }

        env->DeleteLocalRef(jTitle);
        env->DeleteLocalRef(jProgress);
    } else if (event->type == RC_CLIENT_EVENT_GAME_COMPLETED) {
        LOGI("RetroAchievements: Game mastered!");
        JNIEnv* env = getEnv();
        if (!env || !m_listener) return;
        jclass listenerClass = env->GetObjectClass(m_listener);
        jmethodID method = env->GetMethodID(listenerClass, "onGameMastered", "()V");
        if (method) {
            env->CallVoidMethod(m_listener, method);
        }
    }
}

void RetroAchievementsManager::notifyLoginSuccess(const std::string& username, const std::string& token) {
    JNIEnv* env = getEnv();
    if (!env || !m_listener) return;
    jstring jUname = env->NewStringUTF(username.c_str());
    jstring jToken = env->NewStringUTF(token.c_str());
    jclass listenerClass = env->GetObjectClass(m_listener);
    jmethodID method = env->GetMethodID(listenerClass, "onLoginSuccess", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (method) {
        env->CallVoidMethod(m_listener, method, jUname, jToken);
    }
    env->DeleteLocalRef(jUname);
    env->DeleteLocalRef(jToken);
}

void RetroAchievementsManager::notifyLoginFailure(const std::string& error) {
    JNIEnv* env = getEnv();
    if (!env || !m_listener) return;
    jstring jErr = env->NewStringUTF(error.c_str());
    jclass listenerClass = env->GetObjectClass(m_listener);
    jmethodID method = env->GetMethodID(listenerClass, "onLoginFailure", "(Ljava/lang/String;)V");
    if (method) {
        env->CallVoidMethod(m_listener, method, jErr);
    }
    env->DeleteLocalRef(jErr);
}

void RetroAchievementsManager::notifyGameLoaded(uint32_t gameId, const std::string& title,
                                                uint32_t unlockedAchievements, uint32_t totalAchievements,
                                                uint32_t unlockedPoints, uint32_t totalPoints,
                                                const std::string& badgeUrl) {
    JNIEnv* env = getEnv();
    if (!env || !m_listener) return;
    jstring jTitle = env->NewStringUTF(title.c_str());
    jstring jBadgeUrl = env->NewStringUTF(badgeUrl.c_str());
    jclass listenerClass = env->GetObjectClass(m_listener);
    jmethodID method = env->GetMethodID(listenerClass, "onGameLoaded", "(ILjava/lang/String;IIIILjava/lang/String;)V");
    if (method) {
        env->CallVoidMethod(m_listener, method,
                            (jint) gameId,
                            jTitle,
                            (jint) unlockedAchievements,
                            (jint) totalAchievements,
                            (jint) unlockedPoints,
                            (jint) totalPoints,
                            jBadgeUrl);
    }
    env->DeleteLocalRef(jTitle);
    env->DeleteLocalRef(jBadgeUrl);
}

void RetroAchievementsManager::notifyGameLoadFailed(const std::string& error) {
    JNIEnv* env = getEnv();
    if (!env || !m_listener) return;
    jstring jErr = env->NewStringUTF(error.c_str());
    jclass listenerClass = env->GetObjectClass(m_listener);
    jmethodID method = env->GetMethodID(listenerClass, "onGameLoadFailed", "(Ljava/lang/String;)V");
    if (method) {
        env->CallVoidMethod(m_listener, method, jErr);
    }
    env->DeleteLocalRef(jErr);
}

} // namespace libretrodroid
