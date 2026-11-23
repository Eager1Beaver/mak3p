<!-- Mak3P License content v1.0.0 -->

# End-user license agreement for Mak3P

**Revision date: November 23, 2026**

**Copyright © 2026 Ilia Golub.**

This agreement describes your rights and responsibilities when using Mak3P (the **Software**). The author and copyright holder is **Ilia Golub** (the **Author**). The Software includes Mak3P's application code and its documentation. Third-party components have their own licenses, as explained
in [Third-party components](#components).

By using the Software, you agree to these terms to the extent permitted by applicable law. If you do not agree, do not use the Software. Nothing in this agreement limits rights that applicable law does not allow to be excluded.

## 1. Free use

The Author grants you a nonexclusive right to use Mak3P free of charge for personal, educational, organizational, and commercial purposes. Mak3P is provided as a portable application and does not require an installer.

You may make copies for your own use and backup. You are responsible for ensuring that you have permission to access, convert, and use your audio files and their metadata or artwork. This agreement gives you no rights in another person's audio or other content.

## 2. Redistribution and authorship

You may redistribute Mak3P **free of charge**, provided that the Author's authorship is fully preserved. Keep the Author's credit, copyright notices, this agreement, and the applicable third-party notices with every copy. Do not claim that you created Mak3P, remove or conceal its authorship, or present a changed copy as an official release from the Author.

You may not sell or rent Mak3P, charge for access to or use of Mak3P, or require payment or a subscription to obtain or use it. This restriction does not prevent you from charging for your own work or content created using Mak3P; the charge must not be for access to or use of Mak3P itself.

These restrictions apply to Mak3P's own code and documentation. They do not restrict rights granted by third-party licenses, including rights to copy, modify, redistribute, or reverse engineer third-party components, or other rights that applicable law protects.

## 3. Privacy and local data

Mak3P processes audio on your computer. Conversion does not require an internet connection. Mak3P does not include telemetry, analytics, advertising, user accounts, or automatic update checks, and does not upload your audio, metadata, or usage information to the Author or to a service.

Mak3P does read and write local data needed for its features:

- It reads the files and folders you select, along with audio properties, metadata, and embedded artwork needed for conversion.
- It writes converted MP3s and temporary files used to prepare those outputs. Unfinished temporary outputs are removed during normal cancellation and error handling; abnormal termination may leave temporary files behind.
- It saves conversion preferences, selected folder paths, and normal window dimensions in `config.ini`, beside the application or in the per-user configuration folder described in [Help](help.html).
- If you enable **Write log file**, it saves a report beside the application. Logs can contain input and output paths, settings, tool versions, diagnostics, and conversion results. Review logs before sharing them.
- It reads the available logical processor count to choose and limit parallel conversions. It does not use that count as a tracking identifier.

Mak3P currently displays its interface in English (United States). It does not select a localization based on your system language.

Opening an external project, source, or contact link may launch your browser or email application. Any resulting connection is handled by that application and the destination service. Access to files on network drives is handled through your operating system. FFmpeg and ffprobe run as separate tools and remain subject to their own software and license terms.

## 4. Your files and responsibilities

Mak3P is designed to leave source files unchanged and avoid overwriting existing outputs. Keep your own backups and verify converted files before deleting originals. The Software may contain defects, and no software can guarantee against every failure of the operating system, storage device, or other software.

You are responsible for choosing appropriate conversion settings and using the Software in accordance with applicable law.

## 5. Warranty disclaimer

To the fullest extent permitted by applicable law, the Software is provided **"as is" and "as available," without warranties of any kind**, whether express or implied, including warranties of merchantability, fitness for a particular purpose, and noninfringement.

The Author does not guarantee uninterrupted or error-free operation, compatibility with a particular computer or audio file, preservation of every metadata field, or availability of updates or support.

## 6. Limitation of liability

To the fullest extent permitted by applicable law, the Author is not liable for loss or damage arising from the use of, or inability to use, the Software, including loss of data, loss of profits, business interruption, or indirect, incidental, special, or consequential damages.

These exclusions do not apply where applicable law prohibits them. Your nonwaivable statutory rights remain unaffected.

## 7. Ownership and third-party rights

Mak3P is licensed for use under these terms; its copyright is not transferred to you. Rights in Mak3P's own code and documentation remain with the Author. Rights in third-party components, names, and marks remain with their respective owners.

Third-party license terms take precedence for the components they cover. This agreement does not replace those terms, claim ownership of those components, or restrict rights they grant. In particular, it does not prohibit reverse engineering needed to exercise a third-party license's rights or modifications required for relinking a covered library.

## 8. Support and contact

Support is provided at the Author's discretion and is not guaranteed. When reporting a problem, include the Mak3P version, operating system, a description of the problem, and relevant diagnostics. Remove private paths and other personal information before sharing logs.

- **Author:** Ilia Golub
- **Email:** [TO BE UPDATED]
- **GitHub:** [Mak3P](https://github.com/Eager1Beaver/mak3p)

## 9. Third-party components {#components}

The notices below identify the principal application components and the external audio tools. Open the links in this section to read the library licenses. The Windows Full package also includes `LICENSES.txt` beside Mak3P.exe with the bundle's exact audio-tool notices and corresponding source details.

### FLTK 1.4.5

Mak3P is based in part on the work of the [FLTK project](https://www.fltk.org/). FLTK is distributed under the GNU Library General Public License, version 2, with FLTK-specific exceptions, including an exception for static linking.

- [Full FLTK license and exceptions](fltk-license.html).
- [FLTK UTF-8 code notices](fltk-utf8-license.html).
- [FLTK source for this version](https://github.com/fltk/fltk/tree/release-1.4.5).

### TagLib 2.3.1

[TagLib](https://taglib.org/) provides audio metadata support. The Windows Full package distributes TagLib under the GNU Lesser General Public License, version 2.1. The Mozilla Public License 1.1 applies as an alternative to files that offer it in their source notices.

- [Full GNU LGPL 2.1 text](taglib-lgpl.html).
- [Full Mozilla Public License 1.1 text](taglib-mpl.html).
- [TagLib source for this version](https://github.com/taglib/taglib/tree/v2.3.1).

### JSON for Modern C++

[nlohmann/json](https://github.com/nlohmann/json) is used under the MIT License. Copyright © 2013–2025 Niels Lohmann.

- [Full MIT license and copyright notice](json-license.html).

### UTF8-CPP and zlib

TagLib uses [UTF8-CPP](https://github.com/nemtrif/utfcpp), available under the Boost Software License 1.0, and [zlib](https://zlib.net/), available under the zlib license. Their notices and disclaimers also apply to their code.

- [Full UTF8-CPP license](utfcpp-license.html).
- [Full zlib license](zlib-license.html).

### FFmpeg and ffprobe

Mak3P invokes [FFmpeg and ffprobe](https://ffmpeg.org/) as separate executables; it does not own those tools. Their licenses depend on the exact build and enabled components. FFmpeg is generally available under the GNU LGPL 2.1 or later; builds using GPL components are covered by the GNU GPL, and builds with nonfree components can have redistribution restrictions.

The Windows Full package includes FFmpeg and ffprobe under GNU LGPL 2.1 or later. See the package's `LICENSES.txt` for the bundled build and corresponding source. For other builds, consult the licenses and source information for the exact tools you use. See the [FFmpeg licensing information](https://ffmpeg.org/legal.html).

### LAME / libmp3lame

Mak3P uses FFmpeg's **libmp3lame** encoder to create MP3 audio. [LAME](https://lame.sourceforge.io/) is a separate project distributed under the GNU LGPL. Consult the LAME license and notices supplied with your FFmpeg build, including any terms for additional components enabled in that build.

### Platform libraries

The application also uses operating-system interfaces and, on Linux, system libraries for its display, fonts, and related services. Those components remain subject to their respective licenses and are supplied by the operating system or their distributors.
