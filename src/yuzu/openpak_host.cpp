// SPDX-FileCopyrightText: Copyright 2026 citron Emulator Project, OpenPak contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <utility>

#include <QActionGroup>
#include <QByteArray>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardItemModel>
#include <QUrl>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/string_util.h"
#include "openpak/platform.h"
#include "openpak/account.h"
#include "openpak/friends_cache.h"
#include "core/core.h"
#include "core/hle/service/am/applet_manager.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include <QSettings>
#include "core/hle/service/friend/friend.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"
#include "openpak/compatible_titles.h"
#include "openpak/session.h"
#include "openpak/qt/chat_client.h"
#include "yuzu/openpak_host.h"
#include "openpak/qt/save_sync.h"
#include "openpak/qt/sign_in_dialog.h"
#include "yuzu/game/game_list.h"
#include "qt_common/game_list/game_list_p.h"

#ifdef ENABLE_WEB_SERVICE
#include "openpak/api.h"
#endif

OpenPakHost::OpenPakHost(Core::System& system_, QWidget* main_window_,
                                       QObject* parent)
    : openpak::qt::Host(parent), system(system_), main_window(main_window_) {
    friend_poll_timer.setInterval(20000);
    connect(&friend_poll_timer, &QTimer::timeout, this, &OpenPakHost::PollFriends);
    friend_poll_timer.start();

    // [Nextendo] Faster than the friend-list poll: an invitation is time-sensitive/interactive
    // (the sender is actively waiting on the Invite Friends screen), not a passive presence
    // refresh -- 5s keeps it responsive without hammering the account server.
    invitation_poll_timer.setInterval(5000);
    connect(&invitation_poll_timer, &QTimer::timeout, this, &OpenPakHost::PollInvitations);
    invitation_poll_timer.start();

    active_profile = openpak::Platform::ProfileId();
}

OpenPakHost::~OpenPakHost() = default;

void OpenPakHost::GoOnline() {
    // [OpenPak] Sign in now rather than when a game first asks. Being online is the point of the
    // integration: until this runs the account is offline, invisible to friends, and hears about
    // no invitation. The chain is walked off the UI thread because a server that is slow to
    // answer must not be a window that is slow to open.
    std::thread{[this] {
        // Configure before signing in, or Enabled() is false and Ensure() returns without a
        // word: the account service configures it too, but not until a game first asks, and the
        // whole point here is to be online before that.
        if (Settings::values.enable_openpak.GetValue()) {
            openpak::client::session::Configure(Settings::values.openpak_server_ip.GetValue(), 443,
                                                {});
        }

        if (!openpak::client::session::Ensure()) {
            return;
        }

        // Presence says what is being played. The host reads it from the system it owns, on its
        // own thread, and only while a game is actually loaded.
        openpak::client::session::StartHeartbeat([this] {
            if (!system.IsPoweredOn()) {
                return std::string{};
            }

            const u64 program_id = system.GetApplicationProcessProgramID();

            return program_id == 0 ? std::string{} : fmt::format("{:016x}", program_id);
        });
    }}.detach();

    PollFriends();
    EnsureChatConnected(); // no-op if not already signed in
}

std::optional<Common::UUID> OpenPakHost::CurrentUser() const {
    return system.GetProfileManager().GetUser(
        static_cast<std::size_t>(Settings::values.current_user.GetValue()));
}

QString OpenPakHost::ProfileName(const std::string& key) const {
    const auto& profile_manager = system.GetProfileManager();

    for (const auto& uuid : profile_manager.GetAllUsers()) {
        Service::Account::ProfileBase profile{};
        if (uuid.IsValid() && uuid.RawString() == key &&
            profile_manager.GetProfileBase(uuid, profile)) {
            return QString::fromStdString(Common::StringFromFixedZeroTerminatedBuffer(
                reinterpret_cast<const char*>(profile.username.data()), profile.username.size()));
        }
    }

    return QString::fromStdString(key);
}

void OpenPakHost::SelectUser(const Common::UUID& uuid) {
    if (const auto index = system.GetProfileManager().GetUserIndex(uuid)) {
        Settings::values.current_user = static_cast<s32>(*index);
    }

    ProfileMaybeChanged();
}

