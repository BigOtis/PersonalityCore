# Speech

Recognition uses [faster-whisper](https://github.com/SYSTRAN/faster-whisper),
defaulting to `base.en`. The CUDA backend needs CUDA 12/cuBLAS and cuDNN 9;
automatic device selection retries on CPU when CUDA cannot load. Configure
`speech.cuda_library_path` for a nonstandard installation. Actual device status
appears in Setup. Background warmup initializes models and speech kernels.

Voice synthesis uses [Kokoro ONNX](https://github.com/thewh1teagle/kokoro-onnx).
The standard installation uses ONNX Runtime on CPU. Character voices and speed
can be selected and previewed independently. British voices use British English
phonemization. Weights are cached in the data directory's `speech` folder.

The renderer captures mono PCM16 using an AudioWorklet, with browser echo
cancellation and noise suppression. Hold Mic or Space to record and release to
submit. Voice activity mode is manually armed with Mic and ends the recording
after detected speech followed by about 750 ms of silence. Each recording is
limited to 60 seconds. Device selectors are in Setup.

Playback uses scheduled Web Audio buffers. The visible Speaking state continues
until playback drains, even if model generation has completed. Mute stops current
playback and ignores subsequent audio. Escape, Interrupt, or starting a microphone
turn stops queued sound and cancels the current turn. Session changes also stop
capture and playback. There is no always-on or automatic spoken barge-in yet.

Completed dialogue segments are synthesized while generation continues. Model
initialization and native speech calls are serialized per speech engine to avoid
races; cancelling a turn discards its audio, though an already running native
synthesis call must finish internally. Native engines remain warm across turns.

The runtime's first-audio timing measures when an audio chunk can be sent. The UI
also measures when playback is scheduled. These are not physical speaker latency.
