# Installing Veil

Veil (`veild`) is optional: one machine users log in to instead of a
single host. It shows each user the hosts they are entitled to, hands
the login to the one they pick, and serves an admin web UI and a browser
client ([Veil](../design/veil.md)). It is installed separately and never
by `make install`.

```sh
make                     # as your normal user
sudo make install-veil
```

`install-veil` puts down `veild`, its unit, `/etc/pam.d/veild` and
`/etc/ghost/veild.toml` (kept if present). The PAM service includes the
distro's stacks, probed from `/etc/pam.d` as for the host
([installing the host](host.md#build-and-install)); `PAM_STACK=debian`
or `system-auth` overrides it. It creates:

- the `veil` system user, with state in `/var/lib/veil`, in the `ghost`
  group so it can reach the PAM helper;
- ghostauth, the PAM helper, with its socket unit, its `ghostauth`
  account and its ticket key, unless `make install` already put them
  down on a machine that is also a host
  ([authentication](../design/login-and-sessions.md#authentication));
- the `ghost-admins` group;
- Veil's lobby certificate at `/etc/ghost/veil-{cert,key}.pem`, and the
  key Wisp thin clients use at `/etc/ghost/veil-wisp.key`;
- a self-signed web certificate at `/etc/ghost/veil-web-{cert,key}.pem`.

Back up the lobby pair and `/var/lib/veil/veil.db`: losing the key means
joining every host again. Settings are in
[configuration](../reference/configuration.md#veildtoml).

Open UDP 4442 (logins, hosts' channels, gateway sessions; `[lobby]
address` binds it to one address), TCP 443 (the
web UI and browser client) and UDP 443 (the browser client's
WebTransport sessions), then `sudo systemctl enable --now veild`.

## Administrators and users

There is no built-in administrator and no default password. An
administrator is any account in `ghost-admins`, signing in at
`https://<veil>/admin` with its own password:

```sh
sudo usermod -aG ghost-admins <user>
```

Users and passwords come from the Veil machine's own PAM, so each user
needs the same username and password on Veil and on every host they use.
sssd or LDAP on all of them is the way to keep that true.

## Joining hosts

In the admin UI's "Add host" page, or with `veild token` on the Veil
machine, get a join command and run it on the host as root, then restart
ghostd:

```sh
sudo ghostd join veil.example --token <id>.<secret>:sha256:<fingerprint>
sudo systemctl restart ghostd
```

Then give users or groups access on the device's page. Setting
`broker_only = true` under `[broker]` in the host's `ghostd.toml` stops
direct logins to it, so nobody can go around the entitlements.
`sudo ghostd leave` undoes a join. A host that has joined pins Veil's
certificate and Veil pins the host's, so re-joining is needed if either
is replaced ([trust](../design/trust.md)).

## Web certificate

The generated web certificate works behind a browser warning, and the
browser client's WebTransport never accepts it, so until you replace it
the client uses its WebSocket fallback. Point `[web] cert` and `key` in
`veild.toml` at a real certificate (Let's Encrypt or an internal CA);
`systemctl reload veild` picks up a renewal. With one in place,
`clients_use_web_cert = true` under `[lobby]` lets spectre-qt and Wisp's
greeter trust Veil without a first-use prompt as well.

## Clients

spectre-qt needs nothing special: point it at Veil and it asks which
host to use. With a web certificate configured, the browser client is
at `https://<veil>/`; its sessions always go through Veil
([browser client](../design/browser-client.md)). Thin clients boot
Wisp ([Wisp](../design/wisp.md)); `veild wisp-env` prints the lines its
boot server needs.