void OpenPakHost::ProfileMaybeChanged() {
    const std::string profile = openpak::Platform::ProfileId();
    if (profile == active_profile) {
        return;
    }
    active_profile = profile;

    // The account service reads the last opened user; it is the current one from here on.
    if (const auto uuid = CurrentUser()) {
        system.GetProfileManager().OpenUser(*uuid);
    }

    // Nothing shown for the last profile's account may be shown as this one's.
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    offline_streak.clear();
    last_known_requests.clear();
    first_poll = true;
    if (chat_client) {
        chat_client->Disconnect();
    }

    if (IsLinked()) {
        emit AccountLinked();
    } else {
        emit AccountUnlinked();
    }

    // The session notices the switch by itself -- the old account goes offline, the new one's
    // device account signs in -- on whichever of this and the heartbeat asks first.
    if (Settings::values.enable_openpak.GetValue()) {
        std::thread{[] { openpak::client::session::Ensure(); }}.detach();
    }

    PollFriends();
    EnsureChatConnected();
}

void OpenPakHost::RunStartup(bool interactive) {
    if (interactive && Settings::values.enable_openpak.GetValue()) {
        PickStartupProfile();

        // Set up once, ever, and never over a game that is starting: sign in, create an account,
        // or play offline -- after which the profile is the person's own either way.
        if (!IsLinked() && !QSettings().value(QStringLiteral("openpak/asked"), false).toBool()) {
            QSettings().setValue(QStringLiteral("openpak/asked"), true);
            RunSetup(false);
        }
    }

    GoOnline();
}

