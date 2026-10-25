# KTPlace documentation

KTPlace places the cells of a chip design: it reads a netlist, finds a location for
every cell so that the wires are short and no region is over-full, and writes the
placement. A run goes through three stages, each a separate algorithm:

```mermaid
flowchart LR
    read[read design]:::io --> gp["global placement<br/>SimPL or NTUplace"]:::stage
    gp --> lg["legalization<br/>Abacus or multi-row"]:::stage
    lg --> dp["detailed placement<br/>FastDP"]:::stage
    dp --> out[placed.pl]:::io
    classDef io fill:#fef3c7,stroke:#d97706,color:#451a03
    classDef stage fill:#ccfbf1,stroke:#0f766e,color:#042f2e
    classDef found fill:#e0e7ff,stroke:#4f46e5,color:#1e1b4b
    classDef simpl fill:#fce7f3,stroke:#db2777,color:#500724
    classDef mov fill:#dbeafe,stroke:#2563eb,color:#172554
    classDef fixed fill:#e5e7eb,stroke:#374151,color:#111827
```

These pages explain the algorithms behind each stage from the ground up, for
readers who know basic calculus but no linear algebra and nothing about chip design.

## How the documentation is organized

<div class="grid cards" markdown>

- **[Foundations](foundations/README.md)**

    The background every algorithm guide relies on: the placement problem, the math
    toolkit, quadratic placement, solving large linear systems, net models. Read it
    once; the guides link back into it.

- **Global placement**

    One guide per global placer, starting with
    [SimPL](global-placement/simpl/README.md).

- **Legalization**

    One guide per legalizer: Abacus and multi-row legalization (planned).

- **Detailed placement**

    One guide per detailed placer: FastDP (planned).

- **[Glossary](glossary.md)**

    Every symbol and term on one page, and SimPL in ten lines.

- **[References](references.md)**

    The papers the guides follow, with links.

</div>

## Algorithm guides

| Stage | Algorithm | Guide |
| --- | --- | --- |
| Global placement | SimPL (Kim, Lee, Markov) | [SimPL](global-placement/simpl/README.md) |
| Global placement | NTUplace (Chen, Hsu, Jiang, Chang) | planned |
| Legalization | Abacus (Spindler, Schlichtmann, Johannes) | planned |
| Legalization | Multi-row legalization | planned |
| Detailed placement | FastDP (Pan, Viswanathan, Chu) | planned |

## Reading paths

- **New to placement:** [Foundations](foundations/README.md) chapters 1 to 5 in
  order, then any algorithm guide.
- **Know linear algebra and calculus:** Foundations chapters 1, 3 and 5, then the
  guide you need.
- **Just one algorithm:** open its guide; each chapter says which foundations it
  needs.

## Viewing these pages

The formulas are LaTeX math (`$…$`). They render on GitHub, on the documentation
site, and in the Markdown preview of VS Code or Cursor (`Ctrl+Shift+V`). Editors
that show Markdown as rich text without math support display raw `$` signs.

To preview the whole site locally, from the repository root:

```sh
make docs          # http://127.0.0.1:8000/KTPlace/, reloads on every save; Ctrl+C stops it
make docs-build    # the strict build CI runs, into site/
```

The first run installs the pinned MkDocs from `docs/requirements.txt` into `.venv/`.

## Adding a guide

Every algorithm guide has the same shape, so a reader who finished one knows how to
read the next:

- It lives in `<stage>/<algorithm>/` (`global-placement/`, `legalization/`,
  `detailed-placement/`).
- `README.md` gives a one-paragraph summary, where the algorithm sits in the flow,
  the foundations to read first, the paper, and the chapter list.
- Chapters are numbered files (`01-the-idea.md`, `02-…`), each one sitting long,
  with previous / contents / next links at the top and bottom and an
  "In the paper: §…" line under the title.
- Background that more than one algorithm needs goes into `foundations/`, not into
  the guide. New terms go into the [glossary](glossary.md), new papers into
  [references](references.md).
- The guide is added to `nav` in `mkdocs.yml`. `mkdocs build --strict` (run in CI)
  fails on any broken link or page missing from the navigation.
