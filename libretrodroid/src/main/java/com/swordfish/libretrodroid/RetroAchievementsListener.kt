package com.swordfish.libretrodroid

interface RetroAchievementsListener {
    fun onLoginSuccess(username: String, token: String)
    fun onLoginFailure(error: String)
    fun onGameLoaded(
        gameId: Int,
        title: String,
        unlockedAchievements: Int,
        totalAchievements: Int,
        unlockedPoints: Int,
        totalPoints: Int,
        badgeUrl: String,
    )
    fun onGameLoadFailed(error: String)
    fun onAchievementUnlocked(
        id: Int,
        title: String,
        description: String,
        points: Int,
        badgeName: String,
        badgeUrl: String,
    )
    fun onAchievementProgress(
        id: Int,
        title: String,
        progress: String,
        percent: Float,
    )
    fun onGameMastered()
    fun onServerRequest(
        url: String,
        postData: String?,
        contentType: String?,
        requestId: Long,
    )
}
