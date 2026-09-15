# Eden for OpenPak

Fork of upstream Eden (Switch, yuzu family) with one addition: the shared OpenPak client.
This build **is the client, not an emulator with a mode** — OpenPak is on by default. The
integration is `openpak-client` vendored at `externals/openpak-client` with its Qt dialogs,
hosted by `OpenPakHost` (`src/yuzu/openpak_host.*`): sign in/out, the shared account dialog
(Tools → OpenPak Account), friends with presence and requests, cloud saves, and an invitation
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

PRDs: [`../prds/`](../prds/README.md) — emulator-wide PRDs live at `emulators/prds/` in the
workspace; this fork is milestone E1 of `emulator-integration-prd.md`.
