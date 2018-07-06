luastatus is a universal status bar content generator. It allows you to
configure the way the data from event sources is processed and shown, with Lua.

Its main feature is that the content can be updated immediately as some event
occurs, be it a change of keyboard layout, active window title, volume or a song
in your favorite music player (provided that there is a plugin for it) — a thing
rather uncommon for tiling window managers.

Its motto is:

> No more heavy-forking, second-lagging shell-script status bar generators!

Show me examples!
===

<table>
<tr> <th>What you write <th>What you get
<tr>
 <td>
  <img src="https://user-images.githubusercontent.com/5462697/42401371-09058050-817e-11e8-8c49-b049832488d0.png" />
  <br/>
  <a href="https://github.com/shdown/luastatus/blob/master/contrib/widget-examples/i3/alsa.lua">example file</a>
 <td> <img src="https://user-images.githubusercontent.com/5462697/42401715-9d4a6324-817f-11e8-99b4-78e8a6813218.gif" />
</table>

Key concepts
===

![Explanation](https://user-images.githubusercontent.com/5462697/42400208-5b54f5f2-8179-11e8-9836-70d4e46d5c13.png)

Widgets
---
A widget is a Lua program with a table named `widget` defined (except for
`luastatus`, you can freely define and modify any other global variables).

The `widget` table **must** contain the following entries:

  * `plugin`: a string with the name of a *plugin* (see below) you want to
  receive data from. If it contains a slash, it is treated as a path to a shared
  library. If it does not, luastatus tries to load `<plugin>.so` from the
  directory configured at the build time (CMake `PLUGINS_DIR` variable, defaults
  to `${CMAKE_INSTALL_FULL_LIBDIR}/luastatus/plugins`).

  * `cb`: a function that converts the data received from a *plugin* to the
  format a *barlib* (see below) understands. It should take exactly one
  argument and return exactly one value.

The `widget` table **may** contain the following entries:

  * `opts`: a table with plugin’s options. If undefined, an empty table will be
  substituted.

  * `event`: a function or a string.
    - If is a function, it is called by the *barlib* when some event with the
     widget occurs (typically a click). It should take exactly one argument and
     not return anything;
    - if is a string, it is compiled as a function in a *separate state*, and
     when some event with the widget occurs, the compiled function is called in
     that state (not in the widget’s state, in which `cb` gets called). This may
     be useful for widgets that want not to receive data from plugin, but to
     generate it themselves (possibly using some external modules). Such a
     widget may want to specify `plugin = 'timer', opts = {period = 0}` and
     block in `cb` until it wants to update. The problem is that in this case,
     widget’s Lua mutex is almost always being acquired by `cb`, and there is
     very little chance for `event` to get called. A separate-state `event`
     function solves that.

Plugins
---
A plugin is a thing that knows when to call the `cb` function and what to pass
to.

Plugins are shared libraries. For how to write them, see
`DOCS/WRITING_BARLIB_OR_PLUGIN.md`.

Barlibs
---
A barlib (**bar** **lib**rary) is a thing that knows:

  * what to do with values the `cb` function returns;

  * when to call the `event` function and what to pass to;

  * how to indicate an error, should one happen.

Barlibs are shared libraries, too. For how to write them, see
`DOCS/WRITING_BARLIB_OR_PLUGIN.md`.

Barlibs are capable of taking options.

Plugins’ and barlib’s Lua functions
---
Plugins and barlibs can register Lua functions. They appear in
`luastatus.plugin` and `luastatus.barlib` submodules, correspondingly.

How it works
===
Each widget runs in its own thread and has its own Lua interpreter instance.

While Lua does support multiple interpreters running in separate threads, it
does not support multithreading within one interpreter, which means `cb()` and
`event()` of the same widget never overlap (a widget-local mutex is acquired
before calling any of these functions, and is released afterwards).

Also, due to luastatus’ architecture, no two `event()` functions, even from
different widgets, can overlap. (Note that `cb()` functions from different
widgets can overlap.)

Lua limitations
===
In luastatus, `os.setlocale` always fails as it is inherently not thread-safe.

Supported Lua versions
===
* 5.1
* LuaJIT, which is currently 5.1-compatible with “some language and library extensions from Lua 5.2”
* 5.2
* 5.3
* 5.4 (`work1` pre-release version)

Getting started
===
First, find your barlib’s subdirectory in the `barlibs/` directory. Then read
its `README.md` file for detailed instructions and documentation.

Similary, for plugins’ documentation, see `README.md` files in the
subdirectories of `plugins/`.

Finally, widget examples are in `contrib/widget-examples`.

Using luastatus binary
===
Note that some barlibs can provide their own wrappers for luastatus; that’s why
you should consult your barlib’s `README.md` first.

Pass a barlib with `-b`, then (optionally) its options with `-B`, then widget
files.

If `-b` argument contains a slash, it is treated as a path to a shared library.
If it does not, luastatus tries to load `<argument>.so` from the directory
configured at the build time (CMake `BARLIBS_DIR` variable, defaults to
`${CMAKE_INSTALL_FULL_LIBDIR}/luastatus/barlibs`).

Example:

    luastatus -b dwm -B display=:0 -B separator=' ' widget1.lua widget2.lua

Installation
===
`cmake . && make && sudo make install`

You can specify a Lua library to build with: `cmake -DWITH_LUA_LIBRARY=luajit .`

You can disable building certain barlibs and plugins, e.g. `cmake -DBUILD_PLUGIN_XTITLE=OFF .`

Reporting bugs, requesting features, suggesting patches
===
Feel free to open an issue or a pull request.

Migrating from older versions
===
See `DOCS/MIGRATION_GUIDE.md`.
