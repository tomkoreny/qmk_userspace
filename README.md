# QMK Userspace

This is a template repository which allows for an external set of QMK keymaps to be defined and compiled. This is useful for users who want to maintain their own keymaps without having to fork the main QMK repository.

## Howto configure your build targets

1. Run the normal `qmk setup` procedure if you haven't already done so -- see [QMK Docs](https://docs.qmk.fm/#/newbs) for details.
1. Fork this repository
1. Clone your fork to your local machine
1. Enable userspace in QMK config using `qmk config user.overlay_dir="$(realpath qmk_userspace)"`
1. Add a new keymap for your board using `qmk new-keymap`
    * This will create a new keymap in the `keyboards` directory, in the same location that would normally be used in the main QMK repository. For example, if you wanted to add a keymap for the Planck, it will be created in `keyboards/planck/keymaps/<your keymap name>`
    * You can also create a new keymap using `qmk new-keymap -kb <your_keyboard> -km <your_keymap>`
    * Alternatively, add your keymap manually by placing it in the location specified above.
    * `layouts/<layout name>/<your keymap name>/keymap.*` is also supported if you prefer the layout system
1. Add your keymap(s) to the build by running `qmk userspace-add -kb <your_keyboard> -km <your_keymap>`
    * This will automatically update your `qmk.json` file
    * Corresponding `qmk userspace-remove -kb <your_keyboard> -km <your_keymap>` will delete it
    * Listing the build targets can be done with `qmk userspace-list`
1. Commit your changes

## Howto build with GitHub

1. In the GitHub Actions tab, enable workflows
1. Push your changes above to your forked GitHub repository
1. Look at the GitHub Actions for a new actions run
1. Wait for the actions run to complete
1. Inspect the Releases tab on your repository for the latest firmware build

## Howto build locally

1. Run the normal `qmk setup` procedure if you haven't already done so -- see [QMK Docs](https://docs.qmk.fm/#/newbs) for details.
1. Fork this repository
1. Clone your fork to your local machine
1. `cd` into this repository's clone directory
1. Set global userspace path: `qmk config user.overlay_dir="$(realpath .)"` -- you MUST be located in the cloned userspace location for this to work correctly
    * This will be automatically detected if you've `cd`ed into your userspace repository, but the above makes your userspace available regardless of your shell location.
1. Compile normally: `qmk compile -kb your_keyboard -km your_keymap` or `make your_keyboard:your_keymap`

Alternatively, if you configured your build targets above, you can use `qmk userspace-compile` to build all of your userspace targets at once.

## Typing shortcuts

The QWERTY, Colemak-DH and plain typing layers support these 45 ms chords:

- `V+M`: arm Czech accents for the next key.
- `C+,`: switch QWERTY and Colemak-DH.
- `C+M`: toggle Caps Word for identifiers such as `CONSTANT_NAMES`.

These chords are disabled while gaming, using another functional layer,
or while Czech accents are armed. Home-row modifiers retain their existing
200 ms tapping term and mappings.

## OLED dashboard and pages

Flash the same firmware to both halves before testing the split displays.
The USB-connected half shows the dashboard; the other half shows contextual
hints or Linux status. The dashboard separates the base layout from the
active layer, distinguishes held and one-shot modifiers, and shows Czech
accent arming, Caps Word, lock indicators, and the F13–16/F17–20 gaming bank.
Modifier columns are Ctrl, Shift, left Alt, Super/GUI, and right Alt/AltGr.

Press `X+.` together on a typing layer to cycle the secondary display:
`Auto` → `Hints` → `Host` → `Learn` → `Pal` → `Auto`.
The chord uses the tap side of the existing AltGr keys; their mappings and
hold timing are unchanged. Page selection is not saved across reboots.

`Auto` shows layer hints on functional/gaming layers and Linux status on
typing layers. `Learn` shows the actual QWERTY or Colemak-DH positions.
`Pal` shows the small pixel companion; it also appears below the other
pages and reacts briefly to keypresses rather than animating continuously.

Both displays dim after 15 seconds without input and turn off after
30 seconds. Input wakes them. Host-status updates do not keep them awake.
Host data expires after five seconds without updates; a disconnected split
link is identified rather than leaving its state marked live.

## Flash the Liatris Aurora Corne on Linux

Connect one half by USB in normal keyboard mode, then run `./deploy.sh`.
The script builds `miryoku_553`, asks for sudo authentication while the
keyboard still works, and sends a Raw HID command to enter BOOTSEL.
No reset-button press is needed once trigger support is installed.

For the first installation on a half without trigger support, run
`./deploy.sh --manual`. Wait for `READY`, then double-tap reset on the
USB-connected half and leave it connected. This mode is also available
for manual recovery.

The script waits for the UF2 drive on the selected keyboard's USB port,
copies and flushes the firmware, and checks that the original controller
reconnects as a keyboard. Automatic entry has a 30-second timeout; manual
entry allows ten minutes. No independent flash read-back is performed.
Repeat with USB connected directly to the other half; each half needs the
initial manual installation.

The trigger accepts only the 32-byte, zero-padded `CORNE_BOOTLOADER_V1`
command over QMK Raw HID (usage page `0xFF60`, usage `0x61`). This command
is not a secret or authentication mechanism: any process allowed to write
to that HID interface can request BOOTSEL. Deployment sends it after sudo
authentication and does not broaden device permissions.

The Linux HID sender requires the GNU `coreutils` multicall binary. It
selects GNU `dd` explicitly because Toybox `dd` lacks the flags needed
to send a complete report in one write.

## Extra info

If you wish to point GitHub actions to a different repository, a different branch, or even a different keymap name, you can modify `.github/workflows/build_binaries.yml` to suit your needs.

To override the `build` job, you can change the following parameters to use a different QMK repository or branch:
```
    with:
      qmk_repo: qmk/qmk_firmware
      qmk_ref: master
```

If you wish to manually manage `qmk_firmware` using git within the userspace repository, you can add `qmk_firmware` as a submodule in the userspace directory instead. GitHub Actions will automatically use the submodule at the pinned revision if it exists, otherwise it will use the default latest revision of `qmk_firmware` from the main repository.

This can also be used to control which fork is used, though only upstream `qmk_firmware` will have support for external userspace until other manufacturers update their forks.

1. (First time only) `git submodule add https://github.com/qmk/qmk_firmware.git`
1. (To update) `git submodule update --init --recursive`
1. Commit your changes to your userspace repository
