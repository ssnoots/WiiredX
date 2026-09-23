# WiiredX

Use a wired Xbox One controller on your Wii U. Plug it in and play.



WiiredX is an [Aroma](https://aroma.foryour.cafe) plugin. It talks to the controller over USB and feeds input to games, either as the GamePad or as a Wii U Pro Controller.

## Features

* **Wired, so it just works.** Plug the controller into a USB port. Nothing to pair, nothing to re-pair.
* **Two modes.** Act as the GamePad (works in nearly every game) or as a Pro Controller on player slot 1–4.
* **Rumble**, in both modes, with adjustable strength.
* **Guide button opens the HOME Menu**, and the pad works inside it.
* **No mid-game screen dimming.** The console can't see the controller, so it would otherwise dim as though you'd walked away.
* **Configurable** from the Aroma plugin menu: button layout, stick deadzone, trigger sensitivity, and more.

## Requirements

* A Wii U running [Aroma](https://aroma.foryour.cafe)
* A wired Xbox One controller, or a wireless one with a USB data cable
* A USB cable that carries data

Tested with an Xbox One S controller (`045E:02EA`). Other Xbox One and Series controllers use the same protocol and should work, but some models need extra wake-up packets that aren't implemented yet. If yours doesn't work, reach out and I'll try and figure it out.

## Install

1. Download `wiiredx.wps` from [Releases](../../releases).
2. Copy it to `sd:/wiiu/environments/aroma/plugins/` on your SD card.
3. Put the card back in the console and boot into Aroma.
4. Plug in the controller. That's it.

To remove it, delete the file.

## Settings

Open the plugin menu with **L + D-pad Down + Minus** on the GamePad, then choose **WiiredX**. Settings apply immediately and are saved to the SD card.

|Setting|Options|Default|What it does|
|-|-|-|-|
|Use Xbox controller|on / off|on|Master switch|
|Act as|GamePad / Pro Controller|GamePad|How games see the controller|
|Pro Controller player slot|1–4|1|Which player slot to occupy in Pro Controller mode|
|Face buttons|By position / By label|By position|"By position" maps Xbox A (bottom) to Nintendo B (bottom), "By label" maps A to A.|
|Stick deadzone|0–40%|15%|How much stick movement around center to ignore|
|Trigger press point|5–95%|30%|How far LT/RT must be pulled to count as ZL/ZR|
|Guide button opens HOME Menu|on / off|on||
|Keep screen awake|on / off|on|Stops the console dimming while you're playing|
|Rumble|on / off|on|Self explanatory|
|Rumble strength|0–100%|55%|Xbox motors are stronger than the GamePad's 55% felt fine to me|

## Which mode should I use?

**GamePad mode** merges the controller into the GamePad's input, so it works in nearly every game, including ones that require the GamePad. The real GamePad keeps working at the same time, which matters for games that need the touchscreen. This is the default.

**Pro Controller mode** presents the controller as a separate Wii U Pro Controller on its own player slot, so it can be a second player alongside someone using the GamePad. Not every game accepts Pro Controllers, and some are fussier than others, so try GamePad mode first if a game ignores it.

## Known issues

* **A brief dropout after closing the HOME Menu.** The controller sometimes drops off the USB bus and reconnects as a new device, which takes about a second. WiiredX detects and recovers from this automatically. It's the console or the controller doing it, not the plugin.
* **The Guide button can't close the HOME Menu.** Only the real HOME button can, because the system detects it at a level plugins can't reach. Use the Close button on screen.
* **Pro Controller mode varies by game.** Games check for controllers in their own ways.
* **The touchscreen still needs the real GamePad.**
* **The system's controller pairing screen won't show the controller.** It isn't a real Bluetooth controller, so it isn't listed there.

## DEBUG

To watch the plugin's log output while it runs, listen on UDP port 4405 from a PC on the same network. On Windows:

```powershell
$u = New-Object System.Net.Sockets.UdpClient 4405
$e = New-Object System.Net.IPEndPoint (\[IPAddress]::Any, 0)
while ($true) { \[Text.Encoding]::ASCII.GetString($u.Receive(\[ref]$e)) }
```

## How it works

WiiredX opens the console's USB host stack directly, claims the controller's interface, sends the wake-up packets, and reads input reports on a background thread. A hook on the game's input functions then merges that state into what the game reads.

The GIP protocol details came from the Linux `xpad` driver.

## Credits

* [devkitPro](https://devkitpro.org) and [wut](https://github.com/devkitPro/wut)
* [WiiUPluginSystem](https://github.com/wiiu-env/WiiUPluginSystem) by Maschell
* The Linux `xpad` driver, for the Xbox controller protocol
* [Ristretto](https://github.com/ItzSwirlz/Ristretto), which showed how a plugin can open the HOME Menu
* [Bloopair](https://github.com/GaryOderNichts/bloopair), the Bluetooth equivalent, which is what you want if you'd rather go wireless

## License

MIT — see [LICENSE](LICENSE).

