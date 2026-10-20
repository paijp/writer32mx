# Notes for Claude

## Browser page (web/, k-keiei.jp/writer32mx/)

When you drive the board through the browser page (`w32ctl` on the VPS),
the user often has the page in the foreground on an Android phone and
cannot see Claude. So, every time:

- **Whenever you stop and the user should look at Claude** (a result, a
  question, an error), run `w32ctl wait "<one-line summary>"` before
  ending your turn: yellow banner + two short vibrations.
- **When the work is finished**, run `w32ctl close "<one-line summary>"`:
  green banner + one long vibration; the session then ends.
- Vibration works only after the user has tapped the page once; ask for a
  tap in the first message. `w32ctl events` shows a `vibrate` event with
  `ok` / `activated` for each attempt.
- After giving the user a session URL, end the turn (text is shown only
  when the turn ends); a reconnect does not need a new session.

See web/README.md for the rest.
