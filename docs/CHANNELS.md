# dvbtx channel — channels changed while the transmitter runs

Reference written to become the `dvbtx-channel(1)` man page of the Debian package.

## NAME

`dvbtx channel` — list, add, remove, rename, renumber channels and change their source, while
dvbtx is on air, without touching the other channels.

## SYNOPSIS

```
dvbtx channel [-c FILE] [--url URL] [--token TOKEN] [--offline] COMMAND ...

dvbtx channel list [--json]
dvbtx channel add NAME SOURCE [--number N] [--mux MUX] [--sid SID] [--backup SRC | --no-backup]
                              [--option KEY=VALUE]...
dvbtx channel remove CHANNEL
dvbtx channel rename CHANNEL NEW_NAME
dvbtx channel renumber CHANNEL NUMBER [--swap]
dvbtx channel source CHANNEL SOURCE [--backup SRC | --no-backup] [--option KEY=VALUE]...
dvbtx channel set CHANNEL KEY=VALUE...
```

`channels` is accepted for `channel`, `rm` and `delete` for `remove`. The common options may be
given before or after the command.

## DESCRIPTION

A channel is a service of a multiplex (a `[[carrier.service]]` of the configuration). While dvbtx
runs, `dvbtx channel` sends the change to it through its web API (`web.listen`): the change is
applied at once to that channel only, and written back to the configuration file, so that the next
start gives the same channels. The other channels of the multiplex, and the other multiplexes, are
not interrupted: no packet lost, no extra delay (measured: the longest gap between two video packets
of a neighbouring channel stays at 25-33 ms, as without change).

When nothing answers on the web address, dvbtx is not running: the configuration file alone is
edited, and the change is applied at the next start (or at once with `systemctl reload dvbtx` if it
was in fact running with its web page disabled).

**CHANNEL** designates a channel by its number (LCN: `5`), its service_id (`0x0105`, `sid:261`) or
its name (`"France 24"`, any case; a name made of digits is taken as a number).

### What each change does on air

| change | on air |
|---|---|
| `add` | new service in the PAT (new version), SDT, NIT (number), EIT; its source starts, the backup plays meanwhile (after `switch_timeout_ms`) |
| `remove` | out of the PAT, SDT, NIT, EIT; its source is stopped (a backup shared with other channels is kept) |
| `rename` | SDT (the TV shows the new name at its next scan of the SDT, usually within seconds) |
| `renumber` | NIT logical_channel_descriptor (most TVs only reorder at a new channel scan) |
| `source` | **no cut**: the new source starts beside the old one; the channel switches to it on a video key frame due at that moment (or, without video, as soon as it has data due), the old source plays until then (15 s at most, then the channel goes on as usual: its backup if the new source does not come) |
| `--backup` | the new backup is used from the next failure of the source (at once if the channel is on its backup) |
| `set` | any other service setting: a setting of the source (`ytdlp_format`, `encode`, `readrate`, `map`...) restarts the source like `source` (no cut); an engine setting (`switch_timeout_ms`, `return_ms`, `max_late_ms`, `early_ms`, `pcr_max_ms`, `pmt_period_ms`, `priority`, `pid_base`) re-creates the service (cut of a few seconds) |

The service_id and the multiplex of a channel cannot be changed: remove it and add it again.

### Numbers, service_id and PIDs stay where they are

By default the service_id, the number and the PIDs of a channel follow its position in the file.
TVs identify a channel by its service_id: a removal must not shift the others. So every structural
change (`add`, `remove`, `renumber`) writes `sid`, `lcn` and `pid_base` explicitly in the services
of the file that relied on the defaults. A new channel takes the first free service_id of its
multiplex (TSID × 256 + n), the first free number and the first free PID range (32 PIDs from 0x100).

### The configuration file

Only the `[[carrier.service]]` blocks concerned are rewritten; comments and layout of the rest are
kept, the previous version is kept as `FILE.bak`, and the result is read back: if it does not say
exactly what was meant (unusual layout, such as a setting given in a `[carrier.service.encode]`
sub-table changed into another form), the file is left as it was and the change is only live
(`warning: not saved ...`).

The file is not written when it differs from what is running (edited by hand meanwhile: apply your
edit first with `systemctl reload dvbtx`), or when the channels come from the command line
(`--service`, `--set carrier.N.service...`).

### Editing the file by hand

`systemctl reload dvbtx` (SIGHUP) re-reads the file and applies the differences of the channels in
the same way, channel by channel (the EPG and SI tables are re-read too). Changes of the radios or
multiplexes need a restart (logged, channels then left as they are); other settings outside the
channels are logged as needing a restart. Give `sid` explicitly to the channels you move or remove
by hand, or the following ones change service_id.

## COMMANDS

`list [--json]`
: the channels: number, service_id, name, multiplex, what is on air (`primary`, `backup`, `none`),
  source and backup. From the file when dvbtx is not running.

