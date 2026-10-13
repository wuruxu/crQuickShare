#!/usr/bin/env python3
"""Validate resource coverage and GRIT fingerprints without building the app."""
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root.parent / 'tools/grit'))
from grit import grd_reader
from grit.node import message

expected = {'ja', 'ko', 'zh-CN', 'zh-HK', 'zh-TW', 'ar', 'fr', 'fr-CA',
            'ru', 'es', 'es-419', 'pt-BR', 'pt-PT'}
xml = ET.parse(root / 'crquickshare_strings.grd')
assert {n.attrib['lang'] for n in xml.findall('./translations/file')} == expected
resources = grd_reader.Parse(str(root / 'crquickshare_strings.grd'))
resources.SetOutputLanguage('en')
resources.RunGatherers()
messages = [n for n in resources if isinstance(n, message.MessageNode)]
ids = {n.GetCliques()[0].GetId(): n for n in messages}
assert len(ids) == len(messages)
for lang in sorted(expected):
    tree = ET.parse(root / 'translations' / f'crquickshare_strings_{lang}.xtb')
    translated = tree.findall('./translation')
    assert len(translated) == len(ids), lang
    assert {n.attrib['id'] for n in translated} == set(ids), lang
    for node in translated:
        source = ids[node.attrib['id']].GetCliques()[0].GetMessage()
        placeholders = {p.GetPresentation() for p in source.GetPlaceholders()}
        assert {p.attrib['name'] for p in node.findall('ph')} == placeholders, (lang, node.attrib['id'])
print(f'Validated {len(messages)} messages in English and {len(expected)} translations.')
