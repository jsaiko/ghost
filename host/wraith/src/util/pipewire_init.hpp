// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// pw_init()/pw_deinit() are process-global (PipeWire's own docs: call once
// at startup, once at shutdown). Both PipeWire users -- the audio sink
// capture and the screencast frame capture -- go through this so the
// library is initialised exactly once whichever opens first. pw_deinit()
// is deliberately never called: it is only meaningful right before process
// exit, and wraith has no such hook (libva's and OpenSSL's global teardown
// aren't called either).
#pragma once

namespace wraith {

void ensure_pw_init();

} // namespace wraith
