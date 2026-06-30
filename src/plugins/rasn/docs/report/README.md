# rASN Technical Report

This directory contains the ACM-style technical report for rASN. Update
`main.tex` and `main.bib` whenever a new rASN or CodePilot refinement
changes the architecture, implementation, evaluation plan, or limitations.

Build in WSL or another TeX Live environment:

```sh
bash build.sh
```

The script cleans generated artifacts and invokes `latexmk -pdf` on `main.tex`.
If `latexmk` is not installed, use the explicit LaTeX/BibTeX sequence:

```sh
pdflatex -interaction=nonstopmode -halt-on-error main.tex
bibtex main
pdflatex -interaction=nonstopmode -halt-on-error main.tex
pdflatex -interaction=nonstopmode -halt-on-error main.tex
```
