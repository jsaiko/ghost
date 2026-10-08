# SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
# SPDX-License-Identifier: GPL-3.0-only

# Wisp: show where we are pointed and which Veil certificate we trust.
cmdline() { tr ' ' '\n' < /proc/cmdline | sed -n "s/^$1=//p"; }
echo "Wisp thin client. Veil: $(cmdline veil)"
cert=$(cmdline veil_cert)
echo "Veil cert pin: ${cert:-MISSING (clients will refuse to connect)}"
