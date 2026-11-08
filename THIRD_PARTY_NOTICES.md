# Licenses and attribution

AudioExclusive combines the following source components. Original notices are retained in their source directories and release payloads.

- Apple Music implementation: AMExclusive contributors, MIT. See `backend-source/apple-source/LICENSE` and `THIRD_PARTY_NOTICES.md` in that directory.
- macOS companion: MIT, as marked in `backend-source/macos-source/AudioExclusive.mm`; see its adjacent `LICENSE`.
- NetEase implementation, installer and repository additions: GPL-3.0. See the root `LICENSE` and `backend-source/netease-source/licenses/`.
- MinHook: BSD-style license, copyright Tsuda Kageyu and contributors. Vendored in the NetEase source; Apple build fetches tag v1.3.4.
- rigtorp SPSCQueue: MIT. See the vendored component license under `backend-source/apple-source/third_party/`.
- Chromium Embedded Framework headers: BSD-style license; Chromium notices are retained under the NetEase licenses directory.
- Steinberg ASIO headers: retained ASIO SDK license under `backend-source/netease-source/licenses/ASIO-SDK-LICENSE.txt`.

Apple Music, NetEase CloudMusic and their frameworks are not distributed as part of this repository. Product names belong to their respective owners.
