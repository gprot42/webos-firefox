# Installing Firefox on a webOS TV

You need two things: the package, a file named like
`com.github.gprot42.geckotv_0.1.25_arm.ipk`, and a TV that lets you install
apps from outside LG's store. That is either a TV in LG's **Developer Mode**
or a **rooted** TV (one with the Homebrew Channel). Then pick one of the
three ways below.

The app has so far been used on rooted TVs. Developer Mode should work the
same way, but has not been tried with this app yet.

## 1. webOS Dev Manager (a program with a window; easiest)

[webOS Dev Manager](https://github.com/webosbrew/dev-manager-desktop) runs
on Windows, macOS and Linux and works with both Developer Mode and rooted
TVs.

1. Install and open Dev Manager, add your TV (it walks you through it).
2. Open the **Apps** page and choose **Install**, then pick the `.ipk` file.
3. Firefox appears in the TV's launcher. Open it from there.

## 2. LG's command-line tools (Developer Mode)

1. On the TV, install the **Developer Mode** app from LG's store, sign in
   with an LG developer account and turn Dev Mode on. LG's guide:
   <https://webostv.developer.lge.com/develop/getting-started/developer-mode-app>
2. On your computer, install LG's tools (`npm install -g @webos-tools/cli`)
   and register the TV once with `ares-setup-device` (it asks for the TV's
   IP address and the passphrase the Developer Mode app shows).
3. Install and start the app:

   ```sh
   ares-install --device tv com.github.gprot42.geckotv_0.1.25_arm.ipk
   ares-launch  --device tv com.github.gprot42.geckotv
   ```

Developer Mode switches itself off after 50 hours unless you renew it in
the Developer Mode app (Dev Manager can renew it for you), and apps
installed this way go with it.

## 3. A rooted TV, over SSH

With the Homebrew Channel's SSH server turned on, the TV installs the file
itself. Replace `TV` with the TV's IP address:

```sh
scp com.github.gprot42.geckotv_0.1.25_arm.ipk root@TV:/tmp/
ssh root@TV
luna-send -n 1 luna://com.webos.appInstallService/dev/install '{"id":"com.github.gprot42.geckotv","ipkUrl":"/tmp/com.github.gprot42.geckotv_0.1.25_arm.ipk","subscribe":false}'
```

`{"returnValue":true}` means the install started; the app shows up in the
launcher a few seconds later. Some TVs only have an older `luna-send-pub`,
which takes the same address and message but without the `-f` and `-n`
options in the same places: `luna-send-pub -n 1 '<address>' '<message>'`.

To remove the app the same way:

```sh
luna-send -n 1 luna://com.webos.appInstallService/dev/remove '{"id":"com.github.gprot42.geckotv"}'
```

## Afterwards

- **Updating:** install the newer `.ipk` the same way; it replaces the old
  version. Removing the app deletes its folder, and the browser's bookmarks
  and logins with it.
- **Removing:** hold the app's icon in the launcher and choose remove, or
  use the command above.
- **If it does not start** (or shows a black screen), and the TV is rooted,
  run this once and send the file it names, `/tmp/geckotv-report.txt`,
  together with what the TV showed while it ran:

  ```sh
  sh /media/developer/apps/usr/palm/applications/com.github.gprot42.geckotv/diagnose.sh
  ```