void OpenPakHost::PickStartupProfile() {
    auto& profile_manager = system.GetProfileManager();
    if (profile_manager.GetUserCount() < 2) {
        return;
    }

    const QString startup = QSettings().value(QStringLiteral("openpak/startup_profile")).toString();

    if (startup.isEmpty()) {
        return; // the last used, which current_user already is
    }

    if (startup != QStringLiteral("ask")) {
        for (const auto& uuid : profile_manager.GetAllUsers()) {
            if (uuid.IsValid() && QString::fromStdString(uuid.RawString()) == startup) {
                SelectUser(uuid);
            }
        }
        return;
    }

    QDialog dialog(main_window);
    dialog.setWindowTitle(tr("Who is playing?"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* list = new QListWidget;
    list->setIconSize(QSize(48, 48));
    layout->addWidget(list);

    const auto current = CurrentUser();
    for (const auto& uuid : profile_manager.GetAllUsers()) {
        if (uuid.IsInvalid()) {
            continue;
        }

        const std::string key = uuid.RawString();
        const std::string account = Common::OpenPakAccount::UsernameOf(key);
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2").arg(
            ProfileName(key), account.empty()
                                  ? tr("Offline")
                                  : tr("OpenPak: %1").arg(QString::fromStdString(account))));
        item->setData(Qt::UserRole, QString::fromStdString(key));
        item->setIcon(QIcon(QString::fromStdString(Common::FS::PathToUTF8String(
            Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
            fmt::format("system/save/8000000000000010/su/avators/{}.jpg",
                        uuid.FormattedString())))));
        list->addItem(item);

        if (current && *current == uuid) {
            list->setCurrentItem(item);
        }
    }

    auto* buttons = new QDialogButtonBox;
    buttons->addButton(tr("Continue"), QDialogButtonBox::AcceptRole);
    QPushButton* add = buttons->addButton(tr("Add account"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    bool add_account = false;
    connect(add, &QPushButton::clicked, &dialog, [&] {
        add_account = true;
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);

    if (dialog.exec() != QDialog::Accepted) {
        return; // closing it keeps the last used
    }

    if (add_account) {
        RunSetup(true);
        return;
    }

    if (const auto* item = list->currentItem()) {
        const std::string key = item->data(Qt::UserRole).toString().toStdString();
        for (const auto& uuid : profile_manager.GetAllUsers()) {
            if (uuid.IsValid() && uuid.RawString() == key) {
                SelectUser(uuid);
            }
        }
    }
}

void OpenPakHost::RunSetup(bool add_account) {
    QMessageBox box(main_window);
    box.setWindowTitle(add_account ? tr("Add an account") : tr("Set up this profile"));
    box.setText(tr("Sign in to OpenPak to play online: friends, invitations and cloud saves follow "
                   "this profile. Or keep it offline. You can sign in later from the Tools menu."));
    QPushButton* sign_in = box.addButton(tr("Sign in with OpenPak"), QMessageBox::AcceptRole);
    QPushButton* create = box.addButton(tr("Create an account"), QMessageBox::ActionRole);
    QPushButton* offline = box.addButton(tr("Play offline"), QMessageBox::RejectRole);
    QPushButton* cancel = add_account ? box.addButton(QMessageBox::Cancel) : nullptr;
    box.setDefaultButton(sign_in);
    // The first launch has no cancel: closing it is the offline path, which still asks a name.
    box.setEscapeButton(add_account ? cancel : offline);
    box.exec();

    auto* const clicked = box.clickedButton();
    if (cancel != nullptr && clicked == cancel) {
        return;
    }

    auto& profile_manager = system.GetProfileManager();

    if (clicked == offline) {
        const auto current = CurrentUser();
        const QString suggested =
            add_account || !current ? QString{} : ProfileName(current->RawString());

        bool ok = false;
        const QString name = QInputDialog::getText(main_window, tr("Profile name"), tr("Name:"),
                                                   QLineEdit::Normal, suggested, &ok)
                                 .trimmed();
        if (!ok || name.isEmpty()) {
            RunSetup(add_account);
            return;
        }

        if (add_account) {
            const auto uuid = Common::UUID::MakeRandom();
            profile_manager.CreateNewUser(uuid, name.toStdString());
            profile_manager.WriteUserSaveFile();
            SelectUser(uuid);
        } else {
            ApplyProfileName(name.toStdString());
        }
        return;
    }

    if (clicked == create) {
        QDesktopServices::openUrl(QUrl(QString::fromStdString(WebService::OpenPakApi::BaseUrl()) +
                                       QStringLiteral("/register")));
    }

    // The account is kept under the current profile, so a new one is made current before the
    // sign-in and taken away again if nobody signs in.
    const auto previous = CurrentUser();
    std::optional<Common::UUID> created;
    if (add_account) {
        created = Common::UUID::MakeRandom();
        profile_manager.CreateNewUser(*created, "OpenPak");
        profile_manager.WriteUserSaveFile();
        SelectUser(*created);
    }

    const QString intro =
        clicked == create
            ? tr("Finish creating your account in the browser and verify your email, then sign in "
                 "here.")
            : QString{};

    AskAndSignIn(true, intro, {}, {}, [this, add_account, created, previous](bool signed_in) {
        if (signed_in) {
            return;
        }
        if (created) {
            system.GetProfileManager().RemoveUser(*created);
            system.GetProfileManager().WriteUserSaveFile();
            if (previous) {
                SelectUser(*previous);
            }
        }
        RunSetup(add_account);
    });
}

QMenu* OpenPakHost::CreateStartupMenu(QWidget* parent) {
    auto* menu = new QMenu(tr("OpenPak account at startup"), parent);

    // Rebuilt each time it opens: profiles come and go in the profile manager.
    connect(menu, &QMenu::aboutToShow, menu, [this, menu] {
        menu->clear();
        auto* group = new QActionGroup(menu);
        const QString chosen =
            QSettings().value(QStringLiteral("openpak/startup_profile")).toString();

        const auto add = [&](const QString& label, const QString& value) {
            QAction* action = menu->addAction(label);
            action->setCheckable(true);
            action->setChecked(chosen == value);
            group->addAction(action);
            connect(action, &QAction::triggered, this, [value] {
                QSettings().setValue(QStringLiteral("openpak/startup_profile"), value);
            });
        };

        add(tr("Last used"), QString{});
        add(tr("Ask every time"), QStringLiteral("ask"));
        menu->addSeparator();
        for (const auto& uuid : system.GetProfileManager().GetAllUsers()) {
            if (uuid.IsValid()) {
                add(ProfileName(uuid.RawString()), QString::fromStdString(uuid.RawString()));
            }
        }
    });

    return menu;
}

bool OpenPakHost::IsLinked() const {
    return Common::OpenPakAccount::IsLinked();
}

// Prototype chat server, not the official Nextendo fleet -- this only exists on the
// developer's own test VPS while the feature is being proven out and pitched. No
// OPENPAK_API-style restriction is needed here (unlike the account API, this carries
// no account token, only a bare PID + display name), but an override is still honoured
// so this can point elsewhere without a rebuild.
void OpenPakHost::EnsureChatConnected() {
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    if (chat_client && chat_client->IsConnected()) {
        return;
    }
    if (!chat_client) {
        chat_client = new NextendoChatClient(this);
        connect(chat_client, &NextendoChatClient::MessageReceived, this,
                [this](const QJsonObject& obj) {
                    const QString type = obj.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("invite_received")) {
                        emit ChatInviteReceived(obj.value(QStringLiteral("room_id")).toString(),
                                                obj.value(QStringLiteral("room_name")).toString(),
                                                static_cast<u64>(
                                                    obj.value(QStringLiteral("from_pid")).toDouble()),
                                                obj.value(QStringLiteral("from_name")).toString());
                    } else if (type == QStringLiteral("invite_sent")) {
                        emit ChatInviteSent(static_cast<u64>(
                            obj.value(QStringLiteral("target_pid")).toDouble()));
                    } else if (type == QStringLiteral("member_joined")) {
                        emit ChatMemberJoined(pending_chat_room_id,
                                              static_cast<u64>(obj.value(QStringLiteral("pid")).toDouble()),
                                              obj.value(QStringLiteral("name")).toString());
                    } else if (type == QStringLiteral("room_joined")) {
                        pending_chat_room_id = obj.value(QStringLiteral("room_id")).toString();
                    } else if (type == QStringLiteral("banned")) {
                        emit ChatBanned(obj.value(QStringLiteral("reason")).toString());
                    }
                    emit ChatRawMessage(obj);
                });
        connect(chat_client, &NextendoChatClient::Connected, this, [this] {
            chat_client->SendJson(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("identify")},
                {QStringLiteral("pid"), static_cast<qint64>(Common::OpenPakAccount::GetPid())},
                {QStringLiteral("name"),
                 QString::fromStdString(Common::OpenPakAccount::GetUsername())},
            });
        });
    }

    // OpenPak runs no chat server. The client stays for whoever points it at one.
    const char* host_env = std::getenv("OPENPAK_CHAT_HOST");
    if (!host_env || !*host_env) {
        return;
    }
    QString host = QString::fromUtf8(host_env);
    quint16 port = 8600;
    if (const char* env = std::getenv("OPENPAK_CHAT_PORT"); env && *env) {
        port = static_cast<quint16>(std::atoi(env));
    }
    chat_client->Connect(host, port);
}

