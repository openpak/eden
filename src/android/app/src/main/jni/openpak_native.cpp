// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

// The OpenPak surface the Android app calls into.
//
// Everything the desktop build gets from the Qt dialog -- signing in, being online, hearing about
// an invitation -- lives in openpak::client, which has no UI of its own. This is the whole bridge:
// four calls in, JSON out, so the Kotlin side needs no JNI object marshalling to grow a screen.

#include <string>

#include <common/android/android_common.h>
#include <common/fs/path_util.h>
#include <common/logging.h>
#include <common/settings.h>
#include <jni.h>
#include <nlohmann/json.hpp>

#include "native.h"
#include "openpak/platform.h"
#include "openpak/session.h"

namespace {

jstring Text(JNIEnv* env, const std::string& value) {
    return Common::Android::ToJString(env, value);
}

/// Presence says what is being played, which only the emulation session knows. An empty answer is
/// the game list, and reads as simply online.
std::string RunningTitleId() {
    EmulationSession& session = EmulationSession::GetInstance();

    if (!session.IsRunning()) {
        return {};
    }

    const u64 program_id = session.System().GetApplicationProcessProgramID();

    return program_id == 0 ? std::string{} : fmt::format("{:016x}", program_id);
}

} // namespace

extern "C" {

// Sign in at launch, so the account is online before anybody opens anything. The chain runs on
// the caller's thread: Kotlin calls this off the main thread.
jboolean Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeStart(JNIEnv* env, jobject) {
    if (!Settings::values.enable_openpak.GetValue()) {
        return JNI_FALSE;
    }

    openpak::Platform::SetDirectories(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir),
                                      Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir));

    openpak::client::session::Configure(Settings::values.openpak_server_ip.GetValue(), 443, {});

    if (!openpak::client::session::Ensure()) {
        LOG_WARNING(Frontend, "[OpenPak] Could not sign in at launch");
        return JNI_FALSE;
    }

    openpak::client::session::StartHeartbeat(RunningTitleId);

    LOG_INFO(Frontend, "[OpenPak] Signed in at launch");

    return JNI_TRUE;
}

// Bind this install to a person. Until it runs, a title gets a valid token that no server can
// attach to anybody -- which is a game that loads and then cannot find a friend.
jstring Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeSignIn(JNIEnv* env, jobject, jstring jemail,
                                                           jstring jpassword) {
    const std::string failure = openpak::client::session::LinkWithPassword(
        Common::Android::GetJString(env, jemail), Common::Android::GetJString(env, jpassword));

    if (failure.empty()) {
        openpak::client::session::StartHeartbeat(RunningTitleId);
    }

    return Text(env, failure);
}

// Who is signed in, for a screen to draw. Never throws and never blocks: it reads what the last
// sign-in left behind.
jstring Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeStatus(JNIEnv* env, jobject) {
    const nlohmann::json status{
        {"enabled", openpak::client::session::Enabled()},
        {"signed_in", !openpak::client::session::IdToken().empty()},
        {"linked", openpak::client::session::Linked()},
        {"nickname", openpak::client::session::Nickname()},
        {"friend_code", openpak::client::session::FriendCode()},
        {"user_id", openpak::client::session::UserId()},
        {"server", Settings::values.openpak_server_ip.GetValue()},
    };

    return Text(env, status.dump());
}

// What is waiting in the native inbox, as of the last poll the heartbeat made.
jstring Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeInvitations(JNIEnv* env, jobject) {
    nlohmann::json out = nlohmann::json::array();

    for (const auto& invitation : openpak::client::session::Invitations()) {
        out.push_back({
            {"id", invitation.id},
            {"from", invitation.sender_name},
            {"title_id", invitation.title_id},
            {"expires_at", invitation.expires_at},
        });
    }

    return Text(env, out.dump());
}

// Ask now rather than waiting for the next beat: a pull-to-refresh should answer.
jboolean Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeRefreshInvitations(JNIEnv*, jobject) {
    return openpak::client::session::RefreshInvitations() ? JNI_TRUE : JNI_FALSE;
}

void Java_org_yuzu_yuzu_1emu_utils_OpenPak_nativeDismissInvitation(JNIEnv* env, jobject,
                                                                   jstring jid) {
    openpak::client::session::DismissInvitation(Common::Android::GetJString(env, jid));
}

} // extern "C"
