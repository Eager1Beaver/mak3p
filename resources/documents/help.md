<!-- Mak3P help v1.0.0 -->

# Mak3P Help

Convert local audio files to MP3 in five steps. Your source files remain unchanged, and existing output files are never overwritten.

## Quick workflow

### 1. Add your audio

Click **Add files**, then **Select files…** for individual tracks or **Select folders…** for folders, including their subfolders. These commands are also available in the **Mak3P** menu. You can drag local files or folders onto the window.[^drop]

Mak3P checks each file and recognizes audio by its contents. A file with an unexpected extension appears after Mak3P finds usable audio in it. Files with familiar audio extensions appear with a skip reason if they cannot be used. Adding the same path again does not create a duplicate row.

**Include MP3 from folders** controls whether folder imports include MP3 files. Turn it on to show eligible MP3s from the current selection, or turn it off to hide them. Individually selected or dropped MP3 files are eligible regardless of this option.

### 2. Choose bitrate and channels

Fresh settings use **320 kbps** and **Stereo**. Mak3P remembers your choices.

- **Bitrate:** choose the target constant bitrate (CBR). By default, a lower-bitrate source receives the nearest standard MP3 bitrate at or below both the source bitrate and your target.[^bitrate]
- **Channels:** **Stereo** produces two channels, **Mono** produces one, and **Auto** keeps mono sources mono and otherwise produces stereo. Multichannel audio is mixed down as needed.
- **Force upsampling:** use your target bitrate even for lower-bitrate sources. A higher bitrate cannot restore missing audio detail.

The output sample rate is selected automatically between **44.1 kHz** and **48 kHz**. Existing supported metadata and compatible embedded artwork are preserved automatically.[^metadata]

### 3. Choose where to save

Under **Output location**, select **Same as source** to save each MP3 beside its input, or **Choose folder…** to save all outputs in one existing folder.

Name collisions receive numbered suffixes, such as `track (1).mp3`. Mak3P never replaces a source file or an existing output.

### 4. Set parallel conversions and optional logging

**Parallel conversions** is the number of files converted at once. **Auto** uses half of the available logical processors, rounded down, with at least one worker. Choose a smaller value if you want to leave more resources available for other applications.[^workers]

Enable **Write log file** if you want a detailed report. Mak3P writes a UTF-8 log beside the application, including paths, settings, warnings, failures, and results. Logs may contain personal information in filenames and paths; review them before sharing. If a log cannot be written, Mak3P reports the problem and continues conversion.

### 5. Start and review the results

Click **Start conversion** when at least one file is **Ready**. Each active row shows progress and an estimated remaining time when available. Estimates can change, and files with unknown duration may have no percentage or estimate.

Hover over a row or status for its full path, audio properties, warnings, or the reason a file failed or was skipped.

- **Completed:** the MP3 was successfully saved.
- **Skipped:** no conversion was performed. For example, a verified CBR MP3 already matches the selected target and needs no channel change, or the input is unsupported or unreadable.[^matching]
- **Failed:** conversion or saving the output did not succeed. Check the explanation and the optional log, correct the problem, then remove and add the file again to retry.
- **Cancelled:** the file was not completed before cancellation. You can click **Start conversion** to retry unfinished files.

A skipped or failed file does not stop the remaining files. Adding new files keeps earlier completed results. Changing bitrate, channels, force upsampling, or output location rechecks the selection and can make a new run available.

## Cancel, remove, and clear

Click **Cancel conversion** to stop unfinished work. The button shows **Cancelling…** while active processes stop. Mak3P removes its unfinished temporary outputs and keeps completed MP3s. All rows stay visible.

Adding files, removing rows, and changing settings are unavailable while work is active. When idle, use a row's remove button or **Clear selection** to change the queue. Neither action deletes source files or completed MP3s. The queue is not restored after the application closes.

## Audio tools and supported inputs

Mak3P requires **FFmpeg** and **ffprobe**, with the **libmp3lame** encoder available in FFmpeg. It looks beside the application, in its `bin` folder, in a selected engine folder, and on the system's PATH. If the tools are unavailable, use **Choose FFmpeg folder…**, select their folder, and retry. Mak3P does not download tools automatically.

Supported inputs include FLAC, APE, WavPack, WAV, AIFF, AAC/ALAC in MP4/M4A, DSF, AC-3, Ogg/Vorbis, Opus, WMA, and MP3 when the selected FFmpeg build can decode them. For files containing multiple audio streams, Mak3P uses the first usable audio stream; non-audio streams are omitted.

**Hybrid WavPack:** conversion uses the lossy `.wv` stream because FFmpeg does not apply `.wvc` correction files. Mak3P reports this limitation; correction files remain unchanged and are not separate audio inputs.

## Settings and window size

Mak3P remembers your conversion settings and normal window size in `config.ini`. It prefers the folder beside the application. If that folder is not writable, it uses a per-user location:

- Windows: `%LOCALAPPDATA%/Mak3P/config.ini`.
- Linux: `$XDG_CONFIG_HOME/Mak3P/config.ini`, or `$HOME/.config/Mak3P/config.ini` when `XDG_CONFIG_HOME` is not set.

Use **Mak3P → Default window size** to restore the main window's default size.

## Notes

[^drop]: Drag-and-drop depends on the desktop environment. Windows Explorer cannot currently transfer files directly into a Linux application running through WSLg. In that environment, use **Select files…** or **Select folders…**, or drag from a supported Linux file manager.

[^bitrate]: With a target of 256 kbps, source bitrates of 302, 235, and 173 kbps receive 256, 224, and 160 kbps respectively. Automatic matching can select rates below the target menu's 128 kbps minimum. Sources below 32 kbps are skipped unless **Force upsampling** is enabled. If the source bitrate is unknown, Mak3P uses the target and reports a warning. Bitrates from different codecs are not directly comparable measures of quality.

[^metadata]: Only fields and artwork that the output format and metadata tools can represent are copied. Compatible JPEG/PNG artwork is supported. Unsupported items remain in the source and are reported in row warnings and the optional log.

[^workers]: The manual choices are limited to the detected logical processor count and the application's worker limit. Auto also uses no more workers than there are files to convert. If processor detection fails, Auto uses up to two workers.

[^matching]: A verified CBR MP3 below the selected target remains eligible at its automatically adjusted rate. Force upsampling uses the selected target. Mak3P skips a verified CBR match only if its rate equals the selected target and its channels already match. Variable-bitrate MP3s remain eligible even when their average rate equals the target.
