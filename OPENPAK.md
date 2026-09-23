# Eden for OpenPak

Fork of upstream Eden (Switch, yuzu family) that plays supported Switch titles online on the
OpenPak network. This build **is the client, not an emulator with a mode**: OpenPak is on by
default (`enable_openpak` in the config, to turn it off for a stock test).
Ryujinx is the reference for the Switch integration (`emulators/prds/emulator-integration-prd.md`
§2a): every behaviour below was ported from it, and whatever lands there next is ported here and
to Citron. The shared parts live in `openpak-client` (`externals/openpak-client`, GitHub
`openpak/openpak-client`): its client half is linked into core, its Qt half (the dialogs) into the
frontend. Eden keeps only hook lines and the platform glue.

## What it does

- **Sign-in and the console chain** — email and password go to OpenPak over TLS and nowhere
  else. Signing in also links the console: `acc` walks the library's chain (dauth, BAAS device
  account, login bound to the running title's id and version) and hands titles the id_token
  OpenPak issued (`CreateAuthorizationRequest` answers with it too).
- **Profiles** — each Eden user profile is its own OpenPak account, one active at a time. A
  plain launch picks the profile (last used, ask, or one profile; the setting has no UI until
  the spec's Configure → OpenPak page, E2), offers the setup once (sign in, create an account, play offline), then goes online.
  `acc` answers only the active profile, `TrySelectUserWithoutInteraction` and the title's own
  profile picker take it, and deleting a profile forgets its account.
- **Presence** — the library's heartbeat keeps the account online and the friends caches warm;
  closing the window says goodbye. What it publishes is what the title declares: core reports
  the running title and its NACP presence group, and friend:u 10600/10601/10610 decide ONLINE or
  PLAYING and the appField.
- **Friend service** — `friend:*` is a port of Ryujinx's FriendService command by command
  (`src/core/hle/service/friend/openpak_friends.*`, two hook lines in `friend.cpp`): the same
  ids, port permissions and result codes. Reads come only from the library's caches, writes go
  out on its worker, and INotificationService's queue is signalled from a thread owned by the
  emulation session. Only the active, signed-in profile gets OpenPak's data.
- **Invitations** — an invitation to the running game asks Join or Ignore; Join puts
  `[Uid][application data]` in the application's friend-invitation channel
  (`AppletManager::PushFriendInvitation`, what qlaunch does with IApplicationAccessor 180), where
  the game pops it with 141. One for another game is offered when that game runs. The inbox is
  the heartbeat's cache.
- **Sending invitations** — with OpenPak on, MyPage is OpenPak's own applet
  (`am/frontend/applet_my_page.*` over `openpak::my_page`): "invite friends" opens the library's
  friend picker (`openpak/qt/friend_picker.h`), a game that names its invitees sends as it
  stands, and friend:m 30900/30901 send too.
- **OpenPak window** — the library's account window, Ryujinx's seven pages in its order
  (the OpenPak menu's header and page items): account, friends, invitations and players, history, cloud saves,
  mods (installed into `load/<title>/`), news and status.
- **Controller** — `OpenPakHost::CreateNavigation` turns upstream's `ControllerNavigation`
  (player one's pad or the handheld's; the *controller navigation* setting) into the library's
  `Navigation`, so the OpenPak window and the friend picker work with a pad as well as with
  mouse and keyboard.
- **Toasts** — a friend coming online or starting a game, a friend request, a game invitation.
- **Online status** — the game list's Online column shows the catalogue's live/beta/alpha
  (`openpak::compatibility`, refreshed from the site at startup).
- **Push** — besides the heartbeat's poll, the library holds the Penne push connection a
  console holds (`openpak/push.h`): a delivered friend request, acceptance, removal, invitation
  or presence change re-reads the list it concerns at once. Downlink only; presence stays on
  the REST PATCH. `OPENPAK_NO_PUSH=1` turns it off.
- **Blocking** — friend:m 30400–30403 block and 30402 unblocks against BAAS
  (`baas::BlockUser`/`UnblockUser`); the block list, friend list and request boxes re-sync
  after the write.
- **BCAT** — with OpenPak on, a title's `RequestSyncDeliveryCache` fills its delivery cache from
  the news service's dataset (`openpak::bcat`, `/api/emulator/v1/bcat/titles/<tid>`,
  sha256-checked, cached for offline launches), through the OpenPak BCAT backend in
  `bcat/service_creator.cpp`.
- **NAT type** — the account window's NAT pill runs the console's own Test Connection exchange
  (`openpak::nat`) against nncs1/nncs2 and shows the letter A–F, mapping and filtering in the
  tooltip.
- **Cloud saves** — pulled before a title boots and pushed when it stops, every title, from the
  active profile's save folder (or the device save), versioned against a marker; a manual
  download works for every title.
- **Redirection** — Nintendo's online hostnames resolve to the OpenPak server, following the
  network profile OpenPak publishes (`openpak::NetworkProfile`: fetched at the first sign-in,
  cached, built-in list when neither is there). The NAT check's second probe goes to OpenPak's
  second responder (`openpak_nat_ip`).
- **Certificates** — the OpenPak CA is installed where the console's store reads a root, and
  guest TLS verifies against it (plus the system store) as the title asks, by IP SAN when the
  title names an address. Eden builds its OpenSSL backend on every OS (the Schannel and
  SecureTransport backends are not compiled), so Windows and macOS verify the same way.

## The title online path

What a title's own online stack (NEX, NPLN/gRPC, Photon) needs from the HLE services, found on
Stardew Valley's NPLN tenant and ACNH:

- `nsd` fills the environment into `%` names (lp1) and resolves for real;
  `accounts.nintendo.com` (and its `-sb` sandbox name) is rewritten to the BAAS host behind it.
- `sfdnsres` getaddrinfo: the port in network order, answers for any socket type, the canonical
  name, correct address lengths.
- `bsd`: blocking polls with an eventfd in the set are deferred (gRPC parks a thread there and
  expects another thread's eventfd write to wake it); eventfd with counter semantics, `read`,
  `sendmmsg`/`recvmmsg`; AF_INET6 dual-mode sockets; options a title sets are read back, and
  one this build cannot answer says NOPROTOOPT; getifaddrs (sysctl NET_RT_IFLISTL) answers with
  the selected interface; an ICMP error on a UDP socket is discarded.
- `nifm`: SetExclusiveClient, IsAnyInternetRequestAccepted, SetDefaultIpSetting and the two
  ForTest switches.
- `ssl`: DoNotCloseSocket leaves the socket with the title, Pending answers with the plaintext
  OpenSSL holds, ALPN keeps the offered list and the negotiated name apart.
- `npns:u`: ListenAll/ListenTo succeed and never fire, so ACNH polls.
- The NPLN hold: the first npln resolution waits out the startup translation burst once.
- Stardew Valley 1.6.15.13: a build-scoped patch of the title's own X509 check
  (`core/loader/nso.cpp`), identical to Ryujinx's and Citron's.

## Files and hooks

- `src/yuzu/openpak_host.*` — `OpenPakHost`, the library's `openpak::qt::Host`, the same file as
  Citron's but for the lines where the two emulators differ: titles, saves, settings,
  navigation, the startup picker and setup, the OpenPak window, the toasts (UX spec §3.10: the
  host owns them; nothing OpenPak goes to the status bar), invitations, cloud-save hooks.
- `src/yuzu/main_window.cpp`, `main.ui` — creates the host, installs the friend picker, fills the
  top-level OpenPak menu (`emulators/prds/openpak-ux-spec.md` §3.1, the library's
  `PopulateOpenPakMenu`): *Sign in to OpenPak...* or *Signed in as {name}*, Friends,
  Invitations, Cloud saves, Mods, News, Status, *OpenPak settings...* (Configure at its OpenPak
  page), *OpenPak website*, *Sign out...* (with the §3.5 confirmation). The *Open OpenPak*
  hotkey (no keyboard default, Home+X on a controller; Change Docked Mode moved to Home+ZL, as in
  Citron) opens the window at its last page. The library's translations load with Eden's.
- `src/yuzu/configuration/configure_openpak.*` — Configure → System → OpenPak (§3.13): the
  library's settings section plus the Server IP and NAT IP fields under Advanced. Settings:
  `enable_openpak`, `openpak_server_ip`, `openpak_nat_ip`, `openpak_cloud_sync_enabled`, and
  the UI keys `openpak/notificationsEnabled`, `openpak/notificationCorner`,
  `openpak/startupProfile`, `openpak/setupOffered` (the same as Citron's; the old QSettings
  keys are moved over once). The library's dialogs are the only OpenPak dialogs the build shows.
- `src/core/hle/service/acc/acc.cpp` — the console chain, the active profile, the network
  profile fetch.
- `src/core/hle/service/friend/openpak_friends.*`, `am/frontend/applet_my_page.*`,
  `am/applet_manager.*` — friends, MyPage, the invitation channel.
- `src/core/hle/service/{sockets,nifm,ssl,npns}/` — the online path above.
- `src/core/core.cpp` — the running title and its presence group.
- `src/qt_common/game_list/` — the Online column.
- `src/tests/core/sockets/openpak_redirect.cpp` — `[openpak]` tests (redirect, nsd, resolver).

Settings: `enable_openpak`, `openpak_server_ip`, `openpak_nat_ip`. Environment overrides:
`OPENPAK_ENABLE`, `OPENPAK_SERVER_IP`, `OPENPAK_NO_CERT=1`, `OPENPAK_CHAT_HOST`/`OPENPAK_CHAT_PORT`;
tracing: `EDEN_SSL_TRACE=1` (guest TLS in the clear), `SSLKEYLOGFILE`,
`OPENPAK_TRACE_GUEST_FAULT`.

## Android

The same library and core behind native Material screens, as `emulators/prds/openpak-ux-spec.md`
§4 has them: Kotlin `utils/OpenPak.kt` (the bridge and the poll), `utils/OpenPakUi.kt` (startup
profile choice, first run, full-screen sign-in, sign-out confirmation, friend picker, invitation
prompt, conflict sheet, snackbars and notifications) and `fragments/OpenPakFragment.kt` (the
OpenPak home, the seven sections and the OpenPak settings), over one JSON bridge
`jni/openpak_native.cpp` (`nativeCall(method, args)`). Entry points: the first row of Settings,
the in-game menu, and a notification's tap. Cloud saves are pulled in `InitializeEmulation` and
pushed in `ShutdownEmulation` (`jni/native.cpp`). The strings are the spec's table,
`res/values/openpak_strings.xml`. Citron's Android carries the same files, package names aside.

Every request says which build asks: `X-OpenPak-Client: eden/<version>+<hash>`
(`openpak::Platform::SetClient`, desktop and Android).

## Builds and releases

`vX.Y.Z` tags (`v*.*.*`) publish a GitHub release (upstream's tags are never pushed here): the Android APK
(`.github/workflows/build-android.yml`) and the Linux AppImage
(`.github/workflows/build-linux.yml`), both attached to the same release without artifacts.
`.forgejo/workflows` carries upstream's non-build checks. Windows and macOS desktop builds are
not released yet. Local desktop build: `build-openpak/`.

PRDs: [`../prds/`](../prds/README.md) — `emulator-integration-prd.md` (§2a, the parity table)
and `emulator-network-profile-prd.md`.
