# Jubeat tutorial skip button

This project adds a "HOLD TO SKIP" button to the tutorial screen in Jubeat that
plays before every guest credit. This lets new players still use the tutorial
(unlike the typical "nuke it" approach) while still letting regulars without a
card not be bored out of their minds waiting for the tutorial to play every
time.

![demonstration](example.gif)

This repo also contains a somewhat novel approach that lets this patch be
applied to **encrypted** game data, by modifying jubeat.dll to statically import
the hook functions. A typical minhook approach gets caught by an inbuilt
tamper-protection mechanism. jubeat.dll still needs to be re-encrypted, but if
you need this hook to run on encrypted data, you already know how to do that :)

It has been successfully tested on Festo, Ave, and Beyond the Ave data, and
confirmed to run on at least one encrypted network.

# Building

It's a typical meson project, though you'll need pillow installed in your python
env for the PNG conversion script to work. I use
[my LLVM fork with Windows XP support](https://github.com/mon/llvm-mingw-xp)
and run:
```shell
meson setup build --cross-file cross-clang-mingw-32-win.ini
meson compile -C build
```

Load `tutorial_skip.dll` as a typical hook DLL if you're using spice/bemanitools.

If you're doing the static patch for encrypted data, make your modified
jubeat.dll via:
```shell
uv run tools/patch_jubeat.py jubeat.dll jubeat_patched.dll
```
...or just `python` instead of `uv run` if you have `pefile` installed in your env.

# AI usage 🤖

Because if I can't be bothered to write something, you probably shouldn't read
it.

I reverse engineered the tutorial state machine and input capture myself and
wrote the initial hook impl.

I also had the initial thought of "hey what if we just screwed with IATs to not
need minhook on encrypted".

I cannot into graphics programming, so Claude did that (and it still scares me
how few iterations it took...) with me telling it to be less of an idiot.

It also wrote a bunch of glue code and fixed some dumb edge cases (you can tell
by the horrendous Claudeish comments around there).

It also wrote all the python code, though `patch_jubeat.py` took quite a few
iterations as I worked out what will actually run on encrypted data.
