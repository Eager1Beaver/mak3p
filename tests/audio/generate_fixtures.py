import pathlib
import subprocess
import sys

ffmpeg, destination = sys.argv[1:]
root = pathlib.Path(destination)
root.mkdir(parents=True, exist_ok=True)


def generate(name, arguments):
    subprocess.run([ffmpeg, "-nostdin", "-hide_banner", "-v", "error", "-y",
                    *arguments, str(root / name)], check=True)


def tone(name, codec, rate=44100, channels=1, options=()):
    generate(name, ["-f", "lavfi", "-i", f"sine=frequency=997:sample_rate={rate}",
                    "-t", "0.6", "-ac", str(channels), "-c:a", codec, *options])


tone("mono.wav", "pcm_s16le")
tone("stereo.flac", "flac", 48000, 2)
tone("high.wav", "pcm_s16le", 88200, 2)
tone("low.wav", "pcm_s16le", 16000)
tone("unusual.wav", "pcm_s16le", 40000)
tone("tone.wv", "wavpack")
tone("tone.aiff", "pcm_s16be")
tone("tone.m4a", "aac")
tone("tone.ogg", "libvorbis")
tone("tone.opus", "libopus", 48000)
tone("tone.wma", "wmav2")
tone("tone.ac3", "ac3", 48000, 6, ("-b:a", "384k"))
tone("cbr.mp3", "libmp3lame", 44100, 2, ("-b:a", "256k"))
tone("vbr.mp3", "libmp3lame", 44100, 2, ("-q:a", "2"))
generate("multiple.mka", ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
                         "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
                         "-t", "0.6", "-map", "0:a", "-map", "1:a", "-c:a", "flac"])
generate("video.mp4", ["-f", "lavfi", "-i", "color=c=black:s=16x16:r=10",
                      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
                      "-t", "0.6", "-map", "0:v", "-map", "1:a",
                      "-c:v", "mpeg4", "-threads:v", "1", "-c:a", "aac"])
for name, codec in [("cover.png", "png"), ("cover.jpg", "mjpeg")]:
    generate(name, ["-f", "lavfi", "-i", "color=c=blue:s=16x16",
                    "-frames:v", "1", "-c:v", codec, "-threads:v", "1", "-update", "1"])
(root / "invalid.mp3").write_bytes(b"not an audio file\n")

folder = "Media folder"
(root / folder).mkdir(exist_ok=True)
for name, bitrate in [("track-a.mp3", 160), ("track-b.mp3", 160), ("track-d.mp3", 224), ("track-e.mp3", 224)]:
    tone(f"{folder}/{name}", "libmp3lame", 44100, 2, ("-b:a", f"{bitrate}k", "-metadata", "artist=Test Artist"))
generate(f"{folder}/track-c.jpg", [
    "-f", "lavfi", "-i", "sine=frequency=997:sample_rate=44100", "-i", str(root / "cover.png"),
    "-map", "0:a", "-map", "1:v", "-t", "0.6", "-ac", "2", "-c:a", "libmp3lame", "-b:a", "160k",
    "-c:v", "copy", "-disposition:v", "attached_pic", "-metadata", "title=Test Track",
    "-metadata", "artist=Test Artist", "-f", "mp3",
])
generate(f"{folder}/video.mp4", [
    "-f", "lavfi", "-i", "color=c=black:s=16x16:r=10",
    "-f", "lavfi", "-i", "anoisesrc=color=pink:seed=871:sample_rate=44100",
    "-t", "1.1", "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-threads:v", "1",
    "-c:a", "aac", "-ac", "2", "-b:a", "256k",
])
for name, original in [("image.mp3", "cover.png"), ("artwork.png", "cover.png"), ("cover.jpg", "cover.jpg")]:
    (root / folder / name).write_bytes((root / original).read_bytes())
(root / folder / "notes.txt").write_text("Folder notes\n", encoding="utf-8")
print(f"Generated audio and artwork fixtures under {root}")
