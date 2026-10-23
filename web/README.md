# web/ - use writer32mxcdc from a browser tab

`writer32mxcdc.c` is a plain USB CDC device (`04d8:000a`), so a browser can
drive it directly: write an Intel HEX file to the target, reset it, send text
to its UART and watch its output - no terminal program or driver install.

Chrome / Edge only (Firefox and Safari have neither API):

- **Web Serial** (desktop Chrome/Edge on Windows, macOS, Linux): the OS CDC
  driver stays in charge; the page opens the port at 115200.
- **WebUSB** (Chrome on Android, or the "WebUSB で接続" button): the page
  claims the CDC data interface (interface 1, bulk 0x02/0x82) itself. On a
  desktop the OS CDC driver usually holds that interface, so use Web Serial
  there.

The page must be served over HTTPS (or `http://localhost`).

## Standalone

Serve `index.html` and `app.js` from any static host (or `python3 -m
http.server` and open `http://localhost:8000/`), then:

1. 「ライターに接続」 and pick `cdc0008`. Chrome remembers the permission,
   so later loads connect by themselves.
2. Pick a `.hex` and 「書き込み」. The page checks the records and that the
   end-of-file record is present (the writer writes the last block, the boot
   flash and the configuration words only on it), sends the file and waits
   for the writer's `writing` ... `run`.
3. 「リセット (:FF)」 resets the target; the text box sends to the target's
   UART with LF (`\n`) appended; HEX files are also sent with LF only. A line starting with `:` is refused, since the writer would take it
   for a HEX record.
4. 「ログ保存」 saves everything received.

## Offline (one file)

`writer32mx.html` is `index.html` with `app.js` inlined: download it and
open it from disk in Chrome / Edge, no network needed (pages opened from
`file://` may use Web Serial / WebUSB).  It works like the standalone
page.  Regenerate it with `python3 make-offline.py` after changing
`index.html` or `app.js`.

Download: https://github.com/paijp/writer32mx/raw/main/web/writer32mx.html

On Android, Chrome generally cannot open a downloaded HTML file from
storage, so use the hosted page there.

### Why two forms (index.html + app.js, and writer32mx.html)

The source is `index.html` + `app.js`; `writer32mx.html` is generated
from them and committed so it can be downloaded directly.

- The server (server.py) sends a Content Security Policy with
  `script-src 'self'`: only script files from the same server run, so a
  script that gets into the page (for example from the target's serial
  output, if it were ever shown as HTML) is blocked.  That needs the
  script in a separate file, `app.js`.  Any static server can use the
  same policy with no extra work.
- For offline use one file is easiest to hand around, so the script is
  inlined into `writer32mx.html`.  Opened from disk there is no server and
  no CSP; the page talks to nothing but the USB device.
- A single inline file could also be served under CSP by allowing its
  script by hash (`script-src 'sha256-...'`), but then the hash must be
  recomputed on every edit (by the server, or by hand for other hosts),
  and any second `<script>` or `onclick=` would be blocked silently.  We
  tried that and went back to this layout as simpler to maintain.
- `<noscript>` cannot be used to fall back from the inline script to
  `app.js`: it only applies when JavaScript is disabled, not when CSP
  blocks a script.

So: edit `index.html` / `app.js`, then run `python3 make-offline.py`
and commit `writer32mx.html` with them.

## Relay mode (driven from a server, e.g. by Claude)

Opened as `https://<host>/writer32mx/#t=<token>`, the page hides the manual
controls (only the connect button, messages and log remain), polls a
small relay for jobs and uploads the log and what it did, so someone with
shell access to the server (for example Claude through an `exec_command`
connector) can flash and run the target on the user's desk while the user
only keeps the tab open.

| File | Runs where | What it is |
|---|---|---|
| `index.html`, `app.js` | browser | the page (public; works without a token) |
| `server.py` | server, `python:3.13-slim` in podman, stdlib only | relay: sessions, jobs, log/event upload, file download |
| `w32ctl` | server, as root | mint sessions, post jobs, read logs |
| `nginx.conf` | server nginx | the `location /writer32mx/` block |

Setup: put `index.html`, `app.js`, `diag.html`, `diag.js`, `server.py` in `/srv/writer32mx/app/`,
`w32ctl` and `diagjob` in `/srv/writer32mx/` (mode 700), add the nginx block, and set
`W32_URL` (e.g. `https://example.com/writer32mx/`) for `w32ctl`. `W32_DATA`,
`W32_PORT` (5311) and server.py's `W32_PREFIX` (`/writer32mx`) can be
changed the same way.

