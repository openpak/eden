# Next session — eden

Updated 2026-09-24.

Eden (yuzu family, Qt, plus Android) with the OpenPak layer on the shared `openpak-client` C++
library; Eden keeps only its glue. OPENPAK.md is the fork readme (Android layer included).

Current status 2026-09-24: latest tag `v0.6.3` (v0.6.0/v0.6.1 on 09-23, v0.6.2/v0.6.3 on
09-24; earlier `v0.3.x-openpak` tags are 09-14). Since v0.6.0: the signed redirect ceiling
(openpak-client e180a57), the CTR Demonware key IPS, pinned-NPLN instruction changes (SMB
Wonder 1.2.1), `LoadIdTokenCacheDeprecated` hands titles the OpenPak id_token, catalogue
labels every regional title id.

## Where things stand

- Full Switch parity with Ryujinx via `openpak-client` as of 2026-09-23
  (`../prds/emulator-integration-prd.md` §2a): sign-in, profiles, presence, friends, requests,
  blocks, invitations send/receive (library picker), cloud saves, OpenPak window and menu,
  compatibility list, network profile, title online path, BCAT, crash reports, guest TLS
  (OpenSSL on every OS).
- Releases: CI only on `v*.*.*` tags (+ manual dispatch) on GitHub-hosted ubuntu-24.04;
  `release.yml` creates the release, the Linux x86_64 AppImage (+ zsync, tar.zst) and the
  Android APK attach to it. No Windows/macOS workflow.

## Next steps

- Windows and macOS: no CI build and no verification yet.
- The startup choice awaits the Configure page (E2).
- Follow Ryujinx: every Ryujinx change is ported here through `openpak-client` (§2a).

## Scratch (research and throwaway work)

Decompiles, Ghidra projects, dumps, exefs/romfs extracts, packet captures,
strace and emulator logs, probe harnesses: put them in
`~/REPOS/Openpak/scratch/<topic>`. That folder is a local mount of the media pool,
outside every repository, so nothing in it is committed. Never use `/tmp` (a
shared 15 GB RAM disk) or elsewhere on `/home` for this. Keys and signing
material never go there. Rule: `docs/playbooks/conventions.md` in the workspace.
