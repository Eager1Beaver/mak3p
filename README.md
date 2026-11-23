# Mak3P

Mak3P is a free, portable desktop app for converting local audio files and folders to MP3. It works offline and supports batch conversion, with progress and results shown for each file.

## What it does

- Converts formats such as FLAC, APE, WavPack, WAV, AIFF, M4A, Ogg/Vorbis, Opus, WMA, and MP3 when supported by the selected audio tools.
- Offers a target bitrate and Stereo, Mono, or Auto channels.
- Preserves supported metadata and compatible embedded artwork automatically.
- Converts multiple files at once, with optional detailed logs.
- Leaves source files unchanged and uses numbered names to avoid overwriting existing outputs.

## Get started

1. **Open Mak3P.** If prompted, choose the folder containing FFmpeg and ffprobe. FFmpeg must support the `libmp3lame` MP3 encoder. Mak3P does not download tools automatically.
2. **Add your audio.** Click **Add files → Select files…** or **Select folders…**, or drag local files or folders onto the window.
3. **Choose your settings.** Fresh settings use **320 kbps / Stereo**. Choose **Same as source** to save each MP3 beside its input, or **Choose folder…** to save the batch in one existing folder. **Parallel conversions** controls how many files run at once; **Auto** selects a value for your computer.
4. **Click Start conversion.** Review each file's progress and result. Hover over a row or status to read warnings and explanations for skipped or failed files.
5. **Cancel if needed.** **Cancel conversion** stops unfinished work and keeps completed MP3s. Removing rows or clearing the selection does not delete files.

## Useful details

By default, lower-bitrate inputs receive a standard MP3 bitrate no higher than both their source bitrate and your target. **Force upsampling** uses your target instead; increasing bitrate cannot restore missing audio detail. CBR MP3s that match the selected target are skipped when no channel change is needed.

**Include MP3 from folders** affects folder imports only. Individually selected MP3s remain eligible. Metadata and artwork preservation depends on what MP3 and the metadata tools can represent; any limitations appear in file warnings.

Mak3P remembers your settings and normal window size. Enable **Write log file** to save a report beside the application. Logs can contain filenames and paths, so review them before sharing.

## Help and license

Open **Mak3P → Help** for the full guide. **About → License** displays the end-user agreement and third-party notices; **About → Changelog** shows release changes. Keep the `documents` folder beside the application so these documents remain available.

You can also read the [full guide](resources/documents/help.md) and [license agreement](resources/documents/licenses.md) here.
