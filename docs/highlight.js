(() => {
  const keywords = new Set([
    "FN",
    "ret",
    "if",
    "elx",
    "else",
    "loop",
    "hold",
    "throw",
    "pass",
    "use",
    "REF",
    "FREE",
    "IS",
    "as",
  ]);

  const blueWords = new Set([
    "WEB",
    "REQ",
    "SQL",
    "OS",
    "FS",
    "PATH",
    "PROC",
    "IO",
    "BUF",
    "NUL",
  ]);
  const typeWords = new Set([
    "INT",
    "FLT",
    "STR",
    "BOL",
    "ARR",
    "MAP",
    "OBJ",
    "DYN",
    "TSK",
  ]);
  const constantWords = new Set(["true", "false", "INIT"]);
  const loopWords = new Set(["i", "e", "val"]);

  function escapeHtml(text) {
    return text
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;");
  }

  function peekNonSpace(text, index) {
    let i = index;
    while (i < text.length && /\s/.test(text[i])) i += 1;
    return text[i] || "";
  }

  function previousNonSpace(text, index) {
    let i = index - 1;
    while (i >= 0 && /\s/.test(text[i])) i -= 1;
    return text[i] || "";
  }

  function isIdentChar(ch) {
    return /[A-Za-z0-9_]/.test(ch || "");
  }

  function isShapeSeparator(text, index) {
    const rawBefore = text[index - 1] || "";
    const rawAfter = text[index + 1] || "";
    const before = previousNonSpace(text, index);
    const after = peekNonSpace(text, index + 1);
    const compact =
      /[0-9)\]]/.test(rawBefore) && /[A-Za-z0-9_(]/.test(rawAfter);
    const spaced =
      !isIdentChar(rawBefore) &&
      !isIdentChar(rawAfter) &&
      /[A-Za-z0-9_)\]]/.test(before) &&
      /[A-Za-z0-9_(]/.test(after);

    return compact || spaced;
  }

  function span(className, text) {
    return `<span class="${className}">${escapeHtml(text)}</span>`;
  }

  function highlightCaster(text) {
    let out = "";
    let i = 0;
    let expectFunctionName = false;

    while (i < text.length) {
      const ch = text[i];
      const next = text[i + 1] || "";

      if (ch === "/" && next === "/") {
        const end = text.indexOf("\n", i);
        const finish = end === -1 ? text.length : end;
        out += span("syntax-comment", text.slice(i, finish));
        i = finish;
        continue;
      }

      if (ch === "`") {
        const end = text.indexOf("`", i + 1);
        const finish = end === -1 ? text.length : end + 1;
        out += span("syntax-string", text.slice(i, finish));
        i = finish;
        continue;
      }

      if (ch === '"') {
        let finish = i + 1;
        while (finish < text.length) {
          if (text[finish] === "\\") {
            finish += 2;
            continue;
          }
          if (text[finish] === '"') {
            finish += 1;
            break;
          }
          finish += 1;
        }
        out += span("syntax-string", text.slice(i, finish));
        i = finish;
        continue;
      }

      if (/[0-9]/.test(ch)) {
        const start = i;
        i += 1;
        while (i < text.length && /[0-9]/.test(text[i])) i += 1;
        if (text[i] === "." && /[0-9]/.test(text[i + 1] || "")) {
          i += 1;
          while (i < text.length && /[0-9]/.test(text[i])) i += 1;
        }
        out += span("syntax-number", text.slice(start, i));
        continue;
      }

      if (ch === "x" && isShapeSeparator(text, i)) {
        out += span("syntax-operator", ch);
        i += 1;
        continue;
      }

      if (/[A-Za-z_]/.test(ch)) {
        const start = i;
        i += 1;
        while (i < text.length && /[A-Za-z0-9_]/.test(text[i])) i += 1;

        const word = text.slice(start, i);
        const after = peekNonSpace(text, i);
        const before = previousNonSpace(text, start);
        const isQualifiedPrefix = after === "." && /^[A-Z]/.test(word);
        const isQualifiedType = before === "." && /^[A-Z]/.test(word);
        const isMethod = before === "." && after === "(";
        const isCall = after === "(";

        if (blueWords.has(word) || isQualifiedPrefix) {
          out += span("syntax-blue", word);
        } else if (typeWords.has(word) || isQualifiedType) {
          out += span("syntax-type", word);
        } else if (constantWords.has(word)) {
          out += span("syntax-constant", word);
        } else if (keywords.has(word)) {
          out += span("syntax-keyword", word);
        } else if (loopWords.has(word)) {
          out += span("syntax-loop-var", word);
        } else if (expectFunctionName || isMethod || isCall) {
          out += span(isMethod ? "syntax-method" : "syntax-function", word);
        } else {
          out += escapeHtml(word);
        }

        expectFunctionName = word === "FN" && after !== "[";
        if (word !== "FN" && !/\s/.test(word)) {
          expectFunctionName = false;
        }
        continue;
      }

      const two = text.slice(i, i + 2);
      if (
        ["+=", "-=", "++", "==", "!=", "<=", ">=", "&&", "||", "->"].includes(
          two,
        )
      ) {
        out += span("syntax-operator", two);
        i += 2;
        continue;
      }

      if ("=+-*/%<>!&.*".includes(ch)) {
        out += span("syntax-operator", ch);
        i += 1;
        continue;
      }

      out += escapeHtml(ch);
      i += 1;
    }

    return out;
  }

  document.querySelectorAll("pre code").forEach((block) => {
    block.innerHTML = highlightCaster(block.textContent);
  });

  function initScrollSpy() {
    if (typeof window === "undefined") return;

    const links = Array.from(document.querySelectorAll('nav a[href^="#"]'));
    const sections = links
      .map((link) => document.querySelector(link.getAttribute("href")))
      .filter(Boolean);

    if (!links.length || !sections.length) return;

    const byId = new Map(
      links.map((link) => [link.getAttribute("href").slice(1), link]),
    );
    const caster = document.querySelector(".section-caster");
    const casterLabel = caster
      ? caster.querySelector(".section-caster-label")
      : null;
    let currentActiveId = "";

    function setActive(id) {
      if (id === currentActiveId) return;
      currentActiveId = id;

      links.forEach((link) => {
        const isActive = link === byId.get(id);
        link.classList.toggle("is-active", isActive);
        if (isActive) {
          link.setAttribute("aria-current", "true");
        } else {
          link.removeAttribute("aria-current");
        }
      });

      const activeLink = byId.get(id);
      if (caster && casterLabel && activeLink) {
        casterLabel.textContent = activeLink.textContent.trim();
        caster.classList.remove("is-casting");
        void caster.offsetWidth;
        caster.classList.add("is-casting");
      }
    }

    function updateActiveSection() {
      const offset = window.innerHeight < 760 ? 96 : 140;
      let activeId = sections[0].id;

      sections.forEach((section) => {
        if (section.getBoundingClientRect().top <= offset) {
          activeId = section.id;
        }
      });

      setActive(activeId);
    }

    window.addEventListener("scroll", updateActiveSection, { passive: true });
    window.addEventListener("resize", updateActiveSection);
    updateActiveSection();
  }

  initScrollSpy();

  function initConwayOutput() {
    if (typeof window === "undefined") return;

    const output = document.getElementById("conway-output");
    if (!output) return;

    const alive = "#";
    const dead = ".";
    let generation = 0;

    function initialGrid() {
      return Array.from({ length: 10 }, () =>
        Array.from({ length: 10 }, () => Math.random() > 0.68),
      );
    }

    let grid = initialGrid();

    function isAlive(source, row, col) {
      return Boolean(source[row] && source[row][col]);
    }

    function neighborCount(source, row, col) {
      let total = 0;

      for (let r = row - 1; r <= row + 1; r += 1) {
        for (let c = col - 1; c <= col + 1; c += 1) {
          if (r === row && c === col) continue;
          if (isAlive(source, r, c)) total += 1;
        }
      }

      return total;
    }

    function nextCell(source, row, col) {
      const currentlyAlive = isAlive(source, row, col);
      const neighbors = neighborCount(source, row, col);

      if (currentlyAlive && neighbors < 2) return false;
      if (currentlyAlive && neighbors > 3) return false;
      if (!currentlyAlive && neighbors === 3) return true;

      return currentlyAlive;
    }

    function step(source) {
      return source.map((row, rowIndex) =>
        row.map((_, colIndex) => nextCell(source, rowIndex, colIndex)),
      );
    }

    function render(source) {
      return source.map((row) =>
        row.map((cell) => (cell ? alive : dead)).join(""),
      );
    }

    function countAlive(source) {
      return source.reduce((sum, row) => sum + row.filter(Boolean).length, 0);
    }

    function paint() {
      output.textContent = [
        "$ caster conway.cast",
        "",
        `generation ${generation}`,
        ...render(grid),
        "",
        `alive ${countAlive(grid)}`,
        "",
        "looping forever... >",
      ].join("\n");

      grid = step(grid);
      generation += 1;
    }

    paint();
    window.setInterval(paint, 1200);
  }

  initConwayOutput();
})();