QString OpenPakHost::ResolveGameName(const std::string& app_id_hex,
                                            const std::string& hint_name) const {
    if (app_id_hex.empty()) {
        return {};
    }

    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto [nacp, icon] = pm.GetControlMetadata();
    if (nacp) {
        const auto name = nacp->GetApplicationName();
        if (!name.empty()) {
            return QString::fromStdString(name);
        }
    }
    if (!hint_name.empty()) {
        return QString::fromStdString(hint_name);
    }
    return tr("a game");
}

QString OpenPakHost::ResolveGameIcon(const std::string& app_id_hex) const {
    if (app_id_hex.empty()) {
        return {};
    }

    u64 program_id = 0;
    try {
        program_id = std::stoull(app_id_hex, nullptr, 16);
    } catch (const std::exception&) {
        return {};
    }
    if (program_id == 0) {
        return {};
    }

    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto [nacp, icon_file] = pm.GetControlMetadata();
    if (!icon_file) {
        return {};
    }
    const std::vector<u8> icon_bytes = icon_file->ReadAllBytes();
    if (icon_bytes.empty()) {
        return {};
    }
    return QString::fromLatin1(QByteArray::fromRawData(
        reinterpret_cast<const char*>(icon_bytes.data()), static_cast<int>(icon_bytes.size()))
                                    .toBase64());
}

std::string OpenPakHost::GetLocalAppId() const {
    if (!system.IsPoweredOn()) {
        return {};
    }
    return fmt::format("{:016X}", system.GetApplicationProcessProgramID());
}

void OpenPakHost::SignIn() {
    AskAndSignIn(false, {}, {}, {});
}

