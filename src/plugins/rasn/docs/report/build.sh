#!/usr/bin/env bash

latexmk -C
rm -f main.aux main.bbl main.blg main.log main.out main.out.pyg main.pyg main.fdb_latexmk  main.pdf
rm -fr _minted-main

latexmk -pdf -shell-escape main.tex
