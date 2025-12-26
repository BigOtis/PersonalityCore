# Bundled Piper voices

This folder is where the Fab distribution of **LocalTalker** will ship one or more default Piper voice models.

The runtime will auto-default to:

- `Resources/Voices/en_US-lessac-small.onnx`

If you change the bundled voice name(s), update the defaults in:

- `Source/LocalTalker/Private/LocalCharacterComponent.cpp` (`ResolvePaths()`)


