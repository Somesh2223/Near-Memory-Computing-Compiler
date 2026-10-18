#!/usr/bin/env python3
"""check_manuscript.py -- counts the template's required items in main.tex and checks the citations."""
import re, os

t = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "main.tex"), encoding="utf-8").read()
body = t[t.index(r"\begin{abstract}"):t.index(r"\begin{thebibliography}")]
body = re.sub(r"\\begin\{tikzpicture\}.*?\\end\{tikzpicture\}", " ", body, flags=re.S)
body = re.sub(r"\\begin\{tabular\}.*?\\end\{tabular\}", " ", body, flags=re.S)
body = re.sub(r"%.*", " ", body)
body = re.sub(r"\\[a-zA-Z]+\*?(\[[^\]]*\])?", " ", body)
body = re.sub(r"[{}$\\]", " ", body)
words = [w for w in body.split() if re.search(r"[A-Za-z]", w)]
print("prose words:", len(words))

abstract = t[t.index(r"\begin{abstract}") + 16:t.index(r"\end{abstract}")]
print("abstract words:", len(abstract.split()))
print("figures:", len(re.findall(r"\\begin\{figure\*?\}", t)),
      "tables:", len(re.findall(r"\\begin\{table\*?\}", t)),
      "equations:", t.count(r"\begin{equation}"),
      "algorithms:", t.count(r"\begin{algorithm}"),
      "references:", t.count(r"\bibitem"))
kw = t[t.index(r"\begin{keywords}") + 16:t.index(r"\end{keywords}")].strip()
kws = [k.strip() for k in kw.split(",")]
print("keywords:", len(kws), "alphabetical:", [k.lower() for k in kws] == sorted(k.lower() for k in kws))

cites = re.findall(r"\\cite\{([^}]*)\}", t)
order = []
for c in cites:
    for k in c.split(","):
        k = k.strip()
        if k not in order:
            order.append(k)
bib = re.findall(r"\\bibitem\{([^}]*)\}", t)
print("citation order matches bibliography order:", order == bib)
print("uncited:", [b for b in bib if b not in order], "missing:", [o for o in order if o not in bib])
years = re.findall(r"\\bibitem\{[^}]*\}[^(]*\((\d{4})\)", t)
print("reference years:", sorted(set(years)), "all 2018-2024:", all(2018 <= int(y) <= 2024 for y in years), len(years))
labels = set(re.findall(r"\\label\{([^}]*)\}", t))
refs = set(re.findall(r"\\ref\{([^}]*)\}", t))
print("undefined \\ref targets:", sorted(refs - labels))
for f in re.findall(r"\\includegraphics(?:\[[^\]]*\])?\{([^}]*)\}", t):
    print("figure file", f, "exists:", os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), f)))
