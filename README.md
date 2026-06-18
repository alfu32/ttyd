![backend](https://github.com/tsl0922/ttyd/workflows/backend/badge.svg)
![frontend](https://github.com/tsl0922/ttyd/workflows/frontend/badge.svg)
[![GitHub Releases](https://img.shields.io/github/downloads/tsl0922/ttyd/total)](https://github.com/tsl0922/ttyd/releases)
[![Docker Pulls](https://img.shields.io/docker/pulls/tsl0922/ttyd)](https://hub.docker.com/r/tsl0922/ttyd)
[![Packaging status](https://repology.org/badge/tiny-repos/ttyd.svg)](https://repology.org/project/ttyd/versions)
![GitHub](https://img.shields.io/github/license/tsl0922/ttyd)

# ttyd - Share your terminal over the web

ttyd is a simple command-line tool for sharing terminal over the web.

![screenshot](https://github.com/tsl0922/ttyd/raw/main/screenshot.gif)

# Features

- Built on top of [libuv](https://libuv.org) and [WebGL2](https://developer.mozilla.org/en-US/docs/Web/API/WebGL_API) for speed
- Fully-featured terminal with [CJK](https://en.wikipedia.org/wiki/CJK_characters) and IME support
- [ZMODEM](https://en.wikipedia.org/wiki/ZMODEM) ([lrzsz](https://ohse.de/uwe/software/lrzsz.html)) / [trzsz](https://trzsz.github.io) file transfer support
- [Sixel](https://en.wikipedia.org/wiki/Sixel) image output support ([img2sixel](https://saitoha.github.io/libsixel) / [lsix](https://github.com/hackerb9/lsix))
- SSL support based on [OpenSSL](https://www.openssl.org) / [Mbed TLS](https://github.com/Mbed-TLS/mbedtls)
- Run any custom command with options
- Basic authentication support and many other custom options
- Cross platform: macOS, Linux, FreeBSD/OpenBSD, [OpenWrt](https://openwrt.org), Windows

# Installation

## Install on macOS

- Install with [Homebrew](http://brew.sh): `brew install ttyd`
- Install with [MacPorts](https://www.macports.org): `sudo port install ttyd`

## Install on Linux

- Install on Debian/Ubuntu: `sudo apt install ttyd`
- Install the snap: `sudo snap install ttyd --classic`
- Install on OpenWrt: `opkg install ttyd`
- Install on Gentoo: clone the [repo](https://bitbucket.org/mgpagano/ttyd/src/master) and follow the directions [here](https://wiki.gentoo.org/wiki/Custom_repository#Creating_a_local_repository).
- Install with [Homebrew](https://docs.brew.sh/Homebrew-on-Linux) : `brew install ttyd`
- Precompiled static binaries: download from the [releases](https://github.com/tsl0922/ttyd/releases) page

## Install on Windows

- Binary version (recommended): download from the [releases](https://github.com/tsl0922/ttyd/releases) page
- Install with [WinGet](https://github.com/microsoft/winget-cli): `winget install tsl0922.ttyd`
- Install with [Scoop](https://scoop.sh/#/apps?q=ttyd&s=2&d=1&o=true): `scoop install ttyd`
- [Compile on Windows](https://github.com/tsl0922/ttyd/wiki/Compile-on-Windows)

# Build as DLL/SO/DYLIB

ttyd can also be built as a shared library for FFI callers, such as a Java application using JNI or JNA. The shared build exports the regular CLI entry point:

```c
int main(int argc, char **argv);
```

Install the native development dependencies before configuring. At minimum, CMake must be able to find headers and libraries for `libuv`, `libwebsockets`, `json-c`, and `zlib`. `libwebsockets` must be built with libuv support enabled (`LWS_WITH_LIBUV=ON`). For example, on Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libuv1-dev libwebsockets-dev libjson-c-dev zlib1g-dev
```

On Windows with vcpkg, install the same dependency set before running CMake:

```powershell
vcpkg install libwebsockets libuv json-c zlib openssl getopt-win32 --triplet x64-windows-static
```

On macOS with Homebrew:

```bash
brew install cmake libuv libwebsockets json-c zlib openssl@3
```

Then configure and build the shared target with CMake:

```bash
cmake -S . -B build-shared -DTTYD_BUILD_SHARED=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-shared --target ttyd_shared --config Release
```

On Windows this produces `ttyd.dll` under the build configuration directory, for example `build-shared/Release/ttyd.dll` with Visual Studio. On Linux this produces `libttyd.so`, usually at `build-shared/libttyd.so`. On macOS this produces `libttyd.dylib`, usually at `build-shared/libttyd.dylib`.

When calling from Java FFI, pass arguments exactly as the CLI would receive them, including `argv[0]`. For example: `["ttyd", "-p", "7681", "bash"]`. The call runs the ttyd server loop and blocks until the server exits, so invoke it from a dedicated Java thread if the application must keep running other work.

# Usage

## Command-line Options

```
USAGE:
    ttyd [options] <command> [<arguments...>]

OPTIONS:
    -p, --port              Port to listen (default: 7681, use `0` for random port)
    -i, --interface         Network interface to bind (eg: eth0), or UNIX domain socket path (eg: /var/run/ttyd.sock)
    -U, --socket-owner      User owner of the UNIX domain socket file, when enabled (eg: user:group)
    -c, --credential        Credential for basic authentication (format: username:password)
    -H, --auth-header       HTTP Header name for auth proxy, this will configure ttyd to let a HTTP reverse proxy handle authentication
    -u, --uid               User id to run with
    -g, --gid               Group id to run with
    -s, --signal            Signal to send to the command when exit it (default: 1, SIGHUP)
    -w, --cwd               Working directory to be set for the child program
    -a, --url-arg           Allow client to send command line arguments in URL (eg: http://localhost:7681?arg=foo&arg=bar)
    -W, --writable          Allow clients to write to the TTY (readonly by default)
    -t, --client-option     Send option to client (format: key=value), repeat to add more options
    -T, --terminal-type     Terminal type to report, default: xterm-256color
    -O, --check-origin      Do not allow websocket connection from different origin
    -m, --max-clients       Maximum clients to support (default: 0, no limit)
    -o, --once              Accept only one client and exit on disconnection
    -q, --exit-no-conn      Exit on all clients disconnection
    -B, --browser           Open terminal with the default system browser
    -I, --index             Custom index.html path
    -b, --base-path         Expected base path for requests coming from a reverse proxy (eg: /mounted/here, max length: 128)
    -P, --ping-interval     Websocket ping interval(sec) (default: 5)
    -6, --ipv6              Enable IPv6 support
    -S, --ssl               Enable SSL
    -C, --ssl-cert          SSL certificate file path
    -K, --ssl-key           SSL key file path
    -A, --ssl-ca            SSL CA file path for client certificate verification
    -d, --debug             Set log level (default: 7)
    -v, --version           Print the version and exit
    -h, --help              Print this text and exit
```

Read the example usage on the [wiki](https://github.com/tsl0922/ttyd/wiki/Example-Usage).

## Browser Support

Modern browsers, See [Browser Support](https://github.com/xtermjs/xterm.js#browser-support).

## Alternatives

* [Wetty](https://github.com/krishnasrinivas/wetty): [Node](https://nodejs.org) based web terminal (SSH/login)
* [GoTTY](https://github.com/yudai/gotty): [Go](https://golang.org) based web terminal
