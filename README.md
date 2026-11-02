# CYD Mini OS
Apps: Clock+Weather (Surat), WiFi Toolkit (scan/channels/meter/speed test), Games (Snake, Pong, 2048),
Draw (pixel art, 4 save slots in flash), Focus timer, Hardware test, Setup, Info.

## Flash from the browser (no installs)
1. Create a free GitHub repo, upload ALL files in this folder (keep the .github folder).
2. Repo Settings > Pages > Source: "GitHub Actions".
3. Actions tab > wait for "Build firmware and publish web flasher" (about 3-5 min).
4. Open https://<your-username>.github.io/<repo-name>/ in Chrome/Edge, plug in the board, click Install.

Change weather city: WX_LAT / WX_LON in src/main.cpp. Timezone: configTime(19800,...) (seconds from UTC).
