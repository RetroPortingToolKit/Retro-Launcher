# Local game control for Direct mode

Set `RETCOMM_GAME_CONTROL_PORT` when starting a Direct `--boot` session to enable local game control. Use a port number from 1 to 65535 or `0` for an automatically selected port. The hub writes the actual port to `game-control-port` beside the session logs. The session pauses once its first picture is ready.

For unattended checks, set `RETCOMM_GAME_CONTROL_HIDDEN=1` to keep the launcher's window hidden. Use `SDL_AUDIODRIVER=dummy` and a test-owned `RETCOMM_HOME` for muted, isolated runs.

Connect to `127.0.0.1` using one local client. Send one ASCII command per line and read its entire reply before sending the next command. A reply is `ok FIELD LENGTH\n` or `error FIELD LENGTH\n`, followed by exactly `LENGTH` bytes. `FIELD` is the number of completed core advances since launch.

| Command | Result |
| --- | --- |
| `status` | `loading`, `paused`, or `playing` |
| `capture` | The current game picture as a binary PPM, without the launcher overlay |
| `step FRAMES BUTTONS LX LY RX RY` | Advance 1–10,000 fields with the specified seat-one input, then stay paused. The reply has an empty body. |
| `close` | Finish the Direct session using normal save and shutdown handling. The reply has an empty body. |

Pass decimal numbers and use `0` for neutral axes. `BUTTONS` uses the 16 generic `RCORE_PAD_*` bits. For N64, `1` is A, `4` is B, `64` is Z, and `1024` is Start. Axes range from -32768 to 32767. Positive `LY` moves the N64 analog stick up. The N64 core maps the right stick to C Buttons, with positive `RY` for C-Up. `step 3 0 0 0 0 0` advances three fields with neutral input.