```
w32ctl new [minutes]                   mint a session (default 60), start the container, print URL
w32ctl msg <text>                      progress message on the page
w32ctl wait [text]                     highlighted "look here" message
w32ctl close [text]                    end the session
w32ctl reset [sec]                     send :FF (reset the target) and log sec seconds (default 10); run is an alias
w32ctl flash <file.hex> [sec]          write the image, then log sec seconds (no :FF needed: the writer releases reset when writing ends)
w32ctl jobwait [sec]                   block until the current job ends (default 50)
w32ctl log [bytes]                     tail of the session's log (log.bin)
w32ctl events [n]                      last n events the page reported (JSON lines)
w32ctl status                          control.json, whether the page was opened, time left
w32ctl askreload [text]                ask the user to reload by hand
w32ctl reload                          make open pages reload (after editing app.js)
(reset and flash take --tx TEXT, escapes like \r\n allowed: sent 1 s after the target starts)
```

The page uploads everything received while connected, not only during
jobs; each job starts with a `=== job jN <kind> <time> ===` marker and a
send is marked `=== tx N bytes ===`. A flash job fails on the writer's
`IDCODE invalid` / `FCBUSY stuck`, or if `run` does not follow `writing`
within 5 minutes.

Typical loop:

```
cd /srv/writer32mx && ./w32ctl flash img/foo.hex 8 --tx 'hello\r\n' && ./w32ctl jobwait 45
./w32ctl log 2000
```

### USB diagnostics page (diag/)

`https://<host>/writer32mx/diag/#t=<token>` (same session token) runs
WebUSB / Web Serial steps sent from the server and reports each result,
for finding out why a browser cannot connect.  The user taps
「USB デバイスを選ぶ」 (or 「シリアルを選ぶ」) once, since a permission can
only be granted from a tap; everything after that is driven by
`diagjob`:

```
./diagjob '[{"op":"open"},{"op":"claim","n":0},{"op":"claim","n":1},
            {"op":"out","ep":2,"text":":FF\r\n"},{"op":"in","ep":2,"ms":3000,"loop":true}]'
```

Operations: `info`, `open`, `close`, `reset`, `forget`, `config`
(`value`), `claim` / `release` (`n`), `alt` (`n`, `alt`), `ctrlOut` /
`ctrlIn` (`requestType`, `recipient`, `request`, `value`, `idx`, `data`
hex / `len`), `out` (`ep`, `text` or `hex`), `in` (`ep`, `len`, `ms`,
`loop`), `serialOpen` (`baud`), `serialWrite`, `serialRead` (`ms`),
`serialClose`, `wait` (`ms`).  A failed step stops the job unless it has
`"cont": true`.  Only these operations exist; nothing from the server is
run as code.

Found with it: on Android 16 `claimInterface(1)` (the CDC data
interface) fails with "Unable to claim interface" unless interface 0 is
claimed first, so app.js now claims the control interface before the
data interface.

The page's `loaded` event (and diag `info`) also reports `os`,
`osVersion` and `model` from `navigator.userAgentData`, since the user
agent string always says "Android 10; K".

### Session flow

**Rule for Claude:** the user may be watching only this page (e.g. Android
with the browser in the foreground). Every time Claude stops for the user,
it runs `w32ctl wait "<summary>"` first, and `w32ctl close "<summary>"`
when done; `w32ctl new` and `jobwait` print a reminder. See ../CLAUDE.md.


1. `w32ctl new 30` prints the URL. When Claude hands it out, it should end
   its turn right after showing it (the Claude app shows a turn's text only
   when the turn ends) and come back via a scheduled check-in or the user's
   next message, then wait for a `claimed` event in `w32ctl events`.
2. The user opens the link in Chrome and, the first time only, connects.
3. Jobs, `jobwait`, `log`, repeat.
4. `w32ctl wait "..."` (yellow banner, two short vibrations) when the user
   should look at Claude; `w32ctl close` (green, one long vibration) at the
   end. On phones vibration needs one tap on the page first.

### Security

- The token is the only credential: 32 random characters minted by `w32ctl`
  on the server. It travels in the URL fragment (never sent to the server, so
  not in logs or Referer), is moved to `sessionStorage` and removed from the
  address bar, and is sent as `X-Token`.
- Every `api/` request with a bad or expired token is a plain 404. Sessions
  expire (default 60 min); `close` cuts them to 10 min, read-only.
- Sessions are stored under `sha256(token)`. The server exits a minute after
  the last session expires; the container is `--rm`.
- CSP: scripts from self only, connect to self only, no framing.

## Tested

With a mock port in headless Chromium: standalone load, relay session,
flash job (HEX check, wait for `writing`/`run`, `--tx`), reset job, log and
event upload. On a real writer32mxcdc2 with WebUSB on Android 16 and
Android 8.1 Chrome.
