// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject

/**
 * OpenPak, as the app sees it.
 *
 * Being online is not something anybody should have to ask for: [start] runs at launch so the
 * account is online, visible to friends, and hearing about invitations before any screen is
 * opened. Everything below crosses into the shared client, so every call here is off the main
 * thread -- the sign-in chain is five requests to a server that may not be there.
 */
object OpenPak {
    /** Who is signed in, as the last sign-in left it. */
    data class Status(
        val enabled: Boolean,
        val signedIn: Boolean,
        val linked: Boolean,
        val nickname: String,
        val friendCode: String,
        val server: String
    )

    /** An invitation waiting in the native inbox -- one a console sent, most likely. */
    data class Invitation(
        val id: String,
        val from: String,
        val titleId: String,
        val expiresAt: Long
    )

    private external fun nativeStart(): Boolean
    private external fun nativeSignIn(email: String, password: String): String
    private external fun nativeStatus(): String
    private external fun nativeInvitations(): String
    private external fun nativeRefreshInvitations(): Boolean
    private external fun nativeDismissInvitation(id: String)

    /** Sign in at launch. Fire and forget: a server that is not there is an app that still opens. */
    fun start() {
        CoroutineScope(Dispatchers.IO).launch {
            val signedIn = nativeStart()
            Log.info("[OpenPak] start(): ${if (signedIn) "signed in" else "not signed in"}")
        }
    }

    /** Bind this install to a person. Empty means it worked; anything else is worth showing them. */
    suspend fun signIn(email: String, password: String): String = withContext(Dispatchers.IO) {
        nativeSignIn(email, password)
    }

    suspend fun status(): Status = withContext(Dispatchers.IO) {
        val json = JSONObject(nativeStatus())
        Status(
            enabled = json.optBoolean("enabled"),
            signedIn = json.optBoolean("signed_in"),
            linked = json.optBoolean("linked"),
            nickname = json.optString("nickname"),
            friendCode = json.optString("friend_code"),
            server = json.optString("server")
        )
    }

    /** What is waiting, as of the last poll. [refresh] asks now instead. */
    suspend fun invitations(): List<Invitation> = withContext(Dispatchers.IO) {
        val array = JSONArray(nativeInvitations())

        (0 until array.length()).map { index ->
            val item = array.getJSONObject(index)
            Invitation(
                id = item.optString("id"),
                from = item.optString("from"),
                titleId = item.optString("title_id"),
                expiresAt = item.optLong("expires_at")
            )
        }
    }

    suspend fun refresh(): Boolean = withContext(Dispatchers.IO) { nativeRefreshInvitations() }

    suspend fun dismiss(id: String) = withContext(Dispatchers.IO) { nativeDismissInvitation(id) }
}
