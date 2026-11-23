"""Check packaged document navigation, notices, and generator failures."""

from html.parser import HTMLParser
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from urllib.parse import urlsplit


SOURCE, OUTPUT, GENERATOR, PANDOC = sys.argv[1:]
sys.argv[1:] = []
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("document_generator", GENERATOR)
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class Document(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.targets, self.links, self.tags, self.text = [], [], set(), []
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        self.tags.add(tag)
        attrs = dict(attrs)
        if "name" in attrs:
            self.targets.append(attrs["name"])
        if "href" in attrs:
            self.links.append(urlsplit(attrs["href"]))
        if "style" in attrs or "id" in attrs:
            raise AssertionError("Unadapted HTML attributes")

    def handle_data(self, data):
        self.text.append(data)


class Documents(unittest.TestCase):
    def test_packaged_navigation(self):
        documents = {path.name: Document(path.read_text(encoding="utf-8"))
                     for path in Path(OUTPUT).glob("*.html")}
        self.assertEqual(len(documents), 11)
        for name, document in documents.items():
            self.assertLessEqual(document.tags, generator.FLTKHTML.tags | {"html"})
            self.assertEqual(len(document.targets), len(set(document.targets)))
            self.assertTrue(all(len(target.encode()) <= 31 for target in document.targets))
            for link in document.links:
                if not link.scheme:
                    destination = link.path or name
                    self.assertIn(destination, documents)
                    if link.fragment:
                        self.assertIn(link.fragment, documents[destination].targets)
        help_doc = documents["help.html"]
        for number in range(1, 6):
            self.assertIn("fn" + str(number), help_doc.targets)
            self.assertIn("fnref" + str(number), help_doc.targets)
        self.assertNotIn("[^", "".join(help_doc.text))
        about = documents["about.html"]
        self.assertIn("strong", about.tags)
        self.assertIn("free audio-to-MP3 converter", "".join(about.text))
        self.assertTrue(any(link.scheme == "https" and link.netloc == "github.com" for link in about.links))

    def test_sources_and_notices(self):
        for source in Path(SOURCE).glob("*.md"):
            self.assertEqual(source.read_bytes(), (Path(OUTPUT) / source.name).read_bytes())
        notices = list((Path(OUTPUT) / "licenses").glob("*.txt"))
        self.assertEqual(len(notices), 7)
        for notice in notices:
            document = Document((Path(OUTPUT) / (notice.stem + ".html")).read_text(encoding="utf-8"))
            canonical = " ".join(notice.read_text(encoding="utf-8").split())
            rendered = " ".join(" ".join(document.text).split())
            self.assertIn(canonical, rendered)

    def test_long_anchor_navigation(self):
        identifier = "a-heading-" + "long-" * 20
        doc = generator.FLTKHTML()
        doc.feed(f'<h2 id="{identifier}">Heading</h2><a href="#{identifier}">Jump</a>')
        result = Document("".join(doc.output))
        self.assertEqual(result.links[0].fragment, result.targets[0])
        self.assertLessEqual(len(result.targets[0]), 31)

    def test_missing_link_fails_generation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "help.md").write_text("# Help\n\n[Missing](#no-such-target)\n", encoding="utf-8")
            result = subprocess.run([sys.executable, GENERATOR, "--source", str(root),
                                     "--output", str(root / "output"), "--pandoc", PANDOC],
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("missing target", result.stderr)

    def test_unsupported_markup_fails(self):
        for html in ('<img src="image.png">', '<a href="javascript:alert(1)">Bad</a>',
                     '<a href="../missing.html">Outside</a>'):
            with self.assertRaises(ValueError):
                generator.FLTKHTML().feed(html)


unittest.main()
