# SGI L2 Controller GUI for IRIX

Based on [SGI-L2-WUI](https://github.com/flexion-unity/SGI-L2-WUI) by flexion, chulofiasco and nater1217.

`sgil2gui` is a native IRIX 6.5 / Motif application for the SGI L2 system controller. 

* talks to your L2 controller over its client protocol (TCP port 8000)
* renders the server rack inventory in the GUI
* shows LCD text, power / attention / failure LEDs of every brick
* power up / down the whole rack or single bricks
* L2 console, rack config info, environment summary, fan status
* custom brick sort order to match your physical rack

Supported bricks: Origin 300, Origin 350, Onyx 350,
Tezro (rackmount), Altix 350 (yellow or grey bezel), Altix 3000 C-brick, Altix 3000 iX-brick, NL4R, router (Origin or Altix style).

<img src="res/img/sgil2gui-demo.png" width="420">

* Click a brick to open its dialog: LCD, details, Attention / Failure LED switches, power button.
* **Rack** menu / toolbar: refresh, power the whole rack up / down, env summary, fan status, router info.
* **Tools > L2 Console** (Ctrl+L): send L2 commands


## Build and install (IRIX)

```sh
make
make install    # as root: /usr/local/bin/sgil2gui + app-defaults
```

`make install` copies `app-defaults/SgiL2Gui` to `/usr/lib/X11/app-defaults`.


## Usage

```sh
sgil2gui                                 # launch GUI. uses ~/.sgil2guirc

sgil2gui -host 192.168.0.100             # override the L2 host
sgil2gui -demo                           # built-in L2 simulator, no hardware
```

On first start without a configured host the Settings dialog opens.
Save writes config to: `~/.sgil2guirc`.
