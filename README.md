# IR-Switch
A homebrew app for the switch that allows you to view the Nintendo Switch's IR camera on the right joy-con that is steamed to the Nintendo switch's screen.

# Features:

• Change the resolution

• Hide the UI

• Switch between greyscale and thermal colour*

• Turn on and off the IR LED.

• Detach the controllers to use like a mini camera

*The thermal colour mode shows the amount of InfaRed light that is recieved by the InfraRed sensor. It is NOT a thermal camera because the sensor only detects shorter infrared wavelengths of around 850-940nm that are emitted by a TV remote for example. It cannot detect heat's wavelength.

# Controls:

• Press left stick to hide/show UI

• Up/Down on D-PAD to switch between thermal/greyscale

• Left/Right on D-PAD to change resolution

• ZL/ZR to change the exposure preset (FAST 100us / NORMAL 300us / BRIGHT 700us)

• Y to toggle software auto-gain

• B to run an automatic resolution sweep (3 s per step, all five formats)

• X to toggle the on-screen log, which is also written to sdmc:/switch/IR-Switch.log

• - to turn off IR LED

Note:

Higher resolution for the IR camera slows down the camera speed while lower resolution speeds up the camera speed.

The IR sensor itself tops out at 320x240 - that is a hardware limit, not an app limit. The app now renders at the
Switch's native 1280x720 with integer (pixel-perfect) scaling instead of rendering 640x480 and letting the compositor
blur it, adds software auto-gain so dark frames stay readable, and shows a live FPS counter so the frame rate can be
measured rather than guessed.

Measured on hardware: at 160x120 the sensor delivers roughly one image per second, i.e. about 17 KB/s of image
data, and the render loop is not the bottleneck (it runs at ~60 Hz while frames arrive once a second). Because that
budget is a byte rate, the frames per second roughly scale with 1 / (width * height): a smaller format is faster,
a larger one proportionally slower. Press B and read the `sweep fmt ... frames ... fps ...` lines in the log to get
the exact curve on your unit.

# Usage:

Download the zip from https://github.com/tech-will/IR-Switch/releases/tag/Final-Release and extract to the root of your switch SD card. 
You can also get it on the Homebrew App Store.

[<img width="1080" height="300" alt="HB App Store shortcut link" src="https://github.com/user-attachments/assets/b665a650-9f9b-4db0-a980-c326a1da74b5" style="width:35%; height:auto;" />](https://hb-app.store/switch/IRSwitch)

# Photos:
My Imac in thermal colour mode.
![2025112312343900-06618FAD45328E0C2BC8741B95018E59](https://github.com/user-attachments/assets/bec20280-eaea-422f-a639-1c0f6ebfeacd)
A rubics cube in greyscale colour mode.
![2025112312402500-06618FAD45328E0C2BC8741B95018E59](https://github.com/user-attachments/assets/0b3fa259-4450-4745-b0db-2aef570c3de4)

ps no idea when I managed to get 903 downloads on the hb app store

#Future plans
Might add UI after I finish my other homebrew app.
