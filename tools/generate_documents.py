#!/usr/bin/env python3
"""Generate local FLTK HTML documents from Markdown and dependency notices."""

import argparse
import hashlib
import re
import shutil
import subprocess
from html import escape
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit


def anchor_name(identifier):
    """Keep named targets within FLTK's 31-byte target limit."""
    if re.fullmatch(r"[A-Za-z0-9_-]{1,31}", identifier):
        return identifier
    return "a-" + hashlib.sha256(identifier.encode("utf-8")).hexdigest()[:20]


class FLTKHTML(HTMLParser):
    """Adapt generated HTML to the elements supported by Fl_Help_View."""

    tags = set("a b body br center code dd dl dt em font h1 h2 h3 h4 h5 h6 "
               "head hr i kbd li ol p pre strong table th td tr title tt u ul var".split())
    wrappers = {"div", "section", "span", "thead", "tbody", "tfoot"}
    attributes = {
        "a": {"href", "name"}, 
        "table": {"border"},
        "td": {"colspan", "align"}, 
        "th": {"colspan", "align"},
        }

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.output = []
        self.targets = set()
        self.links = []

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        identifier = attrs.pop("id", None) or attrs.pop("name", None)
        if identifier:
            target = anchor_name(identifier)
            if target in self.targets:
                raise ValueError(f"Duplicate anchor: {identifier}")
            self.targets.add(target)
            self.output.append(f'<a name="{target}"></a>')
        if tag in self.wrappers:
            return
        if tag == "sup":
            self.output.append("[")
            return
        if tag not in self.tags:
            raise ValueError(f"Unsupported document element: {tag}")
        if tag == "a" and "href" in attrs:
            link = urlsplit(attrs["href"])
            if link.scheme and link.scheme not in {"http", "https", "mailto"}:
                raise ValueError(f"Unsupported link: {attrs['href']}")
            if not link.scheme:
                if link.netloc or (link.path and not re.fullmatch(r"[A-Za-z0-9_-]+\.html", link.path)):
                    raise ValueError(f"Expected a local HTML document: {attrs['href']}")
                fragment = anchor_name(link.fragment) if link.fragment else ""
                attrs["href"] = link.path + ("#" + fragment if fragment else "")
                self.links.append((link.path, fragment))
            if len(attrs["href"].encode("utf-8")) > 191:
                raise ValueError(f"Link exceeds FLTK's filename limit: {attrs['href']}")
        fields = "".join(f' {key}="{escape(value, quote=True)}"' for key, value in attrs.items()
                         if key in self.attributes.get(tag, set()) and value is not None)
        self.output.append(f"<{tag}{fields}>")

    def handle_endtag(self, tag):
        if tag == "sup":
            self.output.append("]")
        elif tag in self.tags and tag not in {"br", "hr"}:
            self.output.append(f"</{tag}>")

    def handle_data(self, data):
        self.output.append(escape(data.replace("↩︎", " [Back]").replace("↩", " [Back]"), quote=False))


def convert_markdown(source, pandoc):
    result = subprocess.run(
        [pandoc, "--from=markdown-smart", "--to=html4", "--wrap=none", str(source)],
        check=True, capture_output=True, encoding="utf-8")
    document = FLTKHTML()
    document.feed(result.stdout)
    document.close()
    return document


def notice_html(title, content):
    # Preserve the canonical text separately; flowing paragraphs fit a narrow viewer.
    paragraphs = re.split(r"\n\s*\n", content.replace("\r\n", "\n").replace("\f", "\n"))
    body = "\n".join("<p>" + escape(paragraph.strip(), quote=False) + "</p>"
                     for paragraph in paragraphs if paragraph.strip())
    return (f"<h1>{escape(title)}</h1><p><a href=\"licenses.html#components\">"
            "Back to component information</a></p>" + body)


def write_html(path, body):
    path.write_text("<html><body>\n" + body + "\n</body></html>\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pandoc", required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--notice", nargs=3, action="append", default=[],
                        metavar=("NAME", "TITLE", "FILE"))
    args = parser.parse_args()
    
    documents = {source.stem + ".html": convert_markdown(source, args.pandoc)
                 for source in sorted(args.source.glob("*.md"))}
    notices = {name + ".html" for name, _, _ in args.notice}
    for name, document in documents.items():
        for destination, target in document.links:
            destination = destination or name
            if destination not in documents and destination not in notices:
                raise ValueError(f"{name}: missing linked document {destination}")
            if target and (destination not in documents or target not in documents[destination].targets):
                raise ValueError(f"{name}: missing target {destination}#{target}")
    
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "licenses").mkdir(exist_ok=True)
    for name, document in documents.items():
        write_html(args.output / name, "".join(document.output))
    for source in args.source.glob("*.md"):
        shutil.copyfile(source, args.output / source.name)
    for name, title, filename in args.notice:
        source = Path(filename)
        shutil.copyfile(source, args.output / "licenses" / (name + ".txt"))
        write_html(args.output / (name + ".html"), notice_html(title, source.read_text(encoding="utf-8")))


if __name__ == "__main__":
    main()
