# Using toolbox to manage a 32-bit Wine prefix

This guide shows how to set up a 32-bit Wine environment inside an
[toolbox](https://github.com/containers/toolbox) container instead of polluting
your host system. The example uses Ubuntu 25.04 because it ships a
well-maintained 32-bit Wine build, but you can adapt the steps to other
distributions that toolbox supports.

## Prerequisites

* `toolbox` installed on the host (Fedora and many other distributions include
  it out of the box).
* Sufficient disk space for the container image (~2–3 GB) and the Wine prefix.

## 1. Create a dedicated toolbox

Pick a name for the container (for example `visdriver-ubuntu-2504`) and create
it:

```console
$ toolbox --distro ubuntu --release 25.04 create visdriver-ubuntu-2504
Created container: visdriver-ubuntu-2504
Enter with: toolbox enter visdriver-ubuntu-2504
```

Then enter the container:

```console
$ toolbox enter visdriver-ubuntu-2504
```

Inside the toolbox you operate as an unprivileged user that has password-less
`sudo` access. All package management commands below are executed inside the
container.

## 2. Install Wine with 32-bit support

Ubuntu's Wine packages require enabling the i386 architecture first:

```console
$ sudo dpkg --add-architecture i386
$ sudo apt update
$ sudo apt install --no-install-recommends \
      wine-stable wine32 wine64 winetricks cabextract
```

The `--no-install-recommends` flag keeps the image smaller while still pulling
in everything required for 32-bit prefixes. Feel free to omit the flag if you
prefer the default recommendations.

## 3. Prepare a clean 32-bit prefix

Decide where the prefix should live. The example below places it at
`~/visdriver-wine32` inside the toolbox:

```console
$ export WINEPREFIX=$HOME/visdriver-wine32
$ export WINEARCH=win32
$ wineboot --init
```

Running `wineboot` the first time will populate the prefix and may take a few
minutes while Wine finishes installing its components.

## 4. Share your source tree with the toolbox

Toolbox automatically bind-mounts your home directory into the container, so if
you cloned the `visdriver` repository on the host under `~/projects/visdriver`
then the same path is available inside the toolbox. To build the project inside
the toolbox you can use the regular CMake workflow:

```console
$ cd ~/projects/visdriver
$ cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
$ make -C build -j"$(nproc)"
```

Any Wine commands you run inside the toolbox will now use the clean 32-bit
prefix because of the `WINEPREFIX` and `WINEARCH` exports above.

## 5. (Optional) Persist environment variables

To avoid re-exporting `WINEPREFIX` and `WINEARCH` every time, add them to
`~/.bashrc` inside the toolbox:

```console
$ cat <<'BASHRC' >> ~/.bashrc
export WINEPREFIX="$HOME/visdriver-wine32"
export WINEARCH=win32
BASHRC
```

Adjust `visdriver-wine32` if you stored the prefix under a different directory
name.

## 6. Exiting the toolbox

When you exit the toolbox (`Ctrl+D` or `exit`), the Wine prefix and build
artifacts stay on disk. Re-enter the toolbox whenever you need to run Wine or
rebuild visdriver. If you ever want to remove the toolbox again, run:

```console
$ toolbox rm visdriver-ubuntu-2504
```

Removing the toolbox does **not** delete the Wine prefix or any files stored in
your home directory. Remove the prefix manually if you no longer need it.
