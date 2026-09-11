// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project, OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <filesystem>
#include "openpak/qt/host.h"

#include <map>
#include <set>
#include <string>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>

#include "openpak/types.h"

namespace Core {
class System;
}

namespace Common {
struct UUID;
}

class NextendoChatClient;

// Sign-in/out, friend-cache refresh, and the online-toast poll. One instance, owned by GMainWindow.
class OpenPakHost : public openpak::qt::Host {
    Q_OBJECT

public:
    explicit OpenPakHost(Core::System& system, QWidget* main_window,
                                QObject* parent = nullptr);
    ~OpenPakHost() override;

    bool IsLinked() const override;

    // Title id -> display name via the locally installed game's NACP. Falls back to hint_name
    // (a name the PLAYING client already resolved, e.g. from a friend's presence) when this
    // game isn't in the local library; falls back further to a generic "a game" if both fail.
    QString ResolveGameName(const std::string& app_id_hex, const std::string& hint_name = {}) const override;

    // Title id -> base64 icon via the locally installed game's NACP; empty if not installed.
    QString ResolveGameIcon(const std::string& app_id_hex) const override;

    // 16 hex digits of the locally running title, matching Friend::app_id; empty if not running.
    std::string GetLocalAppId() const override;

    void SignIn() override;
    void SignOut() override;
    void RefreshFriendCache() override;
    void NotifyFriendRequestSent(const QString& friend_code) override;

    // Skips the native Invite Friends applet (blocked by an unrecovered firmware struct
    // layout, see HANDOFF.md) entirely: injects the friend's already-fetched presence
    // app_field straight into the local pending-invitation queue, as if a real invitation
    // had just arrived, so the running/next-launched title's own
    // TryPopFromFriendInvitationStorageChannel poll picks it up normally. Returns a
    // human-readable result so the caller can show it directly -- StatusChanged alone isn't
    // reliably visible from every page/dialog this can be triggered from.
    QString JoinFriendSession(u64 pid) override;

    void ManualSaveDownload(u64 title_id) override;
    void QuickStart(u64 title_id) override;

    // Chat: one persistent connection for the whole session (not tied to any dialog's
    // lifetime), so invite/join notifications work the same way FriendRequestReceived
    // does -- in the background, regardless of whether the chat window is open. Idempotent.
    void EnsureChatConnected() override;
    NextendoChatClient* GetChatClient() override {
        return chat_client;
    }

    // openpak::qt::Host: what the shared dialogs need from Eden
    std::filesystem::path SaveDirectory(u64 title_id) override;
    QString AccentColor() const override;
    bool IsDarkTheme() const override;
    bool NotificationsEnabled() const override;
    void SetNotificationsEnabled(bool enabled) override;
    int NotificationCorner() const override;
    void SetNotificationCorner(int corner) override;
    bool RedirectEnabled() const override;
    bool CloudSyncEnabled() const override;
    void SetCloudSyncEnabled(bool enabled) override;
    std::string ServerIp() const override;
    std::string NatIp() const override;
    void SetGuestInputSuspended(bool suspended) override;
    openpak::qt::Navigation* CreateNavigation(QObject* parent) override;

private:
    void ApplyProfileName(const std::string& name);
    // Forces the active Switch profile's picture to match the linked Nextendo account's
    // avatar, mirroring ApplyProfileName's username sync. Fetches async since it needs a
    // network round-trip; WriteProfileAvatar does the actual disk write once it lands.
    void SyncProfileAvatar();
    void WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64);
    void PollFriends();
    void PollInvitations();

    Core::System& system;
    QWidget* main_window;
    QTimer friend_poll_timer;
    QTimer invitation_poll_timer;
    std::map<u64, s32> last_known_status;
    std::map<u64, int> offline_streak; // consecutive polls seen offline, not yet confirmed
    std::set<u64> last_known_requests;
    bool first_poll = true; // suppresses a toast burst for every friend already online at boot

    NextendoChatClient* chat_client = nullptr;
    QString pending_chat_room_id; // set by whichever create/join is in flight, used to tag ChatMemberJoined
};
