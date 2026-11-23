# Mak3P tests

Build and run the tests using the same CMake preset. Use an x64 Visual Studio Developer PowerShell on Windows or a GCC environment with `VCPKG_ROOT` set on Linux.

| What to test | Preset |
| --- | --- |
| Core models, paths, output naming, and output safety | `core-debug` |
| Windows application and audio conversion | `windows-release` |
| Linux application and audio conversion | `wsl-debug` or `wsl-release` |

```text
cmake --preset <preset>
cmake --build --preset <preset>
ctest --preset <preset> --output-on-failure
```

Audio tests require Python 3 and an FFmpeg/ffprobe pair on `PATH`. The fixture generators use FFmpeg encoders including `libmp3lame`, `libvorbis`, and `libopus`. The desktop build also requires Pandoc. Interactive window tests require a desktop session, with an X11 display on Linux. Use `ctest --preset <preset> -LE gui` in a headless environment.

To test the Windows Full engine with a separate FFmpeg for fixture generation, configure `windows-full` with `AUDIO_TEST_FFMPEG` and `AUDIO_TEST_FFPROBE` set to the Full binaries and `AUDIO_TEST_FIXTURE_FFMPEG` set to a build with the fixture encoders.

Test folders correspond to core, process, audio, settings, batch, conversion, and interface behavior. CTest generates synthetic media in the build tree and sets up the required fixtures when you select individual tests with `-R`.

The optional sample suite adds Monkey's Audio to the format matrix and checks hybrid WavPack correction-file handling. Prepare the upstream samples with `python3 tests/conversion/prepare_samples.py build/test-samples --download`, then reconfigure with `-DMAK3P_TEST_SAMPLE_DIR=build/test-samples`. The script verifies SHA-256 checksums. Use the directory containing the verified files if they are already available locally.
