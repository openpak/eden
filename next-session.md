# Next session — eden

Updated 2026-09-15.

Upstream Eden (Switch, yuzu family) whose OpenPak build is the client, not an emulator with a
mode: `openpak-client` dialogs hosted by `OpenPakHost`, on by default, Nintendo hosts
redirected. `openpak-v0.1.0`/`openpak-v0.2.0` are out; branch `e1` carries everything since,
untagged, including the title online path.

## Where things stand

- `main`: redirect, CI (own `openpak-v*` tag namespace, `.forgejo` checks), on-by-default.
- `e1` (current): shared account dialog, sign-in, friends and cloud saves on
  `externals/openpak-client`; an Android identity and account; the title online path — nsd,
  ports, sockets, ALPN, BSD deferral/v6/sockopt repairs, NPLN worker pollfd tracing at a
  freeze; openpak-client pointed at the Android CA fix.
- Invitations inject into the local pending-invitation queue (native applet blocked by an
  unrecovered firmware struct layout).

## Next steps

- Close the NPLN freeze: the trace commits (91e92fc93f) are the instrument; the BSD repairs
  (cf4ea9d638) are the candidate fix.
- Merge `e1` → `main`, local build, cut the next `openpak-v*` tag.
- Run the E1 journey (sign in, friends, invite, cloud save) on Linux, Windows, macOS,
  Android.

## Pointers

- [`../prds/`](../prds/README.md) — emulator-wide PRDs (`emulators/prds/` in the workspace):
  emulator-integration-prd.md (E1), emulator-network-profile-prd.md.
- `OPENPAK.md` — this fork's own readme.
