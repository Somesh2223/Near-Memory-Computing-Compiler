# NEMO-C manuscript (IEEE Access format)

## Make the PDF on Overleaf

1. On overleaf.com, open **New Project → Templates**, search for **"IEEE Access LaTeX Template"**
   (the official one published by IEEE) and open it as a project. It supplies `ieeeaccess.cls`,
   the logo files and the fonts.
2. Upload `main.tex` and the whole `figures/` folder (or upload `NEMO-C_manuscript_overleaf.zip`
   and extract it). Keep the folder name `figures`.
3. In the left menu, set **Main document** to `main.tex`. The compiler is **pdfLaTeX**.
4. Press **Recompile**, then **Download PDF**.

If the template's own example file (usually `access.tex`) is still in the project, it can stay;
only the main document is compiled.

## Fill in before submitting

- First author's full name (`Smeehen` as given in the earlier draft), and the corresponding author's e-mail.
- **Author Contributions** section: who did which modules.
- **Acknowledgment**: state any AI-tool use as IEEE and VIT require.

## Where the numbers come from

- `../results/results.csv` (produced by `make run-all`)
- `paperdata.json` (produced by `collect_data.py`, run inside WSL from the `nemoc` folder)
- `make_figures.py` draws every graph in `figures/` from those files: `python make_figures.py`
- `check_manuscript.py` counts figures, equations, algorithms and references, and checks
  that the citations are numbered in order and all references are from 2018–2024.
