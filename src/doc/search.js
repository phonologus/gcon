// The documentation's script (gcon doc): the search, headings matched as
// you type, from search-index.js (GCONDOC: [heading, url, page]).
(function () {
  var input = document.getElementById("search");
  var list = document.getElementById("results");
  var root = document.body.getAttribute("data-root") || "";
  if (!input || !list || typeof GCONDOC === "undefined") return;
  var current = -1;

  function show() {
    var q = input.value.trim().toLowerCase();
    list.textContent = "";
    current = -1;
    if (!q) { list.hidden = true; return; }
    var hits = [];
    for (var i = 0; i < GCONDOC.length; i++) {
      var h = GCONDOC[i], t = h[0].toLowerCase(), at = t.indexOf(q);
      if (at < 0) continue;
      hits.push([at === 0 ? 0 : 1, t.length, h]);
    }
    hits.sort(function (a, b) { return a[0] - b[0] || a[1] - b[1]; });
    hits = hits.slice(0, 20);
    if (!hits.length) { list.hidden = true; return; }
    hits.forEach(function (x) {
      var li = document.createElement("li"), a = document.createElement("a");
      a.href = root + x[2][1];
      a.textContent = x[2][0];
      if (x[2][2] !== x[2][0]) {
        var p = document.createElement("span");
        p.className = "page";
        p.textContent = x[2][2];
        a.appendChild(p);
      }
      li.appendChild(a);
      list.appendChild(li);
    });
    list.hidden = false;
  }

  function move(d) {
    var links = list.querySelectorAll("a");
    if (!links.length) return;
    if (current >= 0) links[current].classList.remove("current");
    current = (current + d + links.length) % links.length;
    links[current].classList.add("current");
    links[current].scrollIntoView({ block: "nearest" });
  }

  input.addEventListener("input", show);
  input.addEventListener("keydown", function (e) {
    if (e.key === "ArrowDown") { move(1); e.preventDefault(); }
    else if (e.key === "ArrowUp") { move(-1); e.preventDefault(); }
    else if (e.key === "Enter") {
      var links = list.querySelectorAll("a");
      if (links.length) location.href = links[current >= 0 ? current : 0].href;
    } else if (e.key === "Escape") { input.value = ""; show(); input.blur(); }
  });
  document.addEventListener("keydown", function (e) {
    if (e.key === "/" && document.activeElement !== input) { input.focus(); e.preventDefault(); }
  });
  document.addEventListener("click", function (e) {
    if (e.target !== input && !list.contains(e.target)) list.hidden = true;
  });
})();

// The examples (Icon programs, which declare main): Copy everywhere;
// Run, Edit and Reset where the page comes from gcon doc's own server,
// which gives its pages a secret (data-run) to POST a program to /run
// with. Run becomes Stop while the program runs; stopping aborts the
// request, and the server kills the program.
(function () {
  var token = document.body.getAttribute("data-run");
  var root = document.body.getAttribute("data-root") || "";
  var codes = document.querySelectorAll("pre > code.language-icon");

  function button(bar, label, title, f) {
    var b = document.createElement("button");
    b.type = "button";
    b.textContent = label;
    b.title = title;
    b.addEventListener("click", f);
    bar.appendChild(b);
    return b;
  }

  Array.prototype.forEach.call(codes, function (code) {
    var original = code.textContent;
    if (!/^procedure main\b/m.test(original)) return;
    var pre = code.parentNode;
    var box = document.createElement("div");
    box.className = "example";
    pre.parentNode.insertBefore(box, pre);
    box.appendChild(pre);
    var bar = document.createElement("div");
    bar.className = "example-bar";
    box.appendChild(bar);
    var editor = null, out = null, running = null;

    function source() { return editor ? editor.value : code.textContent; }

    function show(text, cls) {
      if (!out) {
        out = document.createElement("pre");
        out.className = "example-output";
        box.appendChild(out);
      }
      out.textContent = "";
      if (text) out.appendChild(document.createTextNode(text));
      if (cls) {
        var s = document.createElement("span");
        s.className = "status";
        s.textContent = cls;
        out.appendChild(s);
      }
    }

    function edit() {
      if (editor) { editor.focus(); return; }
      editor = document.createElement("textarea");
      editor.className = "example-editor";
      editor.spellcheck = false;
      editor.setAttribute("autocapitalize", "off");
      editor.value = code.textContent;
      var fit = function () { editor.style.height = "auto"; editor.style.height = editor.scrollHeight + 2 + "px"; };
      editor.addEventListener("input", fit);
      editor.addEventListener("keydown", function (e) {
        if (e.key === "Enter" && (e.metaKey || e.ctrlKey) && runb) { run(); e.preventDefault(); }
        else if (e.key === "Tab" && !e.shiftKey) {
          // an indent, Icon's three spaces, rather than leaving the editor
          if (!document.execCommand || !document.execCommand("insertText", false, "   "))
            editor.setRangeText("   ", editor.selectionStart, editor.selectionEnd, "end");
          e.preventDefault();
        } else if (e.key === "Escape") editor.blur();
      });
      box.insertBefore(editor, pre);
      pre.hidden = true;
      fit();
      editor.focus();
    }

    function reset() {
      if (running) { var c = running; idle(); c.abort(); }
      if (editor) { editor.remove(); editor = null; }
      pre.hidden = false;
      code.textContent = original;
      if (out) { out.remove(); out = null; }
    }

    function idle() {
      running = null;
      runb.textContent = "Run";
      runb.title = "Run the program (gcon run)";
    }

    function run() {
      if (running) { running.abort(); return; }
      var ctl = running = new AbortController();
      runb.textContent = "Stop";
      runb.title = "Stop the program";
      show("", "Running…");
      fetch(root + "run", {
        method: "POST",
        headers: { "X-Gcon-Token": token, "Content-Type": "text/plain; charset=utf-8" },
        body: source(),
        signal: ctl.signal
      }).then(function (r) {
        var status = r.headers.get("X-Gcon-Status");
        return r.text().then(function (t) {
          if (running !== ctl) return; // reset meanwhile
          if (!r.ok) show(t, "The server refused: " + r.status);
          else show(t, status && status !== "0" ? "exit status " + status : (t ? "" : "No output."));
        });
      }).catch(function (e) {
        if (running !== ctl) return;
        show("", ctl.signal.aborted ? "Stopped." : "Cannot reach gcon doc's server: is it still running?");
      }).then(function () {
        if (running === ctl) idle();
      });
    }

    var runb = null;
    if (token) {
      runb = button(bar, "Run", "Run the program (gcon run)", run);
      button(bar, "Edit", "Edit the program", edit);
      button(bar, "Reset", "Back to the example as written", reset);
    }
    var copyb = button(bar, "Copy", "Copy the program", function () {
      var done = function () {
        copyb.textContent = "Copied";
        setTimeout(function () { copyb.textContent = "Copy"; }, 1200);
      };
      if (navigator.clipboard && window.isSecureContext)
        navigator.clipboard.writeText(source()).then(done);
      else {
        var t = document.createElement("textarea");
        t.value = source();
        document.body.appendChild(t);
        t.select();
        document.execCommand("copy");
        t.remove();
        done();
      }
    });
  });
})();
