# Contributing

Issues and pull requests are welcome.

For bugs, include your Hyprland version (`hyprctl version`), your monitor
setup, and what you did right before. A crash leaves a report you can see with
`coredumpctl list Hyprland`.

The plugin reaches deep into Hyprland internals, so please test a change in a
nested session before sending it:

```sh
make                                   # builds spatialoverview.so
make test-tools                        # the virtual mouse some tests use
python3 tests/navigator-nested.py --plugin spatialoverview.so
python3 tests/popup-nested.py spatialoverview.so
make test-dnd-route                    # drag-and-drop routing, no compositor needed
python3 tests/dnd-nested.py spatialoverview.so
```

Each test opens Hyprland in a window of its own and closes it again; nothing
touches your running session or your config.
