# Direct RDP & VNC

A free, open-source remote desktop client for Ubuntu. It connects to Windows PCs
over **RDP** and to Macs and Linux desktops over **VNC**, in one simple GNOME app.

- Type an address and connect: `pc.local`, `192.168.1.20:3390`, `vnc://mac.local`, `user@host`
- Saved computers, with passwords kept in the GNOME keyring
- RDP: dynamic resolution (the remote desktop follows the window size), HiDPI scaling,
  certificate checks, Network Level Authentication
- VNC: plain VNC passwords and Mac Screen Sharing sign-in with your Mac account
- Macs: smooth movement with the Mac sending every change as it happens, full
  quality or Apple's own Adaptive picture for slow connections (a fraction of
  the data), and copy and paste in both directions
- Full screen with keyboard shortcuts sent to the remote computer
- Send Ctrl+Alt+Del, Windows key, Alt+Tab and more from the keys menu
- Wake on LAN for saved computers
- Opens `rdp://` and `vnc://` links

## Install

### From the PPA (Ubuntu 24.04 and later)

```sh
sudo add-apt-repository ppa:lidanh/direct-rdp
sudo apt install direct-rdp
```

### From a release .deb

Download `direct-rdp_*.deb` from the
[releases page](https://github.com/Lidanha/direct-rdp/releases), then:

```sh
sudo apt install ./direct-rdp_*.deb
```

## Build from source

```sh
sudo apt install build-essential meson pkgconf libgtk-4-dev libadwaita-1-dev \
  freerdp3-dev libvncserver-dev libsecret-1-dev
meson setup build
meson compile -C build
./build/src/direct-rdp
```

To build the Debian package:

```sh
sudo apt install debhelper devscripts
dpkg-buildpackage -us -uc -b
```

## Built with

[GTK 4](https://gtk.org), [libadwaita](https://gnome.pages.gitlab.gnome.org/libadwaita/),
[FreeRDP 3](https://www.freerdp.com), [LibVNCServer](https://libvnc.github.io) and
[libsecret](https://gnome.pages.gitlab.gnome.org/libsecret/).

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
