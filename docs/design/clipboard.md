# Clipboard

Text-only, two-way clipboard sync behind the `clipboard` capability
(gdp-spec.md §7.9). Code: `host/wraith/src/session/clipboard_sync.*`,
`host/wraith/src/screencast/data_control_clipboard.*` and
`gnome_clipboard.*`, and libgdp's `clipboard.hpp`, which spectre shares.

## Mechanisms

`GdpSession` talks to one `ClipboardSink` from `SessionServices`, so the
session layer doesn't know which mechanism is underneath. The sink lives
as long as the compositor, not the client: the desktop's clipboard
changes whether or not anyone is attached, which is why a newly attached
session pushes its current text once.

| Backend | Mechanism |
|---|---|
| `screencast-ext`, `screencast-kwin` | `ext-data-control-v1` (`data_control_clipboard.*`) |
| `screencast-gnome` | mutter's `org.gnome.Mutter.RemoteDesktop.Session` clipboard methods (`gnome_clipboard.*`) |

- **Data control.** The `ext_` protocol, not
  `zwlr_data_control_unstable_v1`: kwin exports only
  `ext_data_control_manager_v1`, while labwc and wlroots export both.
  `set_selection` takes effect without keyboard focus, which a headless
  client with no surface of its own needs.
- **GNOME** uses the `RemoteDesktop.Session` object `GnomeRemoteSession`
  already holds for `ConnectToEIS`: `EnableClipboard`, `SetSelection`,
  `SelectionRead`, `SelectionWrite`/`SelectionWriteDone`, and the
  `SelectionOwnerChanged` / `SelectionTransfer` signals, the API
  gnome-remote-desktop uses. mutter bridges it to Xwayland, so X11 apps
  are covered.

  `SelectionOwnerChanged`'s options wrap the mime list in a 1-tuple,
  `{'mime-types': <(['text/plain', ...],)>, 'session-is-owner': <false>}`,
  which the introspection XML (`a{sv}`) doesn't show: the variant's
  signature is `(as)`, not `as`, and entering it as `as` fails with
  `-ENXIO`. `SetSelection` takes a plain `as`. The reader enters each
  option with the signature it declares, steps into a struct when there
  is one, and skips options it doesn't know.

## Shared rules

In libgdp's `clipboard.hpp`, so spectre applies the same ones:

- **Mime types.** The preference and offer set is
  `text/plain;charset=utf-8`, `text/plain`, `UTF8_STRING`, `STRING`,
  `TEXT` (all UTF-8 except `STRING`, which is Latin-1). The whole list
  is offered, or pasting into older GTK and X11 apps silently fails.
- **Echo suppression.** Every mechanism reports a selection change when
  wraith itself set the selection; the text that last crossed the wire in
  each direction is remembered so it never comes back as a new change.
  A client's text is recorded before it is written locally.

Every transfer goes through a pipe drained or filled off the event loop
(`ClipboardPipes`), never inline, so a client that asks for the
selection and stalls can't freeze the compositor.

## The capability is settled before the sink exists

On the screencast backends the clipboard mechanism opens only once the
captured compositor is up, a second or two after wraith reports ready.
ghostd redirects the client the moment wraith reports, so the session
may be accepted, and its capabilities fixed, before `SessionServices`
has a sink. Capabilities can't be added after `SessionAccept`.

So the host promises a sink (`set_clipboard_expected()`) before its
compositor starts and retracts the promise only if the mechanism turns
out to be missing. `set_clipboard()` then wires an already-active
session to the sink when it arrives, with the one-time push, and text a
client sends in that window is held rather than dropped. If the
mechanism never appears, a session accepted in that window keeps an
inert capability; later sessions don't offer it.

## Limitations

- Text only: no images, file lists or primary selection.