void OpenPakHost::AskAndSignIn(bool adopt, const QString& intro, const QString& error,
                               const QString& last_email, std::function<void(bool)> done) {
#ifdef ENABLE_WEB_SERVICE
    // Email and password, asked here on the UI thread; the sign-in itself runs off it. The
    // password goes to OpenPak over TLS and nowhere else: what comes back is a website token
    // and the Switch identity the games see.
    OpenPakSignInDialog dialog(main_window, intro, error, last_email);
    if (dialog.exec() != QDialog::Accepted) {
        if (done) {
            done(false);
        }
        return;
    }
    const QString email = dialog.Email();
    const QString password = dialog.Password();
    emit StatusChanged(tr("Signing in to OpenPak..."));

    QPointer<OpenPakHost> self(this);
    std::thread{[this, self, email = email.toStdString(), password = password.toStdString(), adopt,
                 intro, done] {
        auto login_result = WebService::OpenPakApi::SignIn(email, password);

        // One account, one profile: two profiles on one account would share a cloud-save slot and
        // overwrite each other's progress. Checked before the console link, which would otherwise
        // bind this profile's device account to it first.
        if (login_result.ok) {
            const std::string holder =
                Common::OpenPakAccount::HolderOf(login_result.pid, openpak::Platform::ProfileId());
            if (!holder.empty()) {
                login_result.ok = false;
                login_result.error = tr("This OpenPak account is already linked to the profile "
                                        "\"%1\". Sign in there, or sign that profile out first.")
                                         .arg(ProfileName(holder))
                                         .toStdString();
            }
        }

        // Two sign-ins, because they are two different things: the website account is what
        // friends and cloud saves speak with, and the console chain is what puts an identity in
        // front of a title server. A link that fails leaves the website half standing.
        std::string link_failure;
        if (login_result.ok && openpak::client::session::Enabled()) {
            link_failure = openpak::client::session::LinkWithPassword(email, password);
        }

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, self, result = std::move(login_result), link_failure,
             email = QString::fromStdString(email), adopt, intro, done] {
                if (!self) {
                    return;
                }
                if (!result.ok) {
                    emit StatusChanged(QString::fromStdString(result.error));
                    emit SignInFinished();
                    // Back to the dialog with the reason on it, rather than a line in the
                    // status bar and a menu to find again.
                    AskAndSignIn(adopt, intro, QString::fromStdString(result.error), email, done);
                    return;
                }
                emit StatusChanged(
                    link_failure.empty()
                        ? tr("Signed in as %1, and this console is now linked to your account.")
                              .arg(QString::fromStdString(result.username))
                        : tr("Signed in as %1, but the console link did not complete: %2")
                              .arg(QString::fromStdString(result.username),
                                   QString::fromStdString(link_failure)));

                Common::OpenPakAccount::Save(result.pid, result.username, result.friend_code,
                                             result.token, result.bearer);
                if (adopt) {
                    ApplyProfileName(result.username);
                    SyncProfileAvatar();
                }
                Common::NextendoFriends::SetLocalStatus(Common::NextendoFriends::PresenceOnline);
                first_poll = true;
                emit AccountLinked();
                emit SignInFinished();
                RefreshFriendCache();
                EnsureChatConnected();
                if (done) {
                    done(true);
                }
            },
            Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void OpenPakHost::SignOut() {
    // The website token is revoked on the server, not only forgotten here, as Ryujinx does. The
    // console link stays: signing out of the website does not unlink a Switch either.
#ifdef ENABLE_WEB_SERVICE
    std::thread{[bearer = Common::OpenPakAccount::GetBearer()] {
        WebService::OpenPakApi::RevokeToken(bearer);
    }}.detach();
#endif
    Common::OpenPakAccount::Clear();
    Common::NextendoFriends::Set({});
    last_known_status.clear();
    offline_streak.clear();
    if (chat_client) {
        chat_client->Disconnect();
    }
    emit AccountUnlinked();
}

void OpenPakHost::ManualSaveDownload(u64 title_id) {
#ifdef ENABLE_WEB_SERVICE
    // Belt-and-suspenders: the dialog already hides this action while any game is running,
    // but writing into the save directory while the emulated filesystem layer is mounted by
    // a live session is what actually crashes it, so refuse here regardless of caller.
    if (system.IsPoweredOn()) {
        emit StatusChanged(tr("Stop the running game before downloading a cloud save."));
        return;
    }
    if (!Nextendo::CompatibleTitles::Table().count(title_id)) {
        emit StatusChanged(tr("This game doesn't support cloud saves."));
        return;
    }

    emit StatusChanged(tr("Downloading save from the cloud..."));
    QPointer<OpenPakHost> self(this);
    std::thread{[this, self, title_id] {
        Nextendo::SaveSync::Pull(SaveDirectory(title_id), title_id, /*force=*/true);
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            this, [this, self] {
                if (!self) {
                    return;
                }
                emit StatusChanged(tr("Cloud save applied."));
            }, Qt::QueuedConnection);
    }}.detach();
#else
    emit StatusChanged(tr("This build has no web services support."));
#endif
}

void OpenPakHost::QuickStart(u64 title_id) {
    emit QuickStartRequested(title_id);
}

void OpenPakHost::ApplyProfileName(const std::string& name) {
    if (name.empty()) {
        return;
    }

    // This used to construct its own throwaway Service::Account::ProfileManager here, which
    // re-parses the save file into a brand new object -- separate from system.GetProfileManager(),
    // the one live instance every running game and the Profile Manager config page actually read
    // from. Writes landed on disk but never reached the in-memory copy anything else sees, so the
    // rename appeared to silently do nothing (this was the unresolved half of the earlier Balloon
    // World self-profile investigation).
    auto& profile_manager = system.GetProfileManager();
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return;
    }
    const auto uuid = *current;

    Service::Account::ProfileBase profile{};
    if (!profile_manager.GetProfileBase(uuid, profile)) {
        return;
    }

    const std::string trimmed = name.substr(0, profile.username.size() - 1);
    std::fill(profile.username.begin(), profile.username.end(), '\0');
    std::copy(trimmed.begin(), trimmed.end(), profile.username.begin());

    profile_manager.SetProfileBase(uuid, profile);
    profile_manager.WriteUserSaveFile();
    LOG_INFO(Frontend, "[OpenPak] Renamed the active profile to the account nickname");
}

