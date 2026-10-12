# Pegasus auto-load

When Pegasus DL is not running, Wings can load it for you. This is optional and off until you choose it.

## What you see

The "Waiting for Pegasus DL" screen shows three choices under the message as soon as it appears, with a small spinner and "Looking for Pegasus DL..." for the first ten seconds. You can press one at once: Wings downloads (if needed), verifies and sends, and cancels quietly if Pegasus DL answers first. Auto-Load itself still waits the ten seconds before sending.

- **Enable Pegasus-dl.elf Auto-Load**: loads it now and, from then on, once each time Wings starts and Pegasus is not running.
- **Load Pegasus-dl.elf once**: loads it now. Wings asks again next time.
- **I will load Pegasus-dl.elf myself**: Wings stops offering and remembers that. The waiting message stays.

Nothing is offered while Pegasus answers. If a load fails, or Pegasus DL stops responding after it had been running, the screen offers **Retry** and the gray **I will load Pegasus-dl.elf myself**; the gray one then only hides the offer until Pegasus answers again and does not change your start-up setting. Wings never resends by itself after Pegasus DL has stopped, even with Auto-Load on; only Retry sends.

## What it does

1. Reads the official release list from github.com/pegasus-ps5/pegasus-dl (needs the same network switch as cover art).
2. Picks the newest release inside the range Wings was tested with; otherwise the newest overall, and says it is untested.
3. Downloads `pegasus_dl.elf` once, checks it against the SHA-256 GitHub publishes, and saves it under `/data/wings/pegasus/<version>/`.
4. Before every send, checks the saved file's SHA-256 again. A file that no longer matches is deleted and fetched again.
5. Sends the file to the payload loader at `127.0.0.1` port `9021` (this console only), then keeps checking for Pegasus DL.

Auto-Load sends at most once per launch of Wings. It never moves a saved file to a newer release by itself.

## What it never does

- It does nothing if Pegasus DL is already running.
- It never writes into any payload manager's folder.
- It never sends anywhere but `127.0.0.1:9021`, and it never starts anything else.
- If port 9021 refuses the connection (the payload loader is often still starting after the console boots) it shows "Waiting for the payload loader to start..." and tries again every 3 seconds for up to 30 seconds. Only then it says "No payload loader found on port 9021. Load Pegasus DL with your loader." and offers Retry.
- With no internet and nothing saved, it says "Needs the internet once to fetch Pegasus DL" and offers only the manual choice.

## Turn it off

Settings > Pegasus DL > **Pegasus DL start-up**: Ask, Auto-load, or I load it myself. Selecting the row cycles through them. "I load it myself" stops all offers and all sends.

Not yet checked on a real console.