`add NAME SOURCE`
: `SOURCE` is any source dvbtx reads (file, `dir:`, URL, HLS, `ytdlp:`, `twitch://`, `m3u:`,
  `udp://`, `rtmp://`, `srt://`, `v4l2:`, `lavfi:`...). `--number N` (default: the first free one),
  `--mux MUX` (index or name of the multiplex; required when there are several), `--sid SID`,
  `--backup SRC` (default: `defaults.backup`) or `--no-backup`, `--option KEY=VALUE` for any
  service setting (repeatable; values as in TOML: `encode=true`, `ytdlp_format=b`, `priority=7`).

`remove CHANNEL`
: removes the channel.

`rename CHANNEL NEW_NAME`
: 1 to 60 characters, no quotes or backslashes.

`renumber CHANNEL NUMBER [--swap]`
: refused if the number is taken, unless `--swap`: the other channel then gets the old number.

`source CHANNEL SOURCE [--backup SRC | --no-backup] [--option KEY=VALUE]...`
: plays another source on the channel without cutting it. `--option` sets the service settings
  that go with the new source (`KEY=` with nothing removes one, e.g. a `ytdlp_format` that does not
  apply to the new source).

`set CHANNEL KEY=VALUE...`
: changes any setting of the channel (`KEY=` removes it, back to the default).

## OPTIONS

`-c, --config FILE`
: configuration file (default `/etc/dvbtx/dvbtx.toml`): its `web.listen` and `web.token` are used
  to reach the running dvbtx, and it is the file edited when dvbtx is not running.

`--url URL`
: web address of the running dvbtx (default `http://` + `web.listen`, `0.0.0.0` read as
  `127.0.0.1`).

`--token TOKEN`
: `web.token` of the running dvbtx (also `DVBTX_TOKEN` in the environment).

`--offline`
: edit the configuration file only, even if dvbtx is running (to prepare several changes, then
  `systemctl reload dvbtx`).

## WEB PAGE AND API

The web page has a **+ Ajouter une chaîne** button per multiplex and **Modifier** / **✕** buttons
per channel (name, number with swap, source, backup); the result of each change is shown, and
whether it was written to the file.

```
GET    /api/channels                  [{"lcn", "sid", "name", "source", "backup", "carrier", "on", ...}]
POST   /api/channels                  {"name", "source", "carrier"?, "lcn"?, "sid"?, "backup"?, options...}
PATCH  /api/channels/<channel>        {"name"?, "lcn"?, "swap"?, "source"?, "backup"?, options...}
DELETE /api/channels/<channel>
```

`<channel>` as CHANNEL above, URL-encoded. `"backup": ""` (or `false`) removes the backup, an option
set to `null` is removed. Answer `{"ok": true, "sid", "changes": [text], "detail": [{"what", "name",
"from", "to"}], "saved", "warning"?}`, or `{"ok": false, "error"}` with status 400 (403, 401, 415:
see below).

## SECURITY

The web API can change what is broadcast, so:

- requests that change something are refused when they come from another web site (`Origin`
  checked: a page you visit cannot drive the transmitter through your browser) and must send JSON
  (`Content-Type: application/json`);
- `web.token = "..."`: these requests then need `Authorization: Bearer <token>` (the page asks for
  it once, `dvbtx channel` reads it from the configuration file or `--token`). Set one when
  `web.listen` is not `127.0.0.1`;
- sources that run a command or read the standard input (`exec:`, `pipe:`, `stdin:`) are refused
  from the API unless `web.allow_exec_sources = true`; they stay possible in the file.

## FILES

`/etc/dvbtx/dvbtx.toml`
: configuration, rewritten by the changes (it is a conffile of the package: an upgrade that brings
  a new version of it asks what to do, as for any edit).

`/etc/dvbtx/dvbtx.toml.bak`
: the version before the last change.

## EXAMPLES

```bash
dvbtx channel list
dvbtx channel add "France 24" "http://provider.example/live/user/pass/976" --number 3
dvbtx channel add "Sky News" "ytdlp:https://www.youtube.com/@SkyNews/live" \
      --option 'ytdlp_format=bv*[vcodec^=avc1][height<=720]+ba/b[height<=720]'
dvbtx channel source "Sky News" "https://example.org/sky/index.m3u8" --option ytdlp_format=
dvbtx channel rename 3 "France 24 FR"
dvbtx channel renumber "France 24 FR" 1 --swap
dvbtx channel set 3 epg_id=France24.fr
dvbtx channel remove 0x0103
dvbtx channel --offline -c /etc/dvbtx/dvbtx.toml add "Mire" lavfi:smptehdbars && sudo systemctl reload dvbtx
curl -s -X PATCH -H 'Content-Type: application/json' -d '{"name": "Info"}' http://127.0.0.1:8080/api/channels/3
```

## SEE ALSO

`dvbtx run`, `dvbtx check`, `packaging/dvbtx.toml` (every setting), `README.md`.