void OpenPakHost::SyncProfileAvatar() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return;
    }
    const auto uuid = *current;
    const u64 pid = Common::OpenPakAccount::GetPid();
    std::thread{[this, uuid, pid, guard = QPointer<OpenPakHost>(this)] {
        const std::string b64 = WebService::OpenPakApi::GetAvatarByPid(pid);
        if (b64.empty()) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, guard, uuid, b64] {
                if (!guard) {
                    return;
                }
                WriteProfileAvatar(uuid, b64);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void OpenPakHost::WriteProfileAvatar(const Common::UUID& uuid, const std::string& avatar_b64) {
    QImage image;
    if (!image.loadFromData(QByteArray::fromBase64(QByteArray::fromStdString(avatar_b64)))) {
        return;
    }
    if (image.width() != 256 || image.height() != 256) {
        image = image.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

    const auto image_path = QString::fromStdString(Common::FS::PathToUTF8String(
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) /
        fmt::format("system/save/8000000000000010/su/avators/{}.jpg", uuid.FormattedString())));

    QDir{}.mkpath(QFileInfo(image_path).absolutePath());
    if (image.save(image_path, "JPEG")) {
        LOG_INFO(Frontend, "[OpenPak] Synced the active profile's avatar from the account");
    }
}

void OpenPakHost::RefreshFriendCache() {
    PollFriends();
}

void OpenPakHost::NotifyFriendRequestSent(const QString& friend_code) {
    emit FriendRequestSent(friend_code);
}

QString OpenPakHost::JoinFriendSession(u64 pid) {
    const auto entries = Common::NextendoFriends::Get();
    const auto it = std::find_if(entries.begin(), entries.end(),
                                  [pid](const auto& e) { return e.pid == pid; });
    if (it == entries.end() || it->status != Common::NextendoFriends::PresenceOnlinePlay ||
        it->app_field.empty()) {
        const QString message = tr("That friend isn't in a joinable game right now.");
        emit StatusChanged(message);
        return message;
    }

    // Bypasses the native Invite Friends applet entirely: this friend's published session blob
    // is already here (the same presence feed the list renders), so it goes straight into the
    // running game's invitation channel, as an accepted invitation would.
    const auto user = CurrentUser();
    if (!user || !system.GetAppletManager().PushFriendInvitation(
                     *user, std::vector<u8>(it->app_field.begin(), it->app_field.end()))) {
        const QString message = tr("Start the game first, then join from here.");
        emit StatusChanged(message);
        return message;
    }

    const QString message = tr("Joining %1's game.")
                                .arg(QString::fromStdString(it->name));
    emit StatusChanged(message);
    return message;
}

void OpenPakHost::PollFriends() {
#ifdef ENABLE_WEB_SERVICE
    if (!Common::OpenPakAccount::IsLinked()) {
        return;
    }

    QPointer<OpenPakHost> self(this);
    std::thread{[this, self] {
        auto fetched = WebService::OpenPakApi::GetFriends();
        if (!fetched.ok) {
            return;
        }
        if (!self) {
            return;
        }

        QMetaObject::invokeMethod(
            this,
            [this, self, list = std::move(fetched)] {
                if (!self) {
                    return;
                }
                std::vector<Common::NextendoFriends::Entry> cache;
                cache.reserve(list.friends.size());
                for (const auto& entry : list.friends) {
                    const auto decoded_image =
                        QByteArray::fromBase64(QByteArray::fromStdString(entry.image_base64));
                    cache.push_back(
                        {entry.pid, entry.name, entry.presence_status, entry.app_field,
                         std::vector<u8>(decoded_image.begin(), decoded_image.end())});
                }
                Common::NextendoFriends::Set(std::move(cache));
                // [Nextendo] The guest's own INotificationService only ever signals once, at
                // construction -- before this first real poll has a chance to land. Without this,
                // a Friends viewer already on-screen never learns that real data showed up.
                // TODO(openpak): Eden's friend service has no NotifyFriendsListUpdated hook
                // yet (Citron added one); a Friends viewer already on-screen refreshes on its
                // next own poll instead.

                const bool suppress_toasts = first_poll;
                first_poll = false;

                std::map<u64, s32> current_status;
                for (const auto& entry : list.friends) {
                    const auto it = last_known_status.find(entry.pid);
                    const bool was_online = it != last_known_status.end() && it->second != 0;

                    // A status of 0 on a single poll can be a mid-transition blip on the
                    // server (e.g. switching presence when joining a match together) rather
                    // than a real disconnect. Require it to repeat on the next poll (~20s)
                    // before treating the friend as actually offline, so a one-poll blip can't
                    // fire a false "went offline" or the false "came online" right after it.
                    if (entry.presence_status == 0 && was_online) {
                        if (++offline_streak[entry.pid] < 2) {
                            current_status[entry.pid] = it->second;
                            continue;
                        }
                        if (!suppress_toasts) {
                            emit FriendWentOffline(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                        }
                    } else {
                        offline_streak.erase(entry.pid);
                        const bool was_offline = it == last_known_status.end() || it->second == 0;
                        if (!suppress_toasts && was_offline && entry.presence_status != 0) {
                            emit FriendCameOnline(entry.pid, QString::fromStdString(entry.name),
                                                  ResolveGameName(entry.app_id, entry.app_name),
                                                  QString::fromStdString(entry.image_base64));
                        }
                    }
                    current_status[entry.pid] = entry.presence_status;
                }
                last_known_status = std::move(current_status);

                std::set<u64> current_requests;
                for (const auto& entry : list.requests) {
                    current_requests.insert(entry.pid);
                    if (!suppress_toasts && !last_known_requests.contains(entry.pid)) {
                        emit FriendRequestReceived(entry.pid, QString::fromStdString(entry.name),
                                                   QString::fromStdString(entry.image_base64));
                    }
                }
                last_known_requests = std::move(current_requests);
            },
            Qt::QueuedConnection);
    }}.detach();
#endif
}

void OpenPakHost::PollInvitations() {
    // What the heartbeat last read from the native inbox, offered as Ryujinx offers it: an
    // invitation to the running game asks Join or Ignore, and Join leaves the sender's data in the
    // game's invitation channel, where the game looks for it. One to another game is announced
    // once and offered when that game is running. Either answer marks it read.
    const std::string running = GetLocalAppId();
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    for (const auto& invitation : openpak::client::session::Invitations()) {
        if (offered_invitations.contains(invitation.id) ||
            (invitation.expires_at != 0 && invitation.expires_at < now)) {
            continue;
        }

        const QString sender = QString::fromStdString(invitation.sender_name);
        const QString game = ResolveGameName(invitation.title_id);
        if (running.empty() || Common::ToLower(running) != Common::ToLower(invitation.title_id)) {
            if (announced_invitations.insert(invitation.id).second) {
                emit StatusChanged(tr("%1 invited you to play %2. Start it to join.").arg(sender, game));
            }
            continue;
        }

        offered_invitations.insert(invitation.id);
        const auto answer = QMessageBox::question(
            main_window, tr("Game invitation"), tr("%1 invited you to join them in %2.").arg(sender, game),
            QMessageBox::Yes | QMessageBox::Ignore, QMessageBox::Yes);

        if (answer == QMessageBox::Yes) {
            const auto user = CurrentUser();
            if (!user || !system.GetAppletManager().PushFriendInvitation(*user, invitation.app_param)) {
                emit StatusChanged(tr("The game closed before the invitation could be handed over."));
            }
        }

        std::thread{[id = invitation.id] { openpak::client::session::DismissInvitation(id); }}.detach();
    }
}

// ---- openpak::qt::Host: Eden-specific answers ----

std::vector<openpak::qt::Host::Title> OpenPakHost::InstalledTitles() const {
    // The game list already resolved every name; asking the content provider again would parse
    // a control NCA per title on the UI thread.
    std::vector<Title> out;
    const auto* game_list = main_window->findChild<GameList*>();
    const QStandardItemModel* model = game_list ? game_list->GetModel() : nullptr;
    for (int dir = 0; model && dir < model->rowCount(); ++dir) {
        const QStandardItem* folder = model->item(dir);
        for (int row = 0; row < folder->rowCount(); ++row) {
            const QStandardItem* game = folder->child(row);
            const u64 id = game->data(GameListItemPath::ProgramIdRole).toULongLong();
            const bool seen = std::any_of(out.begin(), out.end(),
                                          [id](const Title& title) { return title.id == id; });
            if (id != 0 && !seen) { // the favourites folder repeats rows
                out.push_back({id, game->data(GameListItemPath::TitleRole).toString()});
            }
        }
    }
    return out;
}

std::filesystem::path OpenPakHost::ModDirectory(u64 title_id) {
    // load/<TITLEID>/<mod>/romfs: where BISFactory::GetModificationLoadRoot reads, so an
    // installed mod shows up in the title's Properties -> Add-Ons like any other.
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / fmt::format("{:016X}", title_id);
}

std::filesystem::path OpenPakHost::SaveDirectory(u64 title_id) {
    // nand/user/save/0000000000000000/<user>/<TITLEID>, the path SaveDataFactory::GetFullPath
    // builds. The active profile's own, because the cloud save belongs to the account that
    // profile is signed in as: another profile's folder would upload somebody else's progress.
    // A title with only a device save (ACNH) keeps it under the all-zero user, as on a console.
    const auto root = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir) / "user" / "save" /
                      "0000000000000000";
    const std::string title = fmt::format("{:016X}", title_id);
    const auto current = CurrentUser();
    if (!current || current->IsInvalid()) {
        return {};
    }
    const auto id = current->AsU128();
    const auto own = root / fmt::format("{:016X}{:016X}", id[1], id[0]) / title;
    const auto device = root / std::string(32, '0') / title;

    std::error_code ec;
    if (!std::filesystem::is_directory(own, ec) && std::filesystem::is_directory(device, ec)) {
        return device;
    }
    return own;
}

QString OpenPakHost::AccentColor() const {
    return QStringLiteral("#2f56d0");
}

bool OpenPakHost::IsDarkTheme() const {
    return UISettings::IsDarkTheme();
}

// Notification and cloud-sync preferences live in Eden's own config as UI settings.
bool OpenPakHost::NotificationsEnabled() const {
    return QSettings().value(QStringLiteral("openpak/notifications_enabled"), true).toBool();
}
void OpenPakHost::SetNotificationsEnabled(bool enabled) {
    QSettings().setValue(QStringLiteral("openpak/notifications_enabled"), enabled);
}
int OpenPakHost::NotificationCorner() const {
    return QSettings().value(QStringLiteral("openpak/notification_corner"), 1).toInt();
}
void OpenPakHost::SetNotificationCorner(int corner) {
    QSettings().setValue(QStringLiteral("openpak/notification_corner"), corner);
}
bool OpenPakHost::RedirectEnabled() const {
    return Settings::values.enable_openpak.GetValue();
}
bool OpenPakHost::CloudSyncEnabled() const {
    return QSettings().value(QStringLiteral("openpak/cloud_sync_enabled"), true).toBool();
}
void OpenPakHost::SetCloudSyncEnabled(bool enabled) {
    QSettings().setValue(QStringLiteral("openpak/cloud_sync_enabled"), enabled);
}
std::string OpenPakHost::ServerIp() const {
    return Settings::values.openpak_server_ip.GetValue();
}
std::string OpenPakHost::NatIp() const {
    return Settings::values.openpak_nat_ip.GetValue();
}
void OpenPakHost::SetGuestInputSuspended(bool) {
    // Eden has no guest-input suspension; the dialog is modal, which is enough.
}
openpak::qt::Navigation* OpenPakHost::CreateNavigation(QObject*) {
    return nullptr; // keyboard and mouse; gamepad navigation comes with a later cut
}
