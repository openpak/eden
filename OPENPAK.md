# Eden for OpenPak

Fork of upstream Eden (Switch, yuzu family) with one addition: the shared OpenPak client.
This build **is the client, not an emulator with a mode** — OpenPak is on by default. The
integration is `openpak-client` vendored at `externals/openpak-client` with its Qt dialogs,
hosted by `OpenPakHost` (`src/yuzu/openpak_host.*`): a one-time sign-in offer at first launch,
sign in/out (which also links the console), the shared account dialog (Tools → OpenPak
Account) with friends, presence and requests, cloud saves, invitations, mods (installed into
`load/<title>/`), news and status, and an invitation
path that skips the native Invite Friends applet (blocked by an unrecovered firmware struct
layout) by injecting the friend's already-fetched presence app_field into the local
pending-invitation queue, where the title's own poll picks it up. Nintendo's online hostnames
resolve to the OpenPak server. Everything else is upstream, merged as it moves. Builds:
`openpak-v*` tags publish releases from the fork's own tag namespace (`.github/workflows`,
with `.forgejo/workflows` carrying the non-build checks); `openpak-v0.1.0` and
`openpak-v0.2.0` are out, both on branch `e1`.

Branch layout: `main` carries the redirect, the CI and the on-by-default switch; `e1`
(current) carries the openpak-client dialogs on top plus the unreleased work — an Android
identity and account, and the title online path itself (nsd, ports, sockets, ALPN; BSD
deferral, v6 and sockopt repairs; NPLN worker freeze tracing).

Profiles: each Eden user profile is its own OpenPak account, one active at a time
(`emulators/prds/emulator-integration-prd.md` §3.1). The shared client asks the core which profile
is current (`openpak::Platform::SetProfileSource`, set by `Core::System`) and keeps the account
file (`openpak/account-<uuid>.txt`) and device account (`openpak/device-<server>-<uuid>.json`)
per profile; a switch takes the old account offline and signs the new one in. A plain launch runs
`OpenPakHost::RunStartup`: *Tools → OpenPak account at startup* (last used, ask, or one profile),
then, once ever, the setup (*Sign in with OpenPak*, *Create an account*, *Play offline* with a
name). Signing in there copies the account's name and avatar into the profile once; the old
every-launch sync is gone. `acc` answers only the current profile with the OpenPak identity,
`TrySelectUserWithoutInteraction` picks the current profile, and a title's own profile picker is
skipped unless it rules profiles out. One account links to one profile; deleting a profile forgets
its account. The single account file from before moves to the profile open at the first launch.

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace; this fork is milestone E1 of `emulator-integration-prd.md`.
