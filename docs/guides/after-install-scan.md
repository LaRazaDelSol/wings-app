# After a download: asking ShadowMount to scan

A finished download is not a game ShadowMount knows about yet. ShadowMount registers games when it scans, and it will not scan while a game is running. Wings itself is mounted by ShadowMount, so while Wings is open ShadowMount counts it as the running game and waits. That is why a game often shows up only after you close Wings.

## What Wings does

1. When a download finishes (for an archive: after Pegasus has finished extracting it), Wings waits five seconds. Several downloads that finish together become one request.
2. It asks ShadowMount's local API to scan once. It talks only to `127.0.0.1` on this console. The port comes from `/data/shadowmount/config.ini` (`api_port`, default 10101); `api_enabled=0` there means Wings does not ask.
3. A banner appears at the top of the Downloads tab (not a popup):
   - **"Asked ShadowMount to find your new game. It waits while Wings is open. Close Wings to finish."** ShadowMount accepted the request but will run it only when Wings closes. Choose **Close Wings now** (it asks you to confirm first) or **Not now**.
   - **"ShadowMount is scanning now."** ShadowMount was free to scan. Wings then checks ShadowMount's game list every 5 seconds for up to 60 seconds and shows **"Registered: <title>"** when your game appears. If it does not appear in time, the banner says so and offers to close Wings.
   - **"ShadowMount's API was not found..."** The API is off, on another address, or ShadowMount is not running. Nothing is wrong with the download: ShadowMount finds the game on its next scan, for example after you close Wings.
4. Press Up from the first row to reach the banner's buttons. The banner is shown once per finished download and goes away when you dismiss it.

Wings never closes itself. Only the confirmed **Close Wings now** does, and it asks the system to close Wings the normal way (you return to the home screen).

## Turn it off

Settings > Wings > **After a download finishes**: **Ask ShadowMount to scan** (default) or **Do nothing**. With "Do nothing" Wings sends no request after downloads; you can still choose **Ask ShadowMount to scan now** from the Actions menu (Cross on a job) on the Downloads tab.

## Notes

- The scan cannot be forced while Wings is open; ShadowMount decides when it is safe.
- The title must be one ShadowMount can register (folders and images in its scan paths). If Pegasus installed it somewhere ShadowMount does not scan, it will not appear.
- Wings does not bundle or change ShadowMount. This uses its documented local API (drakmor/ShadowMountPlus, GPL-3.0). *(Not yet hardware-tested.)*
