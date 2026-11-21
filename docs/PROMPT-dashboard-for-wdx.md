# Prompt for Claude Code: dashboard changes for the wdx project

Paste everything below the line into Claude Code, started in
`/home/u185352/Documents/Coding/DOCKER-Applications/docker-wdx-kbus-interface`.

---

## Task

Change the layout of the KBUS Manager page of this project
(`docker/manager/manager.html`) to the same layout as the KBUS Manager of the
MQTT sister project. Only the page layout changes: no gateway code, no WDX
paths, no behaviour of the page. Reference (read it first, don't copy
MQTT-specific parts):
`/home/u185352/Documents/Coding/DOCKER-Applications/docker-mqtt-kbus-interface/web/`
(`index.html`, `kbus.js`, `kbus.css`), and section 3.7 of its `README.md`.

## Changes

1. **COM1 moves from *Settings* to *Status*, as the first entry.**
   - `MENU`: Status becomes `[["serial", "COM1"], ["kbus", "KBUS"], ["modules", "Modules"]]`;
     Settings becomes `[["cycle", "KBUS"], ["login", "Login"]]`.
   - The page `<div class="wbm-page" id="page-settings-serial">` becomes
     `id="page-status-serial"` and moves in the HTML to the other Status
     pages (comment `<!-- Status / COM1 -->`).
   - Reason: the COM1 settings can't be changed on this page (CODESYS sets
     them with `FB_WdaSerial`), so they are status, not settings.
   - Because COM1 is now the first Status entry, `#/status` opens COM1.
2. **The COM1 panel title** "Settings (set by CODESYS)" becomes
   **"Communication Parameters"**, and the note above it ends with
   "this page only shows them (read only)". The values stay read only; the
   Monitor panel with the test send stays as it is.
3. **"KBUS Status" becomes "KBUS"**: in `MENU`, in the page heading
   `<h2 class="page">`, and in the HTML comment `<!-- Status / KBUS -->`.
4. **Check that nothing still refers to the old place**: search for
   `settings/serial`, `page-settings-serial` and `KBUS Status` in the page,
   the README and the docs, and update links and texts (README: the table of
   the KBUS Manager pages, COM1 now under *Status*; any "Settings > COM1").
5. **Keep as it is** (already the same in both projects): the I/O page (DO as
   ON/OFF buttons that show the state, DI as ON/OFF boxes, no lamps), the
   *Settings > KBUS* page, Parameters, Registers, Login.

## Check

- Syntax of the page script: extract the `<script>` of `manager.html` and run
  `docker run --rm -v <dir>:/w:ro node:20-alpine node --check /w/<file>.js`.
- Look at the page in headless Chrome
  (`google-chrome --headless=new --screenshot --window-size=1280,1000 <url>`)
  for `#/status/serial`, `#/status/kbus` and `#/settings/cycle`: the sub menu
  order, the headings and the panel title.

## Rules

- **Ask before deploying.** The old test controller 192.168.1.17 no longer
  exists; ask the user on which controller the wdx project runs. The
  controller 192.168.178.115 runs the **MQTT** project: never deploy the wdx
  gateway or its KBUS Manager there (both use the KBUS and `/kbus`).
- When the user has agreed: deploy only the manager container, then remove
  the old image on the controller (`docker image prune -f`).
- Commit and push only when the user asks.
- At the end, report what changed, the check results and what's left open.
